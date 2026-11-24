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

A tenth column, heal_hp, is the health a cooked dish gives back when eaten,
0 for anything else. Sov asked on Discord on 1 October 2026 for a floor on
food by what it heals, keeping the 400 health dishes and leaving the 120 ones.
The item table has no such number. A dish's use records name a skill,
Item_Active_Food_HP or Item_Active_Food_HP_1 to _5, and a level, as the last
eight bytes of the record before any trailing byte. That skill holds one Hp
change per level, stored times a thousand, and level 1 is the first.
Filling Pan-Fried Rice Cakes reads HP_3 level 6, 600 health, and its fire
resistance at buff level 6, both as the vulkk.com item database lists them.
Raw ingredients, horse feed, potions and the percentage dishes use skills of
other names and stay at 0, so the floor never reaches them.
"""
import csv
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "items_tagged.csv")
DST = os.path.join(ROOT, "mod", "data", "MasterLooter.items.tsv")

# The dish skills: Item_Active_Food_HP and Item_Active_Food_HP_1 to _5.
DISH_SKILL = re.compile(r"^Item_Active_Food_HP(_[1-9])?$")
# A level's Hp change: a u32 3, a zero byte, stat key 1000000 (Hp in
# statusinfo), then the amount as an i64 times a thousand.
HP_CHANGE = re.compile(rb"\x03\x00\x00\x00\x00\x40\x42\x0f\x00(........)", re.S)


def _name(rec):
    n = struct.unpack_from("<I", rec, 0)[0]
    return rec[4:4 + n].decode("utf-8", "replace")


def food_health(cdtables):
    """{string_key: health a dish restores when eaten} for the cooked dishes."""
    levels = {}
    for key, rec in cdtables.load_table("skill"):
        name = _name(rec)
        if DISH_SKILL.match(name):
            levels[key] = [struct.unpack("<q", m.group(1))[0] for m in HP_CHANGE.finditer(rec)]
            assert levels[key] and levels[key] == sorted(levels[key]), (name, levels[key])
    uses = dict(cdtables.load_table("itemuseinfo"))
    items, _bad = cdtables.parse_all_items()
    out = {}
    for it in items:
        best = 0
        for uk in it.get("item_use_info_list") or []:
            rec = uses.get(uk)
            if rec is None:
                continue
            for back in (8, 9):
                if len(rec) < back:
                    continue
                skill, level = struct.unpack_from("<II", rec, len(rec) - back)
                if skill in levels and 1 <= level <= len(levels[skill]):
                    best = max(best, levels[skill][level - 1] // 1000)
        if best:
            out[it["string_key"]] = best
    return out


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
    heal = food_health(cdtables)
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("row\tkey\tstring_key\tname\tclass\ttags\ttier\tvalue_copper\tplaced_prefab\theal_hp\n")
        for r in rows:
            name = r["name"].replace("\t", " ").replace("\n", " ")
            f.write("%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n" % (order.get(int(r["key"]), -1), r["key"], r["string_key"], name,
                                                             r["klass"], r["tags"], r["tier"], r["value_copper"],
                                                             placed.get(r["string_key"], ""), heal.get(r["string_key"], 0)))
    print("wrote", DST, len(rows), "items,", sum(1 for r in rows if int(r["key"]) not in order), "of them DLC-only,",
          len(placed), "of", len(housing), "housing-only items with a placed prefab,",
          sum(1 for r in rows if heal.get(r["string_key"])), "dishes with a health value")


if __name__ == "__main__":
    main()
