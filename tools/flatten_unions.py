"""
Remove root-level unions from RCX files.
- Unions with refId references: inline the union's subtree into each referencing node
- Unions with no references: delete entirely
- All direct children of inlined unions get offset=0 (union semantics)

RCX JSON uses string IDs throughout (id, parentId, refId are all strings or 0).
"""
import json
import copy


def sid(val):
    """Normalize to string ID for comparison."""
    return str(val) if val else '0'


def collect_subtree(nodes_by_parent, root_id):
    """Collect all descendants of root_id (not including root_id itself)."""
    result = []
    stack = [sid(root_id)]
    while stack:
        pid = stack.pop()
        children = nodes_by_parent.get(pid, [])
        for c in children:
            result.append(c)
            stack.append(sid(c['id']))
    return result


def process_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        data = json.load(f)

    nodes = data.get('nodes', [])
    if not nodes:
        print(f"  {filepath}: no nodes, skipping")
        return False

    # Build index by parent
    nodes_by_parent = {}
    for n in nodes:
        pid = sid(n.get('parentId', 0))
        nodes_by_parent.setdefault(pid, []).append(n)

    # Find root-level unions
    root_unions = [n for n in nodes if sid(n.get('parentId', 0)) == '0'
                   and n.get('classKeyword') == 'union']

    if not root_unions:
        print(f"  {filepath}: no root-level unions")
        return False

    # Find max ID for generating new unique IDs
    max_id = max(int(n['id']) for n in nodes)
    next_id = max_id + 1

    # Track which node IDs to remove (root unions + their subtrees)
    ids_to_remove = set()
    # Track new nodes to add
    new_nodes = []
    # Track modifications to existing nodes
    nodes_to_modify = {}  # str(id) -> dict of changes

    stats_inlined = 0
    stats_deleted = 0

    for u in root_unions:
        uid = sid(u['id'])

        # Collect union's full subtree
        subtree = collect_subtree(nodes_by_parent, u['id'])

        # Mark union and subtree for removal
        ids_to_remove.add(sid(u['id']))
        for s in subtree:
            ids_to_remove.add(sid(s['id']))

        # Find all nodes that reference this union via refId,
        # excluding nodes within the union's own subtree (self-references)
        subtree_ids = {sid(s['id']) for s in subtree}
        subtree_ids.add(uid)
        refs = [n for n in nodes if sid(n.get('refId', 0)) == uid and uid != '0'
                and sid(n['id']) not in subtree_ids]

        if not refs:
            stats_deleted += 1
            continue

        stats_inlined += 1

        for ref_node in refs:
            ref_nid = sid(ref_node['id'])

            # Modify the referencing node: clear refId, set classKeyword=union
            nodes_to_modify[ref_nid] = {
                'refId': 0,
                'classKeyword': 'union'
            }

            # Build old_id -> new_id mapping
            id_map = {}
            id_map[uid] = ref_nid  # union root -> referencing node

            for s in subtree:
                id_map[sid(s['id'])] = str(next_id)
                next_id += 1

            # Create new nodes from subtree
            for s in subtree:
                new_node = copy.deepcopy(s)
                old_sid = sid(s['id'])
                old_parent = sid(s.get('parentId', 0))

                new_node['id'] = id_map[old_sid]
                new_node['parentId'] = id_map.get(old_parent, old_parent)

                # Direct children of union: set offset=0 (union semantics)
                if old_parent == uid:
                    new_node['offset'] = 0

                new_nodes.append(new_node)

    # Fix cross-references: inlined nodes that reference other removed unions.
    # For each such node, inline the referenced union's subtree as its children too.
    # Build a lookup of removed unions by their original id
    union_subtrees = {}  # uid -> list of original subtree nodes
    for u in root_unions:
        uid = sid(u['id'])
        union_subtrees[uid] = collect_subtree(nodes_by_parent, u['id'])

    # Fix cross-references in new nodes: if a new node's refId points to a
    # removed union, inline that union's subtree. But for self-referencing
    # types (e.g. linked list pointers), just clear refId to avoid infinite
    # recursion — the pointer type info is preserved in structTypeName.
    all_new = list(new_nodes)
    seen_expansions = set()  # track (node_id, ref_uid) to prevent infinite loops
    max_passes = 5
    for pass_num in range(max_passes):
        cross_refs = [n for n in all_new if sid(n.get('refId', 0)) in ids_to_remove
                      and sid(n.get('refId', 0)) != '0']
        if not cross_refs:
            break

        added_this_pass = []
        for cr in cross_refs:
            ref_uid = sid(cr['refId'])
            expansion_key = (sid(cr['id']), ref_uid)

            # Detect self-referencing / recursive types — just clear refId
            if expansion_key in seen_expansions or ref_uid not in union_subtrees:
                cr['refId'] = 0
                continue

            seen_expansions.add(expansion_key)
            subtree = union_subtrees[ref_uid]

            # Clear refId, set classKeyword=union
            cr['refId'] = 0
            cr['classKeyword'] = 'union'

            id_map = {ref_uid: sid(cr['id'])}
            for s in subtree:
                id_map[sid(s['id'])] = str(next_id)
                next_id += 1

            for s in subtree:
                new_node = copy.deepcopy(s)
                old_sid_val = sid(s['id'])
                old_parent = sid(s.get('parentId', 0))
                new_node['id'] = id_map[old_sid_val]
                new_node['parentId'] = id_map.get(old_parent, old_parent)
                if old_parent == ref_uid:
                    new_node['offset'] = 0
                # If this new node refs a removed union, just clear it
                # (don't recursively inline — one level is enough)
                if sid(new_node.get('refId', 0)) in ids_to_remove:
                    new_node['refId'] = 0
                added_this_pass.append(new_node)

        new_nodes.extend(added_this_pass)
        all_new = added_this_pass
        if not added_this_pass:
            break

    # Apply modifications to existing nodes
    for n in nodes:
        nid = sid(n['id'])
        if nid in nodes_to_modify:
            for k, v in nodes_to_modify[nid].items():
                n[k] = v

    # Remove old union nodes and their subtrees
    nodes = [n for n in nodes if sid(n['id']) not in ids_to_remove]

    # Add new inlined nodes
    nodes.extend(new_nodes)

    # Sort by numeric id for stable output
    nodes.sort(key=lambda n: int(n['id']))

    # Final cleanup: remove orphan nodes whose parentId doesn't exist
    all_ids = {sid(n['id']) for n in nodes}
    all_ids.add('0')
    orphan_count = 0
    while True:
        orphans = {sid(n['id']) for n in nodes
                   if sid(n.get('parentId', 0)) != '0'
                   and sid(n.get('parentId', 0)) not in all_ids}
        if not orphans:
            break
        orphan_count += len(orphans)
        nodes = [n for n in nodes if sid(n['id']) not in orphans]
        all_ids -= orphans

    # Final cleanup: clear any remaining dangling refIds
    dangling_fixed = 0
    for n in nodes:
        ref = sid(n.get('refId', 0))
        if ref != '0' and ref not in all_ids:
            n['refId'] = 0
            dangling_fixed += 1

    data['nodes'] = nodes

    with open(filepath, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False)

    total = len(root_unions)
    extra = ""
    if orphan_count:
        extra += f", cleaned {orphan_count} orphans"
    if dangling_fixed:
        extra += f", fixed {dangling_fixed} dangling refs"
    print(f"  {filepath}: removed {total} root unions ({stats_inlined} inlined, "
          f"{stats_deleted} deleted), added {len(new_nodes)} new nodes{extra}")
    return True


if __name__ == '__main__':
    files = [
        'src/examples/WinSDK.rcx',
        'src/examples/EPROCESS.rcx',
        'src/examples/MMPFN.rcx',
        'src/examples/Vergilius_25H2.rcx',
    ]

    for f in files:
        try:
            process_file(f)
        except Exception as e:
            print(f"  ERROR processing {f}: {e}")
            import traceback
            traceback.print_exc()
