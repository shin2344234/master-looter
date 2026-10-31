"""Hold every item, node and creature up against what the plugin does with it.

Reads the shipped tables in mod/data and the research set in data/, runs each
item through the same gates Rules::Decide and KindOf apply with the default
settings, and writes:

  data/item_audit.csv   one row per item: verdict, the switch that owns it,
                        the Classes tab group, why it is dev, where it drops,
                        and the quest-risk markers it carries
  data/item_audit.md    the groups worth reading by eye

Run it after any table rebuild. It exits 1 when an item the game hands out
through a drop set, a gimmick, a node or a catch is classed dev, because the
plugin refuses that class outright and no setting reaches it.
"""
import csv
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")
MDATA = os.path.join(ROOT, "mod", "data")

# Mirrors of the plugin. Each names the line it copies so a drift is findable.
# build_item_db.py DEV_RE
DEV_RE = re.compile(r"(^|_)(Test|Dev|QA|Dummy|Debug|Sample)(_|$)|_QA$|_T\d_QA|^TestNeck|^Testarmor|^Socket_Test", re.I)
# rules.cpp protectedTags
PROTECTED = ("memory-fragment", "gimmick", "housing-only")
# menu.cpp kGroups
GROUPS = [
    ("Weapons and armor", "weapon shield helm body-armor gloves boots cloak armor mask eyewear"),
    ("Damaged gear", "damaged-gear"),
    ("Accessories", "accessory necklace ring earring bag"),
    ("Abyss and Kuku", "abyss-gear abyss-gear-box abyss-item kuku-power-core kuku-core kuku-pot-item kuku-pot kuku-currency stat-boost"),
    ("Food and drink", "food field-cooked drink elixir potion store-food honey meat seafood fruit vegetable grain cooking-basic"),
    ("Materials", "ore jewel stone wood hide fabric bone crafting-material alchemy-material herb ingredient seed trade-good bait"),
    ("Creatures", "insect fish animal amphibian"),
    ("Ammunition", "arrow ammo ammo-bundle bullet magic-bullet cannonball explosive"),
    ("Books and papers", "book document note poster skill-poster bounty-notice treasure-map legendary-animal-report skill-book recipe-book recipe-food recipe-potion recipe-furniture recipe-abyss-gear recipe-armor"),
    ("Furniture and decor", "furniture household lamp light ornament painting flower-pot decoration cooking-facility storage container"),
    ("Mounts and vehicles", "mount-gear mount-feed mount-utility pet-gear vehicle-part"),
    ("Treasure and keepsakes", "treasure sealed-artifact artifact keepsake currency chest"),
    ("Keys and tools", "key key-item tool"),
]
GROUP_OF = {c: g for g, cs in GROUPS for c in cs.split()}


def tsv(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def csvrows(path):
    with open(path, encoding="utf-8-sig", newline="") as f:
        return list(csv.DictReader(f))


def kind_of(klass, tags):
    """engine.cpp KindOf: which Looting switch owns the item."""
    if klass == "wood" or "wood" in tags: return "Wood"
    if klass == "stone" or "stone" in tags: return "Stone"
    if klass in ("ore", "jewel") or "ore" in tags or "mineral" in tags: return "Ore"
    if klass == "herb": return "Plant"
    if klass == "dye": return "Dye"
    if "furniture" in tags and klass not in ("container", "storage", "chest"): return "Furniture"
    if klass in ("container", "storage"): return "Container"
    if klass in ("vegetable", "fruit", "grain"): return "Crop"
    if klass == "seed": return "Plant"
    return "Item"


def verdict(klass, tags):
    """rules.cpp Decide under the default settings: no item, tag or class
    rules, Skip quest items on, Skip quest equipment on, Skip unsellable off,
    no value floor."""
    for t in PROTECTED:
        if t in tags: return "protected " + t
    if klass == "dev": return "dev item"
    if "quest" in tags: return "quest item"
    if "important" in tags and "no-sell" in tags: return "quest equipment"
    return "loot"


def dev_reason(sk, name, leaf):
    why = []
    if not name: why.append("no English name")
    m = DEV_RE.search(sk)
    if m: why.append("name pattern '%s'" % m.group(0).strip("_"))
    if "ItemGroup_Equip_Dev" in leaf: why.append("dev equipment group")
    if not why: why.append("not editor-usable")
    return "; ".join(why)


def main():
    items = tsv(os.path.join(MDATA, "MasterLooter.items.tsv"))
    tagged = {r["string_key"]: r for r in csvrows(os.path.join(DATA, "items_tagged.csv"))}
    sources = {r["string_key"]: r for r in csvrows(os.path.join(DATA, "item_sources.csv"))}
    nodes = tsv(os.path.join(MDATA, "MasterLooter.nodes.tsv"))
    creatures = tsv(os.path.join(MDATA, "MasterLooter.creatures.tsv"))
    by_sk = {r["string_key"]: r for r in items}
    by_name = collections.defaultdict(list)
    for r in items:
        by_name[r["name"]].append(r)

    # What nodes and catches hand over, by item string key.
    node_yield = collections.Counter()
    for n in nodes:
        if n["item_key"]: node_yield[n["item_key"]] += 1
        for y in filter(None, (n.get("yields") or "").split("|")):
            for r in by_name.get(y.strip(), []):
                node_yield[r["string_key"]] += 1
    catch = collections.Counter()
    rows_by_index = {r["row"]: r for r in items if r["row"] != "-1"}
    for c in creatures:
        it = rows_by_index.get(c["item_row"]) if c["item_row"] != "-1" else by_sk.get(c.get("item_key") or "")
        if it: catch[it["string_key"]] += 1

    out = []
    for r in items:
        sk, klass, tags = r["string_key"], r["class"], set(r["tags"].split())
        t = tagged.get(sk, {})
        s = sources.get(sk, {})
        leaf = t.get("leaf_groups", "")
        root = t.get("root_groups", "")
        n_char = int(s.get("n_characters_via_sets") or 0)
        n_gim = int(s.get("n_gimmicks_via_sets") or 0)
        n_eq = int(s.get("n_equipped_by") or 0)
        handed = n_char + n_gim + node_yield[sk] + catch[sk]
        v = verdict(klass, tags)
        # Quest-risk markers on things the defaults would take. Each one is a
        # property the game itself sets, never a guess from the English name.
        risk = []
        groups = leaf + "|" + root
        if re.search(r"Quest", groups, re.I): risk.append("quest group")
        if "important" in tags: risk.append("important")
        if "no-discard" in tags: risk.append("no-discard")
        if klass in ("key", "key-item", "cube-key"): risk.append("key")
        if "hidden" in tags: risk.append("hidden")
        if "blocked" in tags: risk.append("blocked")
        if "docking" in tags and "equipment" not in tags: risk.append("docking")
        if handed == 0 and n_eq == 0 and "no-sell" in tags: risk.append("placed, no-sell")
        out.append(dict(
            row=r["row"], string_key=sk, name=r["name"], dlc="1" if r["row"] == "-1" else "",
            klass=klass, menu_group=GROUP_OF.get(klass, ""), switch=kind_of(klass, tags),
            verdict=v, dev_reason=dev_reason(sk, r["name"], leaf) if klass == "dev" else "",
            dropped_by_characters=n_char, dropped_by_gimmicks=n_gim, equipped_by=n_eq,
            node_yield=node_yield[sk], catch=catch[sk],
            quest_risk=", ".join(risk) if v == "loot" else "",
            value=r["value_copper"], tags=r["tags"], groups=leaf,
        ))

    with open(os.path.join(DATA, "item_audit.csv"), "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(out[0].keys()))
        w.writeheader()
        w.writerows(out)

    # Report.
    L = []
    def table(rows, cols, limit=None):
        L.append("| " + " | ".join(cols) + " |")
        L.append("|" + "---|" * len(cols))
        for x in rows[:limit] if limit else rows:
            L.append("| " + " | ".join(str(x[c]).replace("|", "/") for c in cols) + " |")
        if limit and len(rows) > limit:
            L.append("")
            L.append("%d more in item_audit.csv." % (len(rows) - limit))
        L.append("")

    verdicts = collections.Counter(x["verdict"] for x in out)
    L.append("# Item audit")
    L.append("")
    L.append("Generated by `scripts/audit_items.py` from the shipped tables. %d items, %d of them DLC-only." %
             (len(out), sum(1 for x in out if x["dlc"])))
    L.append("")
    L.append("Default verdicts: " + ", ".join("%s %d" % kv for kv in verdicts.most_common()) + ".")
    L.append("")

    dev_handed = [x for x in out if x["klass"] == "dev" and
                  (x["dropped_by_characters"] or x["dropped_by_gimmicks"] or x["node_yield"] or x["catch"])]
    dev_handed.sort(key=lambda x: -(x["dropped_by_characters"] + x["dropped_by_gimmicks"] + x["node_yield"]))
    L.append("## Classed dev but handed out by the game")
    L.append("")
    L.append("The plugin refuses the dev class outright. Each of these is in a drop set, a gimmick yield, a node yield or a catch, so somebody can meet it in play.")
    L.append("")
    table(dev_handed, ["string_key", "name", "dev_reason", "dropped_by_characters", "dropped_by_gimmicks", "node_yield", "catch"])

    reasons = collections.Counter()
    for x in out:
        if x["klass"] == "dev":
            for part in x["dev_reason"].split("; "):
                reasons[part] += 1
    L.append("## Why the other dev items are dev")
    L.append("")
    for k, n in reasons.most_common():
        L.append("- %s: %d" % (k, n))
    L.append("")

    ungrouped = collections.Counter(x["klass"] for x in out if not x["menu_group"] and x["klass"] != "dev")
    L.append("## Classes in no group on the Classes tab")
    L.append("")
    L.append("These still have their own row in the class table, but no group checkbox reaches them.")
    L.append("")
    for k, n in ungrouped.most_common():
        ex = [x["name"] for x in out if x["klass"] == k][:3]
        L.append("- %s: %d, e.g. %s" % (k, n, ", ".join(ex)))
    L.append("")

    risky = [x for x in out if x["quest_risk"]]
    strong = [x for x in risky if "quest group" in x["quest_risk"] or "key" in x["quest_risk"].split(", ")
              or ("important" in x["quest_risk"] and "no-discard" in x["quest_risk"])]
    L.append("## Taken by default but marked like quest items")
    L.append("")
    L.append("%d items the defaults would take carry at least one marker the game sets on quest items. "
             "The %d below carry a quest group, a key class, or important and no-discard together." % (len(risky), len(strong)))
    L.append("")
    strong.sort(key=lambda x: (x["klass"], x["name"]))
    table(strong, ["string_key", "name", "klass", "quest_risk", "dropped_by_characters", "dropped_by_gimmicks", "groups"])

    weak = collections.Counter((x["quest_risk"], x["klass"]) for x in risky if x not in strong)
    L.append("The rest carry weaker markers. By marker and class, with three names each:")
    L.append("")
    L.append("| markers | class | items | e.g. |")
    L.append("|---|---|---|---|")
    for (q, k), n in weak.most_common():
        ex = [x["name"] for x in risky if x["quest_risk"] == q and x["klass"] == k][:3]
        L.append("| %s | %s | %d | %s |" % (q, k, n, ", ".join(ex)))
    L.append("")

    with open(os.path.join(DATA, "item_audit.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))
    print("items %d, dev handed out %d, ungrouped classes %d, quest-marked taken by default %d (strong %d)" %
          (len(out), len(dev_handed), len(ungrouped), len(risky), len(strong)))
    return 1 if dev_handed else 0


if __name__ == "__main__":
    sys.exit(main())
