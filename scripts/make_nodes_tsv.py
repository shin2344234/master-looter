"""Gather-node table for the plugin (Crimson Desert 2.01.00).

A gather node reports a 16-bit id that is not stable between sessions, so it
cannot be used to remember what a node yields. What the node does carry is the
path of its own prefab, readable from the gimmick component at +0x18 -> +0x38,
and every one of those paths is a row of GimmickInfo. That row names the node
and tags what kind of gathering it is, which is exactly what the Plants, Ore,
Stone and Wood switches need.

Writes mod/data/MasterLooter.nodes.tsv: prefab basename, kind, item string key
when the name gives one, and the row's own name. The plugin compiles it in.

Run after build_item_db.py, which produces the items_tagged.csv this reads.
"""

import csv
import os
import re
import sys

import cdtables

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
OUT = os.path.join(HERE, "..", "mod", "data", "MasterLooter.nodes.tsv")

# Tags the game puts on a gimmick row, mapped to the switch that owns them.
# A crop or a fruit is picked up as a ground item whether it is on the plant or
# on the floor, which is why they land on `item` and not on `plant`.
TAG_KIND = {
    "collect_wood": "wood", "collect_tree": "wood", "collect_firewood": "wood",
    "collect_mine": "ore", "collect_ore": "ore",
    "collect_botany": "plant",
    "catch_treefruit": "item", "catch_berries": "item", "catch_groundfruit": "item",
    "catch_crops": "item", "catch_vegetable": "item",
}
# Tags above that override collect_botany when a row carries both.
FRUIT_TAGS = ("catch_treefruit", "catch_berries", "catch_groundfruit", "catch_crops", "catch_vegetable")

# Fallback for gather nodes the table does not tag, read off the prefab name.
# Ordered: the first hit wins. Never consulted for a gimmick_attach_ prefab;
# see kind_for().
NAME_KIND = [
    ("wood",  ("_log", "log_", "firewood", "timber", "stump", "branch", "_tree", "tree_")),
    # No ore branch. The game tags every mine and ore gather node it has, so a
    # name guess here can only add a false positive, and it added 49 of them:
    # boss armour plates, gold bars, paper stars, a naval mine, a spear.
    ("stone", ("_rock", "rock_", "_stone", "stone_", "boulder", "pebble")),
    ("plant", ("grass", "herb", "flower", "mushroom", "weed_", "moss", "fern", "vine",
               "leaf", "leaves", "seaweed", "algae", "reed", "bush", "shrub")),
]

STRIP_PREFIX = ("gimmick_", "socket_collection_", "collect_", "collection_", "attach_",
                "nature_", "tree_", "plant_")
STRIP_SUFFIX = re.compile(r"_(scenecollector|hero|big|giant|thin|burnt|small|root|drop|index\d+|\d+)$")


def strings(rec):
    return [s.decode("ascii", "ignore") for s in re.findall(rb"[ -~]{4,}", rec)]


def stem(name):
    s = name.lower()
    for _ in range(4):
        for p in STRIP_PREFIX:
            if s.startswith(p):
                s = s[len(p):]
                break
        else:
            break
    for _ in range(4):
        s2 = STRIP_SUFFIX.sub("", s)
        if s2 == s:
            break
        s = s2
    return s


def prefab_key(path):
    """Basename without the extension, and without the scene-collector wrapper.

    A placed object often points at a wrapper prefab whose name is the gimmick's
    own with `_scenecollector` on the end, in a different folder, so the folder
    is dropped and the suffix normalised away on both sides of the lookup.
    """
    base = path.rsplit("/", 1)[-1].lower()
    if base.endswith(".prefab"):
        base = base[:-7]
    if base.endswith("_scenecollector"):
        base = base[:-len("_scenecollector")]
    return base


KIND_NOUN = {"plant": "Plant", "ore": "Ore", "stone": "Stone", "wood": "Wood", "item": "Crop"}


def pretty(name, kind):
    """A label for a node whose socket name gives no item, e.g. a named tree.

    The raw row names run long (gimmick_tree_pine_spruce_norway_hero_drop_01),
    and they are read in the Nearby list and the log, so they are trimmed to
    the part that says something and fall back to the kind when nothing does.
    """
    s = name.lower()
    if s.startswith("gimmick_"):
        s = s[len("gimmick_"):]
    if s.endswith("_scenecollector"):
        s = s[:-len("_scenecollector")]
    s = re.sub(r"_\d+$", "", s).replace("_", " ").strip()
    if not s or len(s) > 32:
        return KIND_NOUN.get(kind, "Node")
    return s[0].upper() + s[1:]


def load_items():
    by_key, by_name = {}, {}
    path = os.path.join(DATA, "items_tagged.csv")
    if not os.path.exists(path):
        sys.exit("items_tagged.csv not found; run build_item_db.py first")
    for r in csv.DictReader(open(path, encoding="utf-8-sig")):
        by_key[r["string_key"].lower()] = r
        by_name.setdefault(re.sub(r"[^a-z0-9]", "", r["name"].lower()), r)
    return by_key, by_name


# An item is only accepted as a node's yield when its class fits the kind the
# tags gave. Without this a copper vein matches Money_Copper, the coin, and the
# node is then judged by the rules for currency.
KIND_CLASSES = {
    "wood":  {"wood", "crafting-material"},
    "ore":   {"ore", "jewel", "mineral", "metal", "stone"},
    "stone": {"stone", "ore", "jewel", "mineral"},
    "plant": {"herb", "mushroom", "seed", "alchemy-material", "flower", "ingredient",
              "crafting-material", "vegetable", "fruit", "grain"},
    "item":  {"vegetable", "fruit", "grain", "ingredient", "herb", "seed", "trade-good", "goods"},
}


def match_item(name, kind, by_key, by_name):
    s = stem(name)
    for c in (s, s.replace("_", ""), s.split("_")[-1]):
        if len(c) < 3:
            continue
        it = by_key.get(c) or by_name.get(c)
        if not it:
            continue
        ok = KIND_CLASSES.get(kind, set())
        if it["klass"] in ok or (it.get("tags") and set(it["tags"].split()) & ok):
            return it
        return None      # a near-miss on the name is worse than no item at all
    return None


# Words that never appear on a mineable vein. Without these the ore guess
# claims shop counters, decorative pipework, abyss puzzle platforms and a
# fountain, all of which then get a 25 m reach and twelve arm calls.
NOT_A_VEIN = ("pipe", "shop", "npctable", "store", "fountain", "airballoon",
              "platform", "battery", "conductor", "preset", "sandcrawler",
              "visione", "abyss", "magnet", "vehicle", "cannon", "furnace")


def kind_for(tags, name, prefab):
    """The kind, and whether the game said so or the name merely suggested it."""
    for t in FRUIT_TAGS:
        if t in tags:
            return "item", True
    for t in tags:
        if t in TAG_KIND:
            return TAG_KIND[t], True
    # Tags the table does not know still mean the game has classified this row,
    # and as something other than a gather node. Guessing from the name here
    # would be second-guessing it.
    if tags:
        return "", False
    # A gimmick_attach_ prefab is a piece bolted to a creature or a mechanism:
    # stoneworm and stonetoad plating, stoneowl bases, landspider queen rocks,
    # seraphim stones, thorny vines. None of them is something a player
    # gathers, and gathering one empties its visual while whatever carries its
    # damage volume stays put, which is issue #41. The game tags the six real
    # mining spots under this prefix itself, so the guess can only add false
    # positives here, and it added 110 of them.
    if prefab.startswith("gimmick_attach_"):
        return "", False
    low = name.lower()
    for kind, words in NAME_KIND:
        if kind == "ore":
            # Whole segments only. Anywhere-in-the-string is what let a bare
            # "ore" hide inside "core" and "store".
            if any(w in NOT_A_VEIN for w in low.split("_")):
                continue
            if any(t in NOT_A_VEIN for t in NOT_A_VEIN if t in low):
                continue
            if any(seg == w or seg.startswith(w) for seg in low.split("_") for w in words):
                return kind, False
            continue
        if any(w in low for w in words):
            return kind, False
    return "", False


def main():
    rows = cdtables.load_table("gimmickinfo")
    by_key, by_name = load_items()
    out, seen = [], set()
    stats = {"rows": 0, "with_path": 0, "tagged": 0, "by_name": 0, "with_item": 0}
    for key, rec in rows:
        stats["rows"] += 1
        ss = strings(rec)
        if not ss:
            continue
        name = ss[0]
        # Cut at the extension rather than requiring it at the end: the byte
        # after the path is often printable, so strings() hands back
        # "..._scenecollector.prefabxC" and endswith() dropped 47% of the table.
        path = ""
        for s in ss:
            i = s.find(".prefab")
            if i >= 0:
                path = s[:i + len(".prefab")]
                break
        if not path:
            continue
        stats["with_path"] += 1
        tags = {s for s in ss if s.startswith(("collect", "catch"))}
        base = prefab_key(path)
        kind, vouched = kind_for(tags, name, base)
        if not kind:
            continue
        stats["tagged" if vouched else "by_name"] += 1
        if base in seen:
            continue
        seen.add(base)
        it = match_item(name, kind, by_key, by_name)
        if it:
            stats["with_item"] += 1
        out.append((base, kind, it["string_key"] if it else "",
                    it["name"] if it else pretty(name, kind),
                    "tag" if vouched else "name"))
    out.sort()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        # src says whether the game's own gimmick tag gave the kind or the
        # generator guessed it from the prefab name. The engine spends the
        # long ore reach only on the ones the game vouches for.
        f.write("prefab\tkind\titem_key\tname\tsrc\n")
        for r in out:
            f.write("\t".join(r) + "\n")
    kinds = {}
    for _, k, _, _, _ in out:
        kinds[k] = kinds.get(k, 0) + 1
    print("gimmick rows %(rows)d, with a prefab path %(with_path)d, "
          "classified by tag %(tagged)d, by name %(by_name)d, item resolved %(with_item)d" % stats)
    print("wrote %d rows to %s" % (len(out), os.path.relpath(OUT, HERE)))
    print("by kind: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    ore = [r for r in out if r[1] == "ore"]
    print("ore: %d rows, %d of them vouched for by the game's own tag"
          % (len(ore), sum(1 for r in ore if r[4] == "tag")))


if __name__ == "__main__":
    main()
