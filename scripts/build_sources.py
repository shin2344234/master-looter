"""Cross-reference drop sets to the characters and gimmicks that use them (exe 1.0.0.3392).

The CharacterInfo and GimmickInfo record layouts are not fully known, so the drop-related
blocks are located by validated signature scans inside each record (every referenced key
must exist in its table, counts must be small, percent fields must be <= 200%):

  character reward list   u32 n, then n x {drop_set u32, reward_tag_type_flag u32, repeat_count u32}
  character equipment     u32 x, u32 n, then n x {item u32, drop_set u32 (always 0), 7 x u64 percents}
                          percents: dead_drop, throw_drop, min/max endurance, enhanced, min/max enhanced endurance
  character catch/steal   inline drop set u32 followed by 01 00 00 00 00 (2976 and earlier; not found on 3392)
  gimmick drop block      u8 present, u32 n, n x {u32 drop_set, u32}, u8 present, u32 m, m x {flagged entry, u32},
                          flagged buyable entry (see gimmick_drop_block)

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
    # Offset 0 is the 1.0.0.3392 layout, string key first; the rest are the
    # widths of a key that came before it on earlier builds.
    rows = load_table(name)
    best, best_n = None, -1
    for width in (0, 4, 2, 8, 1):
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


CATCH_NOT_DERIVED = ("not derived on this build; catch_sets finds no anchor in the 1.0.0.3392 CharacterInfo "
                     "layout, so the catch_sets column of character_drops.csv is empty")


def catch_sets(rec, u, inlinekeys):
    # Written for 2976 and earlier. On 1.0.0.3392 CatchSpawnData is {u32 preset
    # hash, u32 gimmick, u32 drop set} (RVA +0x16F2AB0) with nothing after it to
    # anchor on, and a bare match on an inline set key hits 7099 of 7315
    # characters, so this finds nothing there. When it finds nothing in the
    # whole table, main() reports the column as not derived, not as zero.
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


def flagged_entry(r):
    """One drop entry behind its presence byte, as RVA +0x24A69A0 reads it on 3392.
    None when the byte is 0. The byte is what parse_entry calls the tag."""
    if r.d[r.p] == 0:
        r.p += 1
        return None
    e = parse_entry(r)
    if e["tag"] != 1 or e["percent"] > 10 ** 8 or e["min"] > e["max"]:
        raise ValueError
    return e


def gimmick_drop_block(rec, dskeys, itemkeys):
    """The drop lists of a 1.0.0.3392 GimmickInfo record (loader RVA +0x1705740).

    Three fields in a row: _dropSetInfoList (u8 present, u32 n, n x {u32 drop set,
    u32}), _dropInfoDataList (u8 present, u32 m, m x {flagged entry, u32}) and
    _buyableDropItem (one flagged entry). _dropRollCount and _dropOffsetSocketName
    sit far from them in this build, with nothing to anchor on, so the block no
    longer carries them; roll_count and socket come back empty."""
    L = len(rec); best = None
    for p in range(0, L - 3):
        if rec[p] > 1:
            continue
        try:
            r = Reader(rec); r.p = p
            keys = []
            if r.u8():
                n = r.u32()
                if n == 0 or n > 32 or r.p + 8 * n > L:
                    continue
                for _ in range(n):
                    k, extra = r.u32(), r.u32()
                    if k not in dskeys or extra > 0xFFFF:
                        raise ValueError
                    keys.append(k)
            ents = []
            if r.u8():
                m = r.u32()
                if m == 0 or m > 64:
                    continue
                for _ in range(m):
                    e = flagged_entry(r)
                    if e is None:
                        raise ValueError
                    r.u32()
                    ents.append(e)
            buy_e = flagged_entry(r)
        except (ValueError, IndexError, struct.error):
            continue
        # Every one of the 314 blocks found on 2976 named at least one drop set.
        # With the set list's presence byte at 0 the anchor is a lone zero:
        # 14,395 records then match with no entries at all, 616 match a list of
        # 0% item entries that never pays, and 55 gate rows match entries whose
        # item key is 0. So a block with no set is taken only when it pays a
        # real item at a real chance, which is gimmick_ocean_chest_0001 and
        # dff_chest_06_index0002, each a broken sea coin at 100%.
        if not keys and not any(e["result_type"] == 0 and e["percent"] > 0 and e.get("tail_key") in itemkeys
                                for e in ents):
            continue
        buy = buy_e.get("tail_key", 0) if buy_e and buy_e["result_type"] == 0 else 0
        if buy and buy not in itemkeys:
            continue
        cand = dict(roll_count="", sets=keys, socket="", buyable=buy, entries=ents)
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
    catch_txt = str(stat["with_catch"]) if stat["with_catch"] else CATCH_NOT_DERIVED
    lines = ["# Drop sources, exe 1.0.0.3392", "",
             "- characters: %d; with reward drop sets: %d; with equipment: %d; with a catch/steal set: %s" % (stat["chars"], stat["with_rewards"], stat["with_equipment"], catch_txt),
             "- gimmicks: %d; with a drop block: %d" % (stat["gimmicks"], stat["gimmicks_with_drops"]),
             "- drop sets used by at least one character or gimmick: %d of %d" % (len(src_rows), len(sets)),
             "- items with at least one known source (set via character or gimmick, worn equipment%s): %d of %d" % (", or catch" if stat["with_catch"] else "", any_src, len(item_rows)), "",
             "## How each source was found", "",
             "Character records hold a reward list (count, then triples of drop set, reward flag, repeat count) and an equipment list (count, then 64-byte entries of item, unused set key and seven percent fields). Gimmick records hold a flagged list of drop set keys, a flagged list of inline entries and a flagged buyable entry; the roll count and drop socket sit elsewhere in the record on this build and are not read. Each block was located by requiring every referenced key to exist in its table.", "",
             "## Equipment drops", "",
             "NPC gear never points at a drop set: the worn item itself drops. The first percent field is the death-drop chance (3% to 5% for most gear, 0 for players), the second the throw-drop chance, then min/max endurance of the dropped piece (15% to 20%, which is why it arrives damaged), the enhanced chance (5%) and min/max enhanced endurance.", "",
             "## Reward flag values by set family (top 20)", ""]
    lines += ["- 0x%x on %s: %d" % (flag, fam, n) for (flag, fam), n in flags.most_common(20)]
    lines += ["", "Flags are the stored reward_tag_type_flag bitfields. 0x400 series co-occur with common death drops, 0x800 with favorite-item gifts; the exact bit meanings are not decoded.", ""]
    with open(os.path.join(DATA, "sources_summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(dict(stat)); print("sets with users", len(src_rows), "items with source", any_src)
    if not stat["with_catch"]:
        print("catch/steal sets:", CATCH_NOT_DERIVED)
    print("top used sets:", [(r["set_name"], r["n_characters"], r["n_gimmicks"]) for r in src_rows[:8]])


if __name__ == "__main__":
    main()
