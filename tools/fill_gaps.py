"""
Audit + fix .rcx example files: every gap between consecutive sibling
offsets inside a Struct parent gets filled with hex padding nodes
(Hex64 then Hex32/Hex16/Hex8 for the tail). No gaps allowed.

Usage:
    python tools/fill_gaps.py [--check] [--write] FILE [FILE...]

  --check  list gaps without modifying anything
  --write  apply fixes in-place
  (default = --check)
"""
import json
import sys
import argparse
from pathlib import Path

# Mirrors kKindMeta in src/core.h. Container kinds (Struct, Array) have
# size 0 — their span is computed from children.
KIND_SIZE = {
    "Hex8": 1, "Hex16": 2, "Hex32": 4, "Hex64": 8, "Hex128": 16,
    "Int8": 1, "Int16": 2, "Int32": 4, "Int64": 8, "Int128": 16,
    "UInt8": 1, "UInt16": 2, "UInt32": 4, "UInt64": 8, "UInt128": 16,
    "Float16": 2, "Float": 4, "Double": 8,
    "Bool": 1,
    "Pointer32": 4, "Pointer64": 8,
    "FuncPtr32": 4, "FuncPtr64": 8,
    "Vec2": 8, "Vec3": 12, "Vec4": 16, "Mat4x4": 64,
    "UTF8": 1, "UTF16": 2,
    "Struct": 0, "Array": 0,
}


def node_size(node, by_id, next_sibling_off=None):
    """Recursively compute byte size of a node.

    `next_sibling_off` is the offset of the next sibling under the same
    parent — used as an upper bound for typed-Struct stubs that have a
    name but no children (e.g. `_LARGE_INTEGER`, `_GUID`). On the C++
    side, those resolve via the type-alias table; here we just claim
    the space up to the next sibling so they don't read as gaps."""
    kind = node["kind"]
    if kind == "Array":
        elem = node.get("elementKind", "UInt8")
        arr_len = int(node.get("arrayLen", 1))
        elem_sz = KIND_SIZE.get(elem, 0)
        return arr_len * (elem_sz if elem_sz > 0 else 1)
    if kind == "Struct":
        nid = node["id"]
        children = [n for n in by_id.values() if n.get("parentId") == nid]
        if not children:
            # Typed stub (e.g. _LARGE_INTEGER) — the C++ side sizes it
            # via type lookup. We can't replicate that without the
            # type-alias table, so trust that the file is correct and
            # claim space up to the next sibling.
            if next_sibling_off is not None:
                return next_sibling_off - int(node["offset"])
            return 0
        end = 0
        sorted_kids = sorted(children, key=lambda c: int(c["offset"]))
        for i, c in enumerate(sorted_kids):
            kid_next = (int(sorted_kids[i + 1]["offset"])
                        if i + 1 < len(sorted_kids) else None)
            end = max(end, int(c["offset"]) + node_size(c, by_id, kid_next))
        return end
    return KIND_SIZE.get(kind, 0)


def hex_chunk_for(off, size):
    """Pick the largest single hex kind that (a) fits in `size` bytes and
    (b) is aligned for `off`. Aligned chunks make later RE work cleaner —
    a 4-byte gap at +0x84 gets a single Hex32, not a misaligned Hex64."""
    if size >= 8 and (off % 8) == 0: return ("Hex64", 8)
    if size >= 4 and (off % 4) == 0: return ("Hex32", 4)
    if size >= 2 and (off % 2) == 0: return ("Hex16", 2)
    return ("Hex8", 1)


def fill_gaps(doc):
    """Walk every Struct parent, fill gaps between consecutive children
    + tail gap before the struct's declared end. Returns (gaps_filled,
    list_of_descriptions)."""
    nodes = doc["nodes"]
    by_id = {n["id"]: n for n in nodes}
    next_id = int(doc.get("nextId", "1"))

    descriptions = []
    inserted = []

    # Identify Struct parents (excluding Array — array element offsets
    # are always exact).
    parents_by_id = {}
    for n in nodes:
        pid = n.get("parentId", "0")
        parents_by_id.setdefault(pid, []).append(n)

    for parent_id, children in parents_by_id.items():
        if parent_id == "0":
            continue  # root level
        parent = by_id.get(parent_id)
        if parent is None or parent.get("kind") != "Struct":
            continue
        # Sort children by offset
        children = sorted(children, key=lambda c: int(c["offset"]))
        # Check gaps between consecutive siblings
        for i in range(len(children) - 1):
            cur = children[i]
            nxt = children[i + 1]
            cur_off = int(cur["offset"])
            nxt_off = int(nxt["offset"])
            cur_size = node_size(cur, by_id, next_sibling_off=nxt_off)
            gap_start = cur_off + cur_size
            gap = nxt_off - gap_start
            if gap > 0:
                parent_name = parent.get("structTypeName") or parent.get("name") or parent_id
                descriptions.append(
                    f"  [{parent_name}] gap {gap} byte(s) between "
                    f"{cur.get('name', '?')}@+0x{cur_off:x} and "
                    f"{nxt.get('name', '?')}@+0x{nxt_off:x}"
                )
                # Generate hex fillers
                pos = gap_start
                remaining = gap
                while remaining > 0:
                    kind, sz = hex_chunk_for(pos, remaining)
                    filler = {
                        "arrayLen": 1,
                        "collapsed": True,
                        "elementKind": "UInt8",
                        "id": str(next_id),
                        "kind": kind,
                        "name": "",
                        "offset": pos,
                        "parentId": parent_id,
                        "refId": "0",
                        "strLen": 64,
                    }
                    inserted.append(filler)
                    next_id += 1
                    pos += sz
                    remaining -= sz

    if inserted:
        doc["nodes"].extend(inserted)
        doc["nextId"] = str(next_id)
    return inserted, descriptions


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--write", action="store_true",
                    help="apply fixes in-place (default = check only)")
    args = ap.parse_args()

    total_inserts = 0
    for fpath in args.files:
        p = Path(fpath)
        if not p.exists():
            print(f"!! missing: {fpath}", file=sys.stderr)
            continue
        with p.open(encoding="utf-8") as f:
            doc = json.load(f)
        inserted, descs = fill_gaps(doc)
        if descs:
            print(f"\n{fpath}: {len(inserted)} filler(s) needed")
            for d in descs:
                print(d)
            if args.write:
                with p.open("w", encoding="utf-8") as f:
                    json.dump(doc, f, indent=4)
                    f.write("\n")
                print(f"  -> wrote {len(inserted)} filler nodes")
            total_inserts += len(inserted)
        else:
            print(f"{fpath}: clean (no gaps)")

    print(f"\nTotal: {total_inserts} filler(s) "
          f"{'inserted' if args.write else 'would be inserted'}")


if __name__ == "__main__":
    main()
