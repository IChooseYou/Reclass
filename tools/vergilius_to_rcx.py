#!/usr/bin/env python3
"""
Fetch kernel structs from Vergilius Project and generate .rcx (JSON) file.

Usage:
    python vergilius_to_rcx.py -o output.rcx _EPROCESS _KPROCESS _MMPFN ...
    python vergilius_to_rcx.py --preset 25h2 -o output.rcx

Fetches struct definitions from vergiliusproject.com, parses the C-like
syntax, and converts to Reclass 2027 native JSON format (.rcx).
"""

import argparse
import json
import re
import sys
import urllib.request
import urllib.error
from html.parser import HTMLParser
import time

# ── Windows kernel type → (RCX kind, byte size) ──

TYPE_MAP = {
    # Unsigned integers
    'UCHAR':        ('UInt8',  1),
    'UINT8':        ('UInt8',  1),
    'BOOLEAN':      ('UInt8',  1),
    'USHORT':       ('UInt16', 2),
    'UINT16':       ('UInt16', 2),
    'WCHAR':        ('UInt16', 2),
    'ULONG':        ('UInt32', 4),
    'UINT32':       ('UInt32', 4),
    'ULONGLONG':    ('UInt64', 8),
    'UINT64':       ('UInt64', 8),
    'ULONG_PTR':    ('UInt64', 8),
    'SIZE_T':       ('UInt64', 8),
    # Signed integers
    'CHAR':         ('Int8',   1),
    'INT8':         ('Int8',   1),
    'SHORT':        ('Int16',  2),
    'INT16':        ('Int16',  2),
    'LONG':         ('Int32',  4),
    'INT32':        ('Int32',  4),
    'LONGLONG':     ('Int64',  8),
    'INT64':        ('Int64',  8),
    'LONG_PTR':     ('Int64',  8),
    # Floating point
    'float':        ('Float',  4),
    'double':       ('Double', 8),
    # Pointer-like
    'PVOID':        ('Pointer64', 8),
    'HANDLE':       ('Pointer64', 8),
    'PCHAR':        ('Pointer64', 8),
    'PWCHAR':       ('Pointer64', 8),
    'PUCHAR':       ('Pointer64', 8),
    'PULONG':       ('Pointer64', 8),
    'PLONG':        ('Pointer64', 8),
    'PUSHORT':      ('Pointer64', 8),
    'PULONGLONG':   ('Pointer64', 8),
    'PVOID64':      ('Pointer64', 8),
}

# ── HTML parser to extract <pre> content ──

class PreExtractor(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_pre = False
        self.pre_content = []
        self.result = None

    def handle_starttag(self, tag, attrs):
        if tag == 'pre':
            self.in_pre = True
            self.pre_content = []

    def handle_endtag(self, tag):
        if tag == 'pre' and self.in_pre:
            self.in_pre = False
            if self.result is None:
                self.result = ''.join(self.pre_content)

    def handle_data(self, data):
        if self.in_pre:
            self.pre_content.append(data)

    def handle_entityref(self, name):
        if self.in_pre:
            self.pre_content.append(f'&{name};')

    def handle_charref(self, name):
        if self.in_pre:
            self.pre_content.append(f'&#{name};')


# ── ID allocator ──

class IdAlloc:
    def __init__(self, start=100):
        self.next = start

    def alloc(self):
        n = self.next
        self.next += 1
        return n


# ── Fetch a struct definition from Vergilius ──

BASE_URL = 'https://www.vergiliusproject.com/kernels/x64/windows-11/25h2'

def fetch_struct_text(name):
    """Fetch the C struct definition text for a given type name."""
    url = f'{BASE_URL}/{name}'
    req = urllib.request.Request(url, headers={
        'User-Agent': 'Mozilla/5.0 (Reclass2027 struct importer)',
    })
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            html = resp.read().decode('utf-8', errors='replace')
    except urllib.error.HTTPError as e:
        print(f'  ERROR: HTTP {e.code} fetching {name}', file=sys.stderr)
        return None
    except Exception as e:
        print(f'  ERROR: {e} fetching {name}', file=sys.stderr)
        return None

    parser = PreExtractor()
    parser.feed(html)
    return parser.result


# ── Vergilius text parser ──

# Regex for offset comment at end of line: //0xNN
RE_OFFSET = re.compile(r'//0x([0-9a-fA-F]+)\s*$')

# Regex for size comment: //0xNN bytes (sizeof)
RE_SIZEOF = re.compile(r'//0x([0-9a-fA-F]+)\s+bytes\s+\(sizeof\)')

# Regex for a field line:  TYPE fieldname;  //0xNN
# Handles: volatile, struct/union prefix, pointers (*), arrays ([N]), bitfields (:N)
RE_FIELD = re.compile(
    r'^\s+'                         # leading whitespace
    r'(?:volatile\s+)?'             # optional volatile
    r'(?:(struct|union|enum)\s+)?'  # optional keyword
    r'(\w+)'                        # type name (or keyword target)
    r'(\*?)'                        # optional pointer
    r'\s+'
    r'(?:volatile\s+)?'             # volatile can appear here too
    r'(\*?)'                        # pointer can be here (struct _X* volatile Field)
    r'(\w+)'                        # field name
    r'(?:\[(\d+)\])?'              # optional array [N]
    r'(?::(\d+))?'                  # optional bitfield :N
    r'\s*;'                         # semicolon
)

def parse_offset(line):
    """Extract hex offset from //0xNN comment."""
    m = RE_OFFSET.search(line)
    return int(m.group(1), 16) if m else None

def parse_struct_size(text):
    """Extract struct size from //0xNN bytes (sizeof) comment."""
    m = RE_SIZEOF.search(text)
    return int(m.group(1), 16) if m else 0


def parse_vergilius(text, ids, struct_registry):
    """
    Parse Vergilius C-like struct text and return list of RCX nodes.

    struct_registry: dict mapping type_name → node_id (built up across calls)
    Returns (nodes, root_id, struct_size)
    """
    lines = text.strip().split('\n')
    nodes = []
    pos = [0]  # mutable for closure

    def peek():
        return lines[pos[0]].rstrip() if pos[0] < len(lines) else None

    def advance():
        line = lines[pos[0]].rstrip()
        pos[0] += 1
        return line

    def skip_blank():
        while pos[0] < len(lines) and not lines[pos[0]].strip():
            pos[0] += 1

    # Parse top-level: optional size comment, struct/union keyword, name, body
    skip_blank()

    struct_size = 0
    line = peek()
    if line and RE_SIZEOF.search(line):
        struct_size = parse_struct_size(line)
        advance()

    # struct/union _NAME
    skip_blank()
    line = advance()
    m = re.match(r'\s*(struct|union)\s+(\w+)', line)
    if not m:
        return nodes, 0, 0

    root_keyword = m.group(1)
    root_name = m.group(2)

    # Opening brace
    skip_blank()
    line = peek()
    if line and line.strip() == '{':
        advance()

    # Create root node
    root_id = ids.alloc()
    root_node = {
        'id': str(root_id),
        'kind': 'Struct',
        'name': root_name.lstrip('_').lower(),
        'structTypeName': root_name,
        'offset': 0,
        'parentId': '0',
        'refId': '0',
        'collapsed': True,
    }
    if root_keyword == 'union':
        root_node['classKeyword'] = 'union'
    nodes.append(root_node)
    struct_registry[root_name] = root_id

    # Parse body
    parse_body(lines, pos, ids, nodes, root_id, struct_registry)

    # Fix anonymous containers whose offset peek failed (first child was
    # a nested struct/union, not a field line with an offset comment).
    # Set their offset to the minimum child offset.
    fixup_anonymous_offsets(nodes)

    # Convert bitfield children into proper bitfield containers
    postprocess_bitfields(nodes)

    # Convert absolute offsets to parent-relative
    convert_to_relative_offsets(nodes)

    return nodes, root_id, struct_size


def parse_body(lines, pos, ids, nodes, parent_id, struct_registry):
    """Parse fields inside { ... }; recursively."""
    while pos[0] < len(lines):
        line = lines[pos[0]].rstrip()
        stripped = line.strip()

        # End of block
        if stripped.startswith('}'):
            pos[0] += 1
            return stripped  # caller checks for "} name;" vs "};"

        # Blank line
        if not stripped:
            pos[0] += 1
            continue

        # Nested struct/union
        m = re.match(r'\s*(struct|union)\s*$', stripped)
        if m:
            keyword = m.group(1)
            pos[0] += 1

            # Expect opening brace
            while pos[0] < len(lines):
                brace_line = lines[pos[0]].strip()
                if brace_line == '{':
                    pos[0] += 1
                    break
                if not brace_line:
                    pos[0] += 1
                    continue
                break

            # Create anonymous struct/union node
            anon_id = ids.alloc()
            # We don't know the offset yet; peek at first child
            anon_offset = 0
            if pos[0] < len(lines):
                off = parse_offset(lines[pos[0]])
                if off is not None:
                    anon_offset = off

            anon_node = {
                'id': str(anon_id),
                'kind': 'Struct',
                'name': '',
                'classKeyword': keyword,
                'offset': anon_offset,
                'parentId': str(parent_id),
                'refId': '0',
                'collapsed': False,
            }
            nodes.append(anon_node)

            # Parse body recursively
            close_line = parse_body(lines, pos, ids, nodes, anon_id, struct_registry)

            # Check for name after closing brace: "} name;" or "};"
            if close_line:
                cm = re.match(r'\}\s*(\w+)\s*;', close_line)
                if cm:
                    anon_node['name'] = cm.group(1)
                # Get offset from close line
                off = parse_offset(close_line)
                if off is not None:
                    anon_node['offset'] = off

            continue

        # Regular field line
        offset = parse_offset(line)
        if offset is None:
            pos[0] += 1
            continue

        # Parse field
        node = parse_field_line(stripped, offset, parent_id, ids, struct_registry)
        if node:
            nodes.append(node)

        pos[0] += 1


def parse_field_line(line, offset, parent_id, ids, struct_registry):
    """Parse a single field line into an RCX node."""
    # Strip offset comment
    line = RE_OFFSET.sub('', line).strip().rstrip(';').strip()

    # Remove volatile
    line = re.sub(r'\bvolatile\b', '', line).strip()
    line = re.sub(r'\s+', ' ', line)

    # Check for struct/union keyword prefix
    keyword = None
    m = re.match(r'^(struct|union|enum)\s+(.+)', line)
    if m:
        keyword = m.group(1)
        line = m.group(2)

    # Check for pointer(s)
    is_pointer = False
    if '*' in line:
        is_pointer = True
        # "TYPE* name" or "TYPE *name" or "_NAME* name"
        parts = line.replace('*', '* ').split()
        # Find the type and name
        type_parts = []
        field_name = None
        for i, p in enumerate(parts):
            if p.endswith('*'):
                type_parts.append(p.rstrip('*'))
                is_pointer = True
            elif i == len(parts) - 1:
                field_name = p
            else:
                type_parts.append(p)
        type_name = ' '.join(tp for tp in type_parts if tp)
        if not field_name:
            return None
    else:
        # "TYPE name" or "TYPE name[N]" or "TYPE name:N"
        parts = line.split()
        if len(parts) < 2:
            return None
        type_name = parts[0]
        rest = ' '.join(parts[1:])

        # Check for array
        am = re.match(r'(\w+)\[(\d+)\]', rest)
        # Check for bitfield
        bm = re.match(r'(\w+):(\d+)', rest)

        if am:
            field_name = am.group(1)
            array_len = int(am.group(2))
            return make_array_node(type_name, keyword, field_name, array_len,
                                   offset, parent_id, ids, struct_registry)
        elif bm:
            field_name = bm.group(1)
            bitwidth = int(bm.group(2))
            return make_bitfield_node(type_name, keyword, field_name, bitwidth,
                                      offset, parent_id, ids)
        else:
            field_name = parts[-1]

    # Pointer field
    if is_pointer:
        node_id = ids.alloc()
        node = {
            'id': str(node_id),
            'kind': 'Pointer64',
            'name': field_name,
            'offset': offset,
            'parentId': str(parent_id),
            'collapsed': True,
        }
        # If it points to a known struct, set refId
        if type_name in struct_registry:
            node['refId'] = str(struct_registry[type_name])
        elif keyword in ('struct', 'union') and type_name:
            # Will be resolved later
            node['_pending_ref'] = type_name
            node['refId'] = '0'
        else:
            node['refId'] = '0'
        return node

    # Embedded struct/union
    if keyword in ('struct', 'union'):
        node_id = ids.alloc()
        node = {
            'id': str(node_id),
            'kind': 'Struct',
            'name': field_name,
            'structTypeName': type_name,
            'offset': offset,
            'parentId': str(parent_id),
            'refId': '0',
            'collapsed': True,
        }
        if keyword == 'union':
            node['classKeyword'] = 'union'
        # Link to existing definition
        if type_name in struct_registry:
            node['refId'] = str(struct_registry[type_name])
        else:
            node['_pending_ref'] = type_name
        return node

    # Primitive type
    kind, size = TYPE_MAP.get(type_name, (None, None))
    if kind is None:
        # Unknown type — treat as Hex64 (8 bytes, common for x64)
        kind = 'Hex64'

    node_id = ids.alloc()
    return {
        'id': str(node_id),
        'kind': kind,
        'name': field_name,
        'offset': offset,
        'parentId': str(parent_id),
    }


def make_array_node(type_name, keyword, field_name, array_len, offset,
                     parent_id, ids, struct_registry):
    """Create a primitive or struct array node."""
    kind, elem_size = TYPE_MAP.get(type_name, (None, None))
    node_id = ids.alloc()

    if kind and keyword is None:
        # Primitive array: kind=Array, elementKind=primitive type
        return {
            'id': str(node_id),
            'kind': 'Array',
            'name': field_name,
            'offset': offset,
            'parentId': str(parent_id),
            'elementKind': kind,
            'arrayLen': array_len,
        }
    else:
        # Struct/union array: kind=Array, elementKind=Struct
        node = {
            'id': str(node_id),
            'kind': 'Array',
            'name': field_name,
            'offset': offset,
            'parentId': str(parent_id),
            'elementKind': 'Struct',
            'arrayLen': array_len,
        }
        if type_name:
            node['structTypeName'] = type_name
            if type_name in struct_registry:
                node['refId'] = str(struct_registry[type_name])
            else:
                node['_pending_ref'] = type_name
        return node


def make_bitfield_node(type_name, keyword, field_name, bitwidth, offset,
                        parent_id, ids):
    """Create a bitfield node — stored as Hex of the underlying type size."""
    kind, size = TYPE_MAP.get(type_name, ('Hex32', 4))
    # Map to hex kind for bitfields
    hex_kind = {1: 'Hex8', 2: 'Hex16', 4: 'Hex32', 8: 'Hex64'}.get(size, 'Hex32')

    node_id = ids.alloc()
    return {
        'id': str(node_id),
        'kind': hex_kind,
        'name': f'{field_name}:{bitwidth}',
        'offset': offset,
        'parentId': str(parent_id),
    }


def fixup_anonymous_offsets(nodes):
    """Fix anonymous struct/union nodes whose offset peek failed.

    When the first child of an anonymous container is another nested
    struct/union (not a field line), the parser can't peek at an offset
    comment and defaults to 0. Fix by setting the container's offset to
    the minimum offset among its direct children.
    """
    children_of = {}
    for node in nodes:
        pid = node.get('parentId', '0')
        children_of.setdefault(pid, []).append(node)

    for node in nodes:
        if node.get('kind') != 'Struct':
            continue
        if node.get('parentId', '0') == '0':
            continue
        # Only fix containers that still have offset 0 (the default from failed peek)
        if node.get('offset', 0) != 0:
            continue
        kids = children_of.get(node['id'], [])
        if not kids:
            continue
        kid_offsets = [k.get('offset', 0) for k in kids]
        min_off = min(kid_offsets)
        if min_off > 0:
            node['offset'] = min_off


def postprocess_bitfields(nodes):
    """
    Convert anonymous structs whose children are ALL bitfield Hex nodes
    into proper bitfield containers with bitfieldMembers array.

    Bitfield children are identified by having ':' in their name (e.g. "Absolute:1").
    The parent becomes kind=Struct, classKeyword=bitfield, elementKind=Hex8/16/32/64,
    and all child nodes are removed from the list.
    """
    # Build parent→children index
    children_of = {}
    for node in nodes:
        pid = node.get('parentId', '0')
        children_of.setdefault(pid, []).append(node)

    ids_to_remove = set()

    for node in nodes:
        # Process struct nodes (not unions, not already bitfields, not named types)
        if node.get('kind') != 'Struct':
            continue
        if node.get('classKeyword') in ('union', 'bitfield'):
            continue
        if node.get('structTypeName', ''):
            continue

        nid = node['id']
        kids = children_of.get(nid, [])
        if not kids:
            continue

        # Check if ALL children are Hex nodes with ':' in name
        all_bitfield = True
        for kid in kids:
            kid_kind = kid.get('kind', '')
            kid_name = kid.get('name', '')
            if not kid_kind.startswith('Hex') or ':' not in kid_name:
                all_bitfield = False
                break

        if not all_bitfield:
            continue

        # Determine container elementKind from children's hex kind
        element_kind = kids[0].get('kind', 'Hex32')

        # Build bitfieldMembers array
        members = []
        bit_offset = 0
        for kid in kids:
            kid_name = kid.get('name', '')
            # Parse "FieldName:Width"
            parts = kid_name.rsplit(':', 1)
            if len(parts) != 2:
                continue
            fname, width_str = parts
            bit_width = int(width_str)
            members.append({
                'name': fname,
                'bitOffset': bit_offset,
                'bitWidth': bit_width,
            })
            bit_offset += bit_width

        # Convert parent to bitfield container
        node['classKeyword'] = 'bitfield'
        node['elementKind'] = element_kind
        node['bitfieldMembers'] = members
        # Use offset from first child (they all share same byte offset)
        if kids:
            node['offset'] = kids[0].get('offset', node.get('offset', 0))
        # Remove fields not needed on bitfield containers
        node.pop('refId', None)
        node.pop('collapsed', None)

        # Mark children for removal
        for kid in kids:
            ids_to_remove.add(kid['id'])

    # Remove bitfield children from node list
    if ids_to_remove:
        nodes[:] = [n for n in nodes if n['id'] not in ids_to_remove]


def convert_to_relative_offsets(nodes):
    """Convert absolute offsets (from struct root) to parent-relative offsets.

    Vergilius provides absolute offsets from the struct root in //0xNN comments,
    but the RCX data model expects offsets relative to the parent node.
    """
    abs_off = {n['id']: n.get('offset', 0) for n in nodes}
    for node in nodes:
        pid = node.get('parentId', '0')
        if pid == '0':
            continue
        if pid in abs_off:
            node['offset'] = node.get('offset', 0) - abs_off[pid]


def resolve_pending_refs(all_nodes, struct_registry):
    """Resolve _pending_ref fields to actual refIds."""
    for node in all_nodes:
        ref_name = node.pop('_pending_ref', None)
        if ref_name and ref_name in struct_registry:
            node['refId'] = str(struct_registry[ref_name])


def build_rcx(all_nodes, base_address='FFFFF80000000000'):
    """Build the final .rcx JSON structure."""
    max_id = max(int(n['id']) for n in all_nodes) if all_nodes else 100
    return {
        'baseAddress': base_address,
        'nextId': str(max_id + 100),
        'nodes': all_nodes,
    }


# ── Curated struct sets ──

PRESET_25H2 = [
    # Fundamental
    '_LIST_ENTRY',
    '_UNICODE_STRING',
    '_LARGE_INTEGER',
    '_EX_PUSH_LOCK',
    '_EX_FAST_REF',
    '_DISPATCHER_HEADER',
    # Process / Thread
    '_EPROCESS',
    '_KPROCESS',
    '_ETHREAD',
    '_KTHREAD',
    '_PEB',
    '_TEB',
    '_KAPC_STATE',
    # Memory
    '_MMPFN',
    '_MMPTE',
    '_MMVAD',
    '_MMVAD_SHORT',
    '_MDL',
    '_CONTROL_AREA',
    # Objects
    '_OBJECT_HEADER',
    '_OBJECT_TYPE',
    '_HANDLE_TABLE',
    '_HANDLE_TABLE_ENTRY',
    # I/O
    '_DEVICE_OBJECT',
    '_DRIVER_OBJECT',
    '_FILE_OBJECT',
    '_IRP',
    # Misc
    '_KPCR',
    '_KPRCB',
    '_CONTEXT',
]


def scrape_all_struct_names():
    """Scrape all struct names from the Vergilius 25H2 index page."""
    class LinkExtractor(HTMLParser):
        def __init__(self):
            super().__init__()
            self.names = []
            self.base = '/kernels/x64/windows-11/25h2/'
        def handle_starttag(self, tag, attrs):
            if tag == 'a':
                for k, v in attrs:
                    if k == 'href' and v and v.startswith(self.base):
                        name = v[len(self.base):].strip('/')
                        if name and '/' not in name:
                            self.names.append(name)

    print('Scraping struct index from Vergilius...', flush=True)
    req = urllib.request.Request(BASE_URL,
        headers={'User-Agent': 'Mozilla/5.0 (Reclass2027 struct importer)'})
    with urllib.request.urlopen(req, timeout=30) as resp:
        html = resp.read().decode('utf-8', errors='replace')

    p = LinkExtractor()
    p.feed(html)
    seen = set()
    names = []
    for n in p.names:
        if n not in seen:
            seen.add(n)
            names.append(n)
    print(f'Found {len(names)} structs')
    return names


def main():
    parser = argparse.ArgumentParser(
        description='Fetch Vergilius structs and generate .rcx file')
    parser.add_argument('structs', nargs='*', help='Struct names (e.g. _EPROCESS)')
    parser.add_argument('-o', '--output', default='Vergilius_25H2.rcx',
                        help='Output .rcx file path')
    parser.add_argument('--preset', choices=['25h2'],
                        help='Use preset struct list')
    parser.add_argument('--from-file', metavar='FILE',
                        help='Read struct names from file (one per line)')
    parser.add_argument('--scrape-all', action='store_true',
                        help='Scrape all struct names from the Vergilius page')
    parser.add_argument('--delay', type=float, default=1.0,
                        help='Delay between HTTP requests (seconds)')
    parser.add_argument('--base', default='FFFFF80000000000',
                        help='Base address (hex string)')
    args = parser.parse_args()

    struct_names = args.structs
    if args.preset == '25h2':
        struct_names = PRESET_25H2
    if args.from_file:
        with open(args.from_file) as f:
            struct_names = [line.strip() for line in f if line.strip()]
    if args.scrape_all:
        struct_names = scrape_all_struct_names()
    if not struct_names:
        parser.error('Specify struct names or use --preset / --from-file / --scrape-all')

    ids = IdAlloc(100)
    struct_registry = {}  # type_name → node_id
    all_nodes = []
    failed = []

    total = len(struct_names)
    for i, name in enumerate(struct_names):
        print(f'[{i+1}/{total}] Fetching {name}...', end=' ', flush=True)

        text = fetch_struct_text(name)
        if not text:
            print('FAILED')
            failed.append(name)
            continue

        struct_nodes, root_id, struct_size = parse_vergilius(text, ids, struct_registry)
        if not struct_nodes:
            print('PARSE ERROR')
            failed.append(name)
            continue

        all_nodes.extend(struct_nodes)
        field_count = len(struct_nodes) - 1
        print(f'OK  ({field_count} fields, 0x{struct_size:X} bytes)')

        if i < total - 1:
            time.sleep(args.delay)

    # Resolve cross-references
    resolve_pending_refs(all_nodes, struct_registry)

    # Build and write .rcx
    rcx = build_rcx(all_nodes, args.base)

    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(rcx, f, indent=4, ensure_ascii=False)

    print(f'\nWrote {args.output}')
    print(f'  {len(struct_registry)} structs, {len(all_nodes)} total nodes')
    if failed:
        print(f'  Failed: {", ".join(failed)}')


if __name__ == '__main__':
    main()
