"""Export the tagged item table as the runtime database the plugin reads.

Writes mod/data/MasterLooter.items.tsv with columns:
key, string_key, name, class, tags, tier, value_copper
"""
import csv
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "items_tagged.csv")
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.items.tsv")


def main():
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    rows = list(csv.DictReader(open(SRC, encoding="utf-8-sig")))
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("key\tstring_key\tname\tclass\ttags\ttier\tvalue_copper\n")
        for r in rows:
            name = r["name"].replace("\t", " ").replace("\n", " ")
            f.write("%s\t%s\t%s\t%s\t%s\t%s\t%s\n" % (r["key"], r["string_key"], name, r["klass"], r["tags"], r["tier"], r["value_copper"]))
    print("wrote", DST, len(rows), "items")


if __name__ == "__main__":
    main()
