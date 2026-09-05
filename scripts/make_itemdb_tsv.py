"""Export the tagged item table as the runtime database the plugin reads.

Writes mod/data/MasterLooter.items.tsv with columns:
row, key, string_key, name, class, tags, tier, value_copper

row is the record's position in iteminfo.staticinfoheader, which is what the
game uses as the runtime item type id (the u16 in inventory slots and on world
item nodes). The plugin verifies this against the live table before trusting it.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "items_tagged.csv")
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.items.tsv")


def main():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import cdtables
    order = {key: i for i, (key, _) in enumerate(cdtables.load_table("iteminfo"))}
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    rows = list(csv.DictReader(open(SRC, encoding="utf-8-sig")))
    rows.sort(key=lambda r: order[int(r["key"])])
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("row\tkey\tstring_key\tname\tclass\ttags\ttier\tvalue_copper\n")
        for r in rows:
            name = r["name"].replace("\t", " ").replace("\n", " ")
            f.write("%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" % (order[int(r["key"])], r["key"], r["string_key"], name,
                                                     r["klass"], r["tags"], r["tier"], r["value_copper"]))
    print("wrote", DST, len(rows), "items")


if __name__ == "__main__":
    main()
