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
# Ordered: the first hit wins.
NAME_KIND = [
    ("wood",  ("_log", "log_", "firewood", "timber", "stump", "branch", "_tree", "tree_")),
    ("ore",   ("bismuth", "copper", "iron_", "_iron", "coal", "silver", "gold_ore", "mineral",
               "crystal", "quartz", "sulfur", "mercury", "_ore", "ore_", "mine_", "_mine")),
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


def load_items():
    by_key, by_name = {}, {}
    path = os.path.join(DATA, "items_tagged.csv")
    if not os.path.exists(path):
        sys.exit("items_tagged.csv not found; run build_item_db.py first")
    for r in csv.DictReader(open(path, encoding="utf-8-sig")):
        by_key[r["string_key"].lower()] = r
        by_name.setdefault(re.sub(r"[^a-z0-9]", "", r["name"].lower()), r)
    return by_key, by_name


def match_item(name, by_key, by_name):
    s = stem(name)
    for c in (s, s.replace("_", ""), s.split("_")[-1]):
        if len(c) < 3:
            continue
        if c in by_key:
            return by_key[c]
        if c in by_name:
            return by_name[c]
    return None


def kind_for(tags, name):
    for t in FRUIT_TAGS:
        if t in tags:
            return "item"
    for t in tags:
        if t in TAG_KIND:
            return TAG_KIND[t]
    low = name.lower()
    for kind, words in NAME_KIND:
        if any(w in low for w in words):
            return kind
    return ""


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
        path = next((s for s in ss if s.endswith(".prefab")), "")
        if not path:
            continue
        stats["with_path"] += 1
        tags = {s for s in ss if s.startswith(("collect", "catch"))}
        kind = kind_for(tags, name)
        if not kind:
            continue
        stats["tagged" if (tags & set(TAG_KIND)) else "by_name"] += 1
        base = prefab_key(path)
        if base in seen:
            continue
        seen.add(base)
        it = match_item(name, by_key, by_name)
        if it:
            stats["with_item"] += 1
        out.append((base, kind, it["string_key"] if it else "", it["name"] if it else name))
    out.sort()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("prefab\tkind\titem_key\tname\n")
        for r in out:
            f.write("\t".join(r) + "\n")
    kinds = {}
    for _, k, _, _ in out:
        kinds[k] = kinds.get(k, 0) + 1
    print("gimmick rows %(rows)d, with a prefab path %(with_path)d, "
          "classified by tag %(tagged)d, by name %(by_name)d, item resolved %(with_item)d" % stats)
    print("wrote %d rows to %s" % (len(out), os.path.relpath(OUT, HERE)))
    print("by kind: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))


if __name__ == "__main__":
    main()
