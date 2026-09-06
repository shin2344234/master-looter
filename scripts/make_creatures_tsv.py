"""Export the creatures a player can catch, with the class of the item they become.

Writes mod/data/MasterLooter.creatures.tsv with columns:
character_key, string_key, name, item_row, item_class

Every CharacterInfo record whose string key starts with Animal_ is a creature.
Its class comes from the tagged item with the same English name when there is
one (Meadow Butterfly the character becomes Meadow Butterfly the item; a
trailing "Butterfly" may be missing on either side). Otherwise the key and
name decide: crabs, shrimp, squid, starfish and seahorses are seafood even
though the game keys them Animal_Insect_*; anything else under Animal_Insect_
or with an insect word is an insect; fish words make a fish; frogs, toads,
salamanders and axolotls are amphibians; the rest are animals. Rows without an
item (monsters, mounts, ambient wildlife) only serve name matching in the
plugin and never vote on species words.
"""
import csv
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.creatures.tsv")
CREATURE_CLASSES = ("insect", "fish", "seafood", "animal", "amphibian")

SEAFOOD = ("crab", "lobster", "crayfish", "shrimp", "seahorse", "squid", "starfish", "clam", "octopus")
INSECT = ("butterfly", "beetle", "dragonfly", "moth", "bee", "hornet", "cricket", "grasshopper", "firefly",
          "spider", "scorpion", "snail", "centipede", "cockroach", "strider", "slater")
FISH = ("fish", "carp", "tench", "coelacanth", "eel_", "salmon", "trout", "bass", "perch", "pike", "loach",
        "sturgeon", "marlin", "shark", "bream")
AMPHIBIAN = ("frog", "toad", "salamander", "axolotl", "newt")


def guess(string_key, name):
    low = (string_key + " " + name).lower()
    if "landspider" in low:                       # the Stoneback crab monsters, not a catch
        return "animal"
    if any(w in low for w in SEAFOOD):
        return "seafood"
    if "_insect_" in low or any(w in low for w in INSECT):
        return "insect"
    if any(w in low for w in FISH):
        return "fish"
    if any(w in low for w in AMPHIBIAN):
        return "amphibian"
    return "animal"


def main():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import cdtables
    order = {key: i for i, (key, _) in enumerate(cdtables.load_table("iteminfo"))}
    items = list(csv.DictReader(open(os.path.join(ROOT, "data", "items_tagged.csv"), encoding="utf-8-sig")))
    by_name = {}
    for it in items:
        if it["klass"] in CREATURE_CLASSES and it["name"]:
            by_name.setdefault(it["name"].strip().lower(), it)

    def find_item(name):
        n = name.strip().lower()
        short = n[:-len(" butterfly")] if n.endswith(" butterfly") else None
        for cand in (n, short, n + " butterfly"):
            if cand and cand in by_name:
                return by_name[cand]
        return None

    chars = list(csv.DictReader(open(os.path.join(ROOT, "data", "character_drops.csv"), encoding="utf-8-sig")))
    n = 0
    counts = {}
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("character_key\tstring_key\tname\titem_row\titem_class\n")
        for c in chars:
            sk = c["string_key"]
            if not sk.startswith("Animal_"):
                continue
            it = find_item(c["name"])
            if it:
                klass, row = it["klass"], order.get(int(it["key"]), -1)
            else:
                klass, row = guess(sk, c["name"]), -1
            f.write("%s\t%s\t%s\t%d\t%s\n" % (c["key"], sk, c["name"], row, klass))
            counts[klass] = counts.get(klass, 0) + 1
            n += 1
    print("wrote", DST, n, "creatures", counts)


if __name__ == "__main__":
    main()
