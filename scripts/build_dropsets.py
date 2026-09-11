"""Build the drop-set outputs for Master Looter.

Writes to ../data:
  dropsets.json          every drop set with its resolved entries
  dropset_entries.csv    one row per entry (set x target)
  item_drop_sets.csv     one row per item: which designed drop sets can produce it
  dropset_summary.md     field semantics, counts, notes
"""
import collections
import csv
import json
import os
import re
import sys

from cdtables import LOCDIR, Reader, load_paloc, load_table
from dropsets import RESULT_TYPE_NAMES, parse_all

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")

TYPE_LABEL = {0: "item", 1: "character (mount)", 3: "character (spawn)", 4: "knowledge", 5: "knowledge group",
              6: "contribution", 7: "friendly points", 9: "gimmick", 10: "character (mercenary)",
              13: "gameplay variable", 16: "type16"}


def keyed(name, w=None):
    """{key: string_key} for a table; the string sits right after the key, whose width is detected
    as the one that decodes the most plausible names (a handful of records may carry odd strings)."""
    rows = load_table(name)
    widths = [w] if w else []
    widths += [x for x in (4, 2, 8, 1) if x not in widths]
    best, best_n = None, -1
    for width in widths:
        m, n = {}, 0
        for k, rec in rows:
            try:
                r = Reader(rec); r.p = width; t = r.cstr()
            except Exception:
                t = ""
            if t and all(32 <= ord(c) < 127 for c in t) and len(t) < 160:
                n += 1
            m[k] = t
        if n > best_n:
            best, best_n = m, n
    if best_n < 0.9 * len(rows):
        raise ValueError("could not detect key width for %s (%d/%d)" % (name, best_n, len(rows)))
    return best


def paloc_names(path):
    """Return {key: english} using the most common sub-index of a .paloc as the name slot."""
    d = load_paloc(path)
    subs = collections.Counter(k & 0xffffffff for k in d)
    sub = subs.most_common(1)[0][0] if subs else 0
    return {k >> 32: v for k, v in d.items() if (k & 0xffffffff) == sub}


def kind_of(sk):
    if sk.startswith("Item("): return "inline item"
    if sk.startswith("Knowledge("): return "inline knowledge"
    if sk.startswith("Friendly("): return "inline friendly"
    if sk.startswith("SubLevelExp"): return "sub-level exp"
    if sk.startswith("Mercenary"): return "mercenary"
    if sk.lower().startswith("gameplayvariablechange"): return "gameplay variable"
    return "designed"


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sets, bad = parse_all()
    assert not bad and all(d["_end"] == d["_len"] for d in sets), "dropset parser not clean"
    items = keyed("iteminfo", 4)
    item_en = paloc_names(os.path.join(LOCDIR, "item.paloc"))
    knowledge = keyed("knowledgeinfo", 4)
    knowledge_en = {}  # knowledge.paloc is keyed by string hash, not knowledge key
    kgroups = keyed("knowledgegroupinfo")
    chars = keyed("characterinfo", 4)
    char_en = paloc_names(os.path.join(LOCDIR, "character.paloc"))
    gimmicks = keyed("gimmickinfo", 4)
    gvars = keyed("gameplayvariableinfo", 4)
    conds = keyed("conditioninfo", 4)
    tagged = {int(r["key"]): r for r in csv.DictReader(open(os.path.join(DATA, "items_tagged.csv"), encoding="utf-8-sig"))}

    def resolve(e):
        t, k = e["result_type"], e["key_raw"]
        if t == 0:
            return items.get(k, ""), item_en.get(k, "")
        if t in (1, 3, 7, 10):
            return chars.get(k, ""), char_en.get(k, "")
        if t == 4:
            return knowledge.get(k, ""), knowledge_en.get(k, "")
        if t == 5:
            return kgroups.get(k, ""), ""
        if t == 9:
            return gimmicks.get(k, ""), ""
        if t == 13:
            return gvars.get(k, ""), ""
        if t == 6:
            return "contribution #%d" % k, ""
        return "", ""

    out_sets = []
    entry_rows = []
    per_item = collections.defaultdict(list)
    for d in sets:
        kind = kind_of(d["string_key"])
        es = []
        for e in d["entries"]:
            sk, en = resolve(e)
            row = dict(
                set_key=d["key"], set_name=d["string_key"], kind=kind,
                roll_type=d["drop_roll_type"], roll_count=d["drop_roll_count"],
                total_rate_pct=d["total_drop_rate"] / 10000.0,
                result_type=e["result_type"], result_label=TYPE_LABEL.get(e["result_type"], "type%d" % e["result_type"]),
                target_key=e["key_raw"], target=sk, target_name=en,
                weight_pct=e["percent"] / 10000.0,
                share_pct=round(100.0 * e["percent"] / d["total_drop_rate"], 4) if d["total_drop_rate"] else "",
                min=e["min"], max=e["max"],
                enchant=None if e["enchant_level"] == 65535 else e["enchant_level"],
                condition=" ".join(conds.get(c, str(c)) for c in e["cond"] if c),
                tail=e["tail"].hex(),
            )
            es.append(row)
            entry_rows.append(row)
            if e["result_type"] == 0:
                per_item[e["key_raw"]].append(row)
        out_sets.append(dict(key=d["key"], string_key=d["string_key"], kind=kind, roll_type=d["drop_roll_type"],
                             roll_count=d["drop_roll_count"], total_rate_pct=d["total_drop_rate"] / 10000.0,
                             need_slot=d["nee_slot_count"] != 65535, tag_hash=d["drop_tag_name_hash"],
                             original_string=d["original_string"], entries=es))

    with open(os.path.join(DATA, "dropsets.json"), "w", encoding="utf-8") as f:
        json.dump(out_sets, f, ensure_ascii=False, indent=0)
    fields = list(entry_rows[0].keys())
    with open(os.path.join(DATA, "dropset_entries.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(entry_rows)

    # per item
    rows = []
    for k, r in tagged.items():
        refs = per_item.get(k, [])
        designed = [x for x in refs if x["kind"] == "designed"]
        rows.append(dict(
            key=k, string_key=r["string_key"], name=r["name"], klass=r["klass"],
            n_sets=len(refs), n_designed_sets=len(designed),
            n_inline_sets=sum(1 for x in refs if x["kind"] == "inline item"),
            best_share_pct=max((x["share_pct"] for x in designed if x["share_pct"] != ""), default=""),
            designed_sets=" | ".join("%s (%g%% x%d-%d)" % (x["set_name"], x["share_pct"], x["min"], x["max"]) for x in designed[:12]),
        ))
    with open(os.path.join(DATA, "item_drop_sets.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)

    # summary
    kinds = collections.Counter(d["kind"] for d in out_sets)
    types = collections.Counter(r["result_label"] for r in entry_rows)
    prefixes = collections.Counter("_".join(d["string_key"].split("_")[:2]) for d in out_sets if d["kind"] == "designed")
    in_designed = sum(1 for r in rows if r["n_designed_sets"])
    in_any = sum(1 for r in rows if r["n_sets"])
    lines = ["# Drop sets, build 2.02.00", "",
             "%d drop sets, %d entries, parsed from dropsetinfo.staticinfobody with every record ending on its byte boundary." % (len(out_sets), len(entry_rows)), "",
             "## Kinds of set", ""]
    lines += ["- %s: %d" % kv for kv in kinds.most_common()]
    lines += ["", "Inline sets are generated from scripts and quests (their name is literally `Item(Carrot,5)` or `Knowledge(...)`): one guaranteed target. "
              "Designed sets (`DropSet_*`) are the hand-made loot tables.", "",
              "## What entries point at", ""]
    lines += ["- %s: %d" % kv for kv in types.most_common()]
    lines += ["", "## Designed set families (first two name tokens)", ""]
    lines += ["- %s: %d" % kv for kv in prefixes.most_common(40)]
    lines += ["", "## Items", "",
              "- %d of %d items appear in at least one drop set; %d appear in a designed set." % (in_any, len(rows), in_designed),
              "", "## Field semantics", "",
              "- weight_pct: the entry's stored percent / 10,000 (1,000,000 = 100%). In every multi-entry set the entry percents sum exactly to the set's total_drop_rate, so they behave as weights.",
              "- share_pct: weight_pct / total, the entry's share of one roll of the set. Whether a set with a total below 100% can also yield nothing is not decided by the data alone.",
              "- total_rate_pct: the set's total_drop_rate / 10,000 (100% for 14,267 of 14,744 sets; 13% to 500% elsewhere).",
              "- min/max: quantity range; negative for friendly-point penalties.",
              "- enchant: enchant level rolled for gear entries (1 to 5), blank when the field is 65535.",
              "- roll_type / roll_count: the set's drop_roll_type and drop_roll_count bytes as stored; 0/0 for inline sets.",
              "- condition: conditioninfo string keys stored in the entry's condition slots (owner or player conditions).",
              "- need_slot: false when nee_slot_count is 65535.",
              "- tail: raw type-specific trailing bytes (mostly the target key repeated; friendly entries carry 32 bytes of reward data).", ""]
    with open(os.path.join(DATA, "dropset_summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("sets", len(out_sets), "entries", len(entry_rows), "items in any set", in_any, "in designed", in_designed)
    print("kinds", kinds.most_common()); print("types", types.most_common())
    unresolved = collections.Counter(r["result_label"] for r in entry_rows if not r["target"])
    print("unresolved targets by type:", unresolved.most_common())
    top = sorted(rows, key=lambda r: -r["n_designed_sets"])[:12]
    print("items in most designed sets:", [(r["string_key"], r["n_designed_sets"]) for r in top])


if __name__ == "__main__":
    main()
