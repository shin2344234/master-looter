"""Export the creatures a player can catch, with the class of the item they become.

Writes mod/data/MasterLooter.creatures.tsv with columns:
character_key, string_key, name, item_row, item_class, skin

skin is the interactioninfo key a hand skin of the creature is rewarded under,
SmallAnimal_Skin or Animal_Skin, read out of its CharacterInfo record, or 0.
The plugin sends that reward after its own skins, since the search alone never
earns the creature's knowledge (issue #70). The record lists every interaction
the character offers as a u32 key, and a seven-digit key does not turn up by
chance: of the 1004 rows 169 carry the small one, 501 the other, none both.

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
import struct
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
    # Runtime rows are header positions in the base table; a DLC-only item has
    # none the mod knows, so a creature that becomes one gets -1 here.
    order = {key: i for i, (key, _) in enumerate(cdtables.load_table("iteminfo", dlc=False))}
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

    # A record opens with its string key since exe 1.0.0.3392; before that the
    # u32 key came first and the name length sat at +4.
    inter = {}
    for key, rec in cdtables.load_table("interactioninfo"):
        n = struct.unpack_from("<I", rec, 0)[0]
        inter[rec[4:4 + n].decode("ascii", "replace")] = key
    skins = [inter["SmallAnimal_Skin"], inter["Animal_Skin"]]
    records = dict(cdtables.load_table("characterinfo"))

    def skin_of(key):
        rec = records.get(key, b"")
        found = [s for s in skins if struct.pack("<I", s) in rec]
        assert len(found) <= 1, (key, found)
        return found[0] if found else 0

    chars = list(csv.DictReader(open(os.path.join(ROOT, "data", "character_drops.csv"), encoding="utf-8-sig")))
    n = 0
    counts = {}
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("character_key\tstring_key\tname\titem_row\titem_class\tskin\titem_key\n")
        for c in chars:
            sk = c["string_key"]
            if not sk.startswith("Animal_"):
                continue
            it = find_item(c["name"])
            if it:
                klass, row, ikey = it["klass"], order.get(int(it["key"]), -1), it["string_key"]
            else:
                klass, row, ikey = guess(sk, c["name"]), -1, ""
            f.write("%s\t%s\t%s\t%d\t%s\t%d\t%s\n" % (c["key"], sk, c["name"], row, klass, skin_of(int(c["key"])), ikey))
            counts[klass] = counts.get(klass, 0) + 1
            n += 1
    print("wrote", DST, n, "creatures", counts)


if __name__ == "__main__":
    main()
