"""Export the tagged item table as the runtime database the plugin reads.

Writes mod/data/MasterLooter.items.tsv with columns:
row, key, string_key, name, class, tags, tier, value_copper

row is the record's position in iteminfo.staticinfoheader, which is what the
game uses as the runtime item type id (the u16 in inventory slots and on world
item nodes). The plugin verifies this against the live table before trusting it.

Items that only the DLC's own rows carry come after the base items with row -1:
nobody has yet seen where the game numbers them, so the plugin finds them by the
live string key alone and never by row.

A ninth column, placed_prefab, is written for housing-only items alone: the
basename of the gimmick the item's gimmick_info names, keyed the way
make_nodes_tsv.prefab_key keys the node table. That gimmick is the thing a
player puts down in a camp or a house. The node table has a row for 15 of the
572, so the other 557 reached the plugin as nodes that name nothing, and with
Unidentified nodes on it gathered them. LlexX on the posts tab, 15 November
2026, could not stop it taking a Greymane Dwelling Tent
(gimmick_tent_graymane_01) whatever the filter said. The plugin hands such a
node this item as its yield, so the Furniture switch and the housing-only
protection reach it.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "items_tagged.csv")
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.items.tsv")


def placed_prefabs(cdtables, housing):
    """{string_key: prefab basename} for the string keys in `housing`."""
    import make_nodes_tsv
    path_of = {}
    for key, rec in cdtables.load_table("gimmickinfo"):
        ss = make_nodes_tsv.strings(rec)
        path = next((s[:s.find(".prefab") + len(".prefab")] for s in ss if ".prefab" in s), "")
        if path:
            path_of[key] = path
    items, _bad = cdtables.parse_all_items()
    out, owner = {}, {}
    for it in items:
        sk = it.get("string_key", "")
        if sk not in housing or not it.get("gimmick_info") or it["gimmick_info"] not in path_of:
            continue
        base = make_nodes_tsv.prefab_key(path_of[it["gimmick_info"]])
        # Two items placing one prefab would make the yield a coin toss, so
        # neither gets it. None do on game 1.0.0.3663.
        if base in owner:
            print("placed prefab %s is shared by %s and %s; left out" % (base, owner[base], sk))
            out.pop(owner[base], None)
            continue
        owner[base] = sk
        out[sk] = base
    return out


def main():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import cdtables
    order = {key: i for i, (key, _) in enumerate(cdtables.load_table("iteminfo", dlc=False))}
    dlc = {key: len(order) + i for i, (key, _) in enumerate(cdtables.load_table("iteminfo")[len(order):])}
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    rows = list(csv.DictReader(open(SRC, encoding="utf-8-sig")))
    rows.sort(key=lambda r: order[int(r["key"])] if int(r["key"]) in order else dlc[int(r["key"])])
    housing = {r["string_key"] for r in rows if "housing-only" in r["tags"].split()}
    placed = placed_prefabs(cdtables, housing)
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("row\tkey\tstring_key\tname\tclass\ttags\ttier\tvalue_copper\tplaced_prefab\n")
        for r in rows:
            name = r["name"].replace("\t", " ").replace("\n", " ")
            f.write("%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" % (order.get(int(r["key"]), -1), r["key"], r["string_key"], name,
                                                         r["klass"], r["tags"], r["tier"], r["value_copper"],
                                                         placed.get(r["string_key"], "")))
    print("wrote", DST, len(rows), "items,", sum(1 for r in rows if int(r["key"]) not in order), "of them DLC-only,",
          len(placed), "of", len(housing), "housing-only items with a placed prefab")


if __name__ == "__main__":
    main()
