"""Export the creatures a player can catch, with the class of the item they become.

Writes mod/data/MasterLooter.creatures.tsv with columns:
character_key, string_key, name, item_row, item_class

Every CharacterInfo record whose string key starts with Animal_ is a creature.
Its class comes from the tagged item with the same English name when there is
one (Meadow Butterfly the character becomes Meadow Butterfly the item), else
from the key itself (Animal_Insect_* is an insect, Animal_Fish_* a fish,
frogs and salamanders are amphibians, the rest are animals). The plugin uses
the table to tell insects, fish and small animals apart at catch time.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.creatures.tsv")
CREATURE_CLASSES = ("insect", "fish", "animal", "amphibian")


def main():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import cdtables
    order = {key: i for i, (key, _) in enumerate(cdtables.load_table("iteminfo"))}
    items = list(csv.DictReader(open(os.path.join(ROOT, "data", "items_tagged.csv"), encoding="utf-8-sig")))
    by_name = {}
    for it in items:
        if it["klass"] in CREATURE_CLASSES and it["name"]:
            by_name.setdefault(it["name"].strip().lower(), it)
    chars = list(csv.DictReader(open(os.path.join(ROOT, "data", "character_drops.csv"), encoding="utf-8-sig")))
    n = 0
    counts = {}
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("character_key\tstring_key\tname\titem_row\titem_class\n")
        for c in chars:
            sk = c["string_key"]
            if not sk.startswith("Animal_"):
                continue
            it = by_name.get(c["name"].strip().lower())
            low = sk.lower()
            if it:
                klass, row = it["klass"], order.get(int(it["key"]), -1)
            elif "_insect_" in low or "butterfly" in low or "beetle" in low or "dragonfly" in low or "moth" in low:
                klass, row = "insect", -1
            elif "_fish_" in low or "fish" in low or "_crab" in low or "shrimp" in low:
                klass, row = "fish", -1
            elif "frog" in low or "salamander" in low or "toad" in low or "newt" in low:
                klass, row = "amphibian", -1
            else:
                klass, row = "animal", -1
            f.write("%s\t%s\t%s\t%d\t%s\n" % (c["key"], sk, c["name"], row, klass))
            counts[klass] = counts.get(klass, 0) + 1
            n += 1
    print("wrote", DST, n, "creatures", counts)


if __name__ == "__main__":
    main()
