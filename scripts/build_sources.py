"""Cross-reference drop sets to the characters and gimmicks that use them (Crimson Desert 2.01.00).

The CharacterInfo and GimmickInfo record layouts are not fully known, so the drop-related
blocks are located by validated signature scans inside each record (every referenced key
must exist in its table, counts must be small, percent fields must be <= 200%):

  character reward list   u32 n, then n x {drop_set u32, reward_tag_type_flag u32, repeat_count u32}
  character equipment     u32 x, u32 n, then n x {item u32, drop_set u32 (always 0), 7 x u64 percents}
                          percents: dead_drop, throw_drop, min/max endurance, enhanced, min/max enhanced endurance
  character catch/steal   inline drop set u32 followed by 01 00 00 00 00 (what catching or searching the actor gives)
  gimmick drop block      u32 roll_count, f32, u32 n, n x u64 drop_set, cstr socket, u32 buyable item, u32 m, m entries

Writes to ../data: character_drops.csv, gimmick_drops.csv, dropset_sources.csv, item_sources.csv,
sources.json (per-set users for the review page), sources_summary.md.
"""
import collections
import csv
import json
import os
import struct
import sys

from cdtables import LOCDIR, Reader, load_paloc, load_table
from dropsets import parse_entry

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")


def keyed(name):
    rows = load_table(name)
    best, best_n = None, -1
    for width in (4, 2, 8, 1):
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
    return best


def paloc_sub(path, sub):
    d = load_paloc(path)
    return {k >> 32: v for k, v in d.items() if (k & 0xffffffff) == sub}


def u32_at(rec):
    L = len(rec)
    return [struct.unpack_from("<I", rec, o)[0] for o in range(L - 3)]


# ----------------------------------------------------------------------------- character scans
def reward_lists(rec, u, dskeys):
    L = len(rec); found = []
    for p in range(0, L - 16):
        n = u[p]
        if n == 0 or n > 24 or p + 4 + 12 * n > L:
            continue
        trip = []
        for i in range(n):
            q = p + 4 + 12 * i
            key, flag, rep = u[q], u[q + 4], u[q + 8]
            if key not in dskeys or flag > 0xFFFF or rep == 0 or rep > 200:
                trip = None; break
            trip.append((key, flag, rep))
        if trip:
            found.append((p, trip))
    found.sort(key=lambda x: (x[0], -len(x[1])))
    kept = []
    for p, trip in found:
        if kept and p < kept[-1][0] + 4 + 12 * len(kept[-1][1]):
            continue
        kept.append((p, trip))
    return kept


def equip_list(rec, u, itemkeys, dskeys):
    L = len(rec)
    for p in range(0, L - 72):
        n = u[p + 4]
        if n == 0 or n > 32 or p + 8 + 64 * n > L:
            continue
        ents = []
        for i in range(n):
            q = p + 8 + 64 * i
            it, dset = u[q], u[q + 4]
            if it not in itemkeys or it < 100 or (dset and dset not in dskeys):
                ents = None; break
            vals = struct.unpack_from("<7Q", rec, q + 8)
            if any(v > 2_000_000 for v in vals):
                ents = None; break
            ents.append((it, dset, vals))
        if ents:
            return ents
    return []


def catch_sets(rec, u, inlinekeys):
    out = []
    for p in range(0, len(rec) - 13):
        if u[p] in inlinekeys and rec[p + 4:p + 9] == b"\x01\x00\x00\x00\x00":
            out.append((u[p], u[p + 9]))
    return out


# ----------------------------------------------------------------------------- gimmick scan
def cstr_len(rec, p, maxlen=64):
    if p + 4 > len(rec):
        return None
    l = struct.unpack_from("<I", rec, p)[0]
    if l > maxlen or p + 4 + l > len(rec):
        return None
    if any(c < 32 or c > 126 for c in rec[p + 4:p + 4 + l]):
        return None
    return l


def gimmick_drop_block(rec, dskeys, itemkeys):
    L = len(rec); best = None
    for p in range(0, L - 24):
        rollc, = struct.unpack_from("<I", rec, p)
        if rollc > 50:
            continue
        n, = struct.unpack_from("<I", rec, p + 8)
        if n > 32 or p + 12 + 8 * n > L:
            continue
        keys = []
        for i in range(n):
            v, = struct.unpack_from("<Q", rec, p + 12 + 8 * i)
            if v not in dskeys:
                keys = None; break
            keys.append(v)
        if keys is None:
            continue
        q = p + 12 + 8 * n
        l = cstr_len(rec, q)
        if l is None:
            continue
        socket = rec[q + 4:q + 4 + l].decode("ascii")
        q += 4 + l
        if q + 8 > L:
            continue
        buy, m = struct.unpack_from("<II", rec, q)
        if (buy and buy not in itemkeys) or m > 64:
            continue
        q += 8
        ents = []
        try:
            r = Reader(rec); r.p = q
            for i in range(m):
                e = parse_entry(r)
                if e["tag"] != 1 or e["percent"] > 10 ** 8 or e["min"] > e["max"]:
                    raise ValueError
                ents.append(e)
        except Exception:
            continue
        if n == 0 and m == 0:
            continue
        cand = dict(roll_count=rollc, sets=keys, socket=socket, buyable=buy, entries=ents)
        if best is None or len(keys) + len(ents) > len(best["sets"]) + len(best["entries"]):
            best = cand
    return best


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    items = keyed("iteminfo")
    item_en = paloc_sub(os.path.join(LOCDIR, "item.paloc"), 0x70)
    tagged = {int(r["key"]): r for r in csv.DictReader(open(os.path.join(DATA, "items_tagged.csv"), encoding="utf-8-sig"))}
    sets = {s["key"]: s for s in json.load(open(os.path.join(DATA, "dropsets.json"), encoding="utf-8"))}
    dskeys = set(sets)
    inlinekeys = {k for k, s in sets.items() if s["kind"] == "inline item"}
    itemkeys = set(items)
    chars = keyed("characterinfo")
    char_en = paloc_sub(os.path.join(LOCDIR, "character.paloc"), 48)
    gims = keyed("gimmickinfo")
    gim_en = paloc_sub(os.path.join(LOCDIR, "gimmick.paloc"), 512)

    def setname(k):
        return sets[k]["string_key"] if k in sets else str(k)

    def set_items(k):
        return [e for e in sets[k]["entries"] if e["result_label"] == "item"] if k in sets else []

    # ---- characters
    char_rows = []
    set_users_c = collections.defaultdict(list)   # set key -> [(char key, flag, rep, source)]
    equipped = collections.defaultdict(list)       # item key -> [(char key, dead%, throw%)]
    stat = collections.Counter()
    for key, rec in load_table("characterinfo"):
        u = u32_at(rec)
        lists = reward_lists(rec, u, dskeys)
        eq = equip_list(rec, u, itemkeys, dskeys)
        catch = catch_sets(rec, u, inlinekeys)
        rewards = lists[0][1] if lists else []
        extra = [t for p, trip in lists[1:] for t in trip]
        stat["chars"] += 1
        stat["with_rewards"] += bool(rewards); stat["with_equipment"] += bool(eq); stat["with_catch"] += bool(catch)
        for k2, flag, rep in rewards:
            set_users_c[k2].append((key, flag, rep, "reward"))
        for k2, flag, rep in extra:
            set_users_c[k2].append((key, flag, rep, "extra"))
        for k2, h in catch:
            set_users_c[k2].append((key, 0, 1, "catch"))
        for it, dset, vals in eq:
            equipped[it].append((key, vals[0] / 10000.0, vals[1] / 10000.0))
        via_items = collections.Counter()
        for k2, flag, rep in rewards:
            for e in set_items(k2):
                via_items[e["target"]] += e["share_pct"] if e["share_pct"] != "" else 0
        char_rows.append(dict(
            key=key, string_key=chars.get(key, ""), name=char_en.get(key, ""),
            n_reward_sets=len(rewards),
            reward_sets="; ".join("%s (flag 0x%x, x%d)" % (setname(k2), flag, rep) for k2, flag, rep in rewards),
            extra_sets="; ".join("%s (flag 0x%x, x%d)" % (setname(k2), flag, rep) for k2, flag, rep in extra),
            catch_sets="; ".join(setname(k2) for k2, h in catch),
            n_equipment=len(eq),
            equipment="; ".join("%s (drop %g%%, throw %g%%)" % (items.get(it, it), vals[0] / 10000.0, vals[1] / 10000.0) for it, dset, vals in eq),
            top_items_from_sets="; ".join("%s (%g%%)" % (n, round(v, 2)) for n, v in via_items.most_common(8)),
        ))
    with open(os.path.join(DATA, "character_drops.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(char_rows[0].keys())); w.writeheader(); w.writerows(char_rows)

    # ---- gimmicks
    gim_rows = []
    set_users_g = collections.defaultdict(list)
    for key, rec in load_table("gimmickinfo"):
        blk = gimmick_drop_block(rec, dskeys, itemkeys)
        stat["gimmicks"] += 1
        if not blk:
            continue
        stat["gimmicks_with_drops"] += 1
        for k2 in blk["sets"]:
            set_users_g[k2].append(key)
        gim_rows.append(dict(
            key=key, string_key=gims.get(key, ""), name=gim_en.get(key, ""),
            roll_count=blk["roll_count"], n_sets=len(blk["sets"]),
            drop_sets="; ".join(setname(k2) for k2 in blk["sets"]),
            buyable_item=items.get(blk["buyable"], "") if blk["buyable"] else "",
            inline_entries="; ".join("%s x%d-%d (%g%%)" % (items.get(e["key_raw"], e["key_raw"]), e["min"], e["max"], e["percent"] / 10000.0) for e in blk["entries"]),
            items="; ".join(sorted({e["target"] for k2 in blk["sets"] for e in set_items(k2)})[:20]),
        ))
    with open(os.path.join(DATA, "gimmick_drops.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(gim_rows[0].keys())); w.writeheader(); w.writerows(gim_rows)

    # ---- per set
    def cname(k):
        return char_en.get(k) or chars.get(k, str(k))

    def gname(k):
        return gim_en.get(k) or gims.get(k, str(k))

    src_rows = []; page = {}
    for k, s in sets.items():
        cu = set_users_c.get(k, []); gu = set_users_g.get(k, [])
        if not cu and not gu:
            continue
        cn = collections.Counter(cname(c) for c, *_ in cu); gn = collections.Counter(gname(g) for g in gu)
        src_rows.append(dict(set_key=k, set_name=s["string_key"], kind=s["kind"],
                             n_characters=len({c for c, *_ in cu}), n_gimmicks=len(set(gu)),
                             characters=" | ".join("%s%s" % (n, " x%d" % c if c > 1 else "") for n, c in cn.most_common(12)),
                             gimmicks=" | ".join("%s%s" % (n, " x%d" % c if c > 1 else "") for n, c in gn.most_common(12))))
        page[k] = dict(c=len({c for c, *_ in cu}), g=len(set(gu)), cn=[n for n, _ in cn.most_common(8)], gn=[n for n, _ in gn.most_common(8)])
    src_rows.sort(key=lambda r: -(r["n_characters"] + r["n_gimmicks"]))
    with open(os.path.join(DATA, "dropset_sources.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(src_rows[0].keys())); w.writeheader(); w.writerows(src_rows)
    with open(os.path.join(DATA, "sources.json"), "w", encoding="utf-8") as f:
        json.dump(page, f, ensure_ascii=False, separators=(",", ":"))

    # ---- per item
    item_rows = []
    for k, r in tagged.items():
        via_c = collections.Counter(); via_g = collections.Counter(); best = 0.0
        for sk, s in sets.items():
            for e in s["entries"]:
                if e["result_label"] == "item" and e["target_key"] == k:
                    share = e["share_pct"] if e["share_pct"] != "" else 0
                    for c, flag, rep, src in set_users_c.get(sk, []):
                        via_c[cname(c)] = max(via_c[cname(c)], share)
                    for g in set_users_g.get(sk, []):
                        via_g[gname(g)] = max(via_g[gname(g)], share)
                    if set_users_c.get(sk) or set_users_g.get(sk):
                        best = max(best, share)
        eq = equipped.get(k, [])
        item_rows.append(dict(
            key=k, string_key=r["string_key"], name=r["name"], klass=r["klass"],
            n_characters_via_sets=len(via_c), n_gimmicks_via_sets=len(via_g), n_equipped_by=len(eq),
            best_share_pct=best if best else "",
            max_equip_drop_pct=max((d for _, d, t in eq), default=""),
            characters=" | ".join("%s (%g%%)" % (n, round(v, 2)) for n, v in via_c.most_common(10)),
            gimmicks=" | ".join("%s (%g%%)" % (n, round(v, 2)) for n, v in via_g.most_common(10)),
            equipped_by=" | ".join("%s (%g%%)" % (cname(c), d) for c, d, t in sorted(eq, key=lambda x: -x[1])[:10]),
        ))
    with open(os.path.join(DATA, "item_sources.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(item_rows[0].keys())); w.writeheader(); w.writerows(item_rows)

    # ---- summary
    flags = collections.Counter((flag, sets[k]["string_key"].split("_")[1] if "_" in sets[k]["string_key"] else sets[k]["string_key"][:10]) for k, us in set_users_c.items() for c, flag, rep, src in us if src == "reward")
    any_src = sum(1 for r in item_rows if r["n_characters_via_sets"] or r["n_gimmicks_via_sets"] or r["n_equipped_by"])
    lines = ["# Drop sources, build 2.01.00", "",
             "- characters: %d; with reward drop sets: %d; with equipment: %d; with a catch/steal set: %d" % (stat["chars"], stat["with_rewards"], stat["with_equipment"], stat["with_catch"]),
             "- gimmicks: %d; with a drop block: %d" % (stat["gimmicks"], stat["gimmicks_with_drops"]),
             "- drop sets used by at least one character or gimmick: %d of %d" % (len(src_rows), len(sets)),
             "- items with at least one known source (set via character or gimmick, worn equipment, or catch): %d of %d" % (any_src, len(item_rows)), "",
             "## How each source was found", "",
             "Character records hold a reward list (count, then triples of drop set, reward flag, repeat count), an equipment list (count, then 64-byte entries of item, unused set key and seven percent fields) and, for catchable animals and searchable people, an inline drop set followed by 01 00 00 00 00. Gimmick records hold roll count, a list of 64-bit drop set keys, a socket name, a buyable item and inline entries. Each block was located by requiring every referenced key to exist in its table.", "",
             "## Equipment drops", "",
             "NPC gear never points at a drop set: the worn item itself drops. The first percent field is the death-drop chance (3% to 5% for most gear, 0 for players), the second the throw-drop chance, then min/max endurance of the dropped piece (15% to 20%, which is why it arrives damaged), the enhanced chance (5%) and min/max enhanced endurance.", "",
             "## Reward flag values by set family (top 20)", ""]
    lines += ["- 0x%x on %s: %d" % (flag, fam, n) for (flag, fam), n in flags.most_common(20)]
    lines += ["", "Flags are the stored reward_tag_type_flag bitfields. 0x400 series co-occur with common death drops, 0x800 with favorite-item gifts; the exact bit meanings are not decoded.", ""]
    with open(os.path.join(DATA, "sources_summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(dict(stat)); print("sets with users", len(src_rows), "items with source", any_src)
    print("top used sets:", [(r["set_name"], r["n_characters"], r["n_gimmicks"]) for r in src_rows[:8]])


if __name__ == "__main__":
    main()
