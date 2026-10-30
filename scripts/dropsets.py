"""Parser for dropsetinfo.staticinfobody (exe 1.0.0.3392, the DLC build).

Record layout, in the order the game's loader (RVA +0x16FC290) reads it and
verified against every record's byte boundary:
  string_key cstr, key u32, is_blocked u8, is_equal_percent u8, drop_roll_type u8,
  nee_slot_count u16, drop_tag_name_hash u32, drop_roll_count u32,
  entries CArray<Entry>, need_weight u64, total_drop_rate u64,
  roll_dice_skip_rate u64, drop_condition_string u32, original_string cstr

Before 3392 the key came first, the small fields were spread through the
record, and drop_condition_string was a string; it is a u32 reference now.
The entry body (RVA +0x1C45B20) reads the same bytes as before.

Entry:
  tag u8 (always 1), key_raw u64, result_type u8, four u32 (conditions/tag hash, zero so far),
  percent u64 (1,000,000 = 100%), sub_percent u64, min i64, max i64, enchant_level u16,
  then a tail whose size depends on result_type (see TAIL_SIZES).
"""
import collections
import json
import os
import sys

from cdtables import Reader, load_table, hexdump

# result_type -> tail size in bytes, from brute-forcing single-entry records against the boundary.
# On 3392 types 7, 8 and 15 share one reader (RVA +0x16ECAC0) that takes 34 bytes, two more than
# type 7 took before, and type 16 takes 10 where it took 6. Type 14, a u16 global game event
# and a u8 state, appears only in two DLC sets.
TAIL_SIZES = {0: 4, 1: 4, 3: 4, 4: 4, 5: 4, 6: 4, 7: 34, 9: 4, 10: 8, 13: 5, 14: 3, 15: 34, 16: 10}

RESULT_TYPE_NAMES = {
    0: "item", 1: "type1", 3: "type3", 4: "knowledge", 5: "type5", 6: "type6", 7: "friendly",
    9: "type9", 10: "type10", 13: "gameplay-variable", 14: "global-game-event", 15: "type15", 16: "type16",
}


def parse_entry(r):
    e = {}
    e["tag"] = r.u8("e.tag")
    e["key_raw"] = r.u64("e.key_raw")
    e["result_type"] = r.u8("e.type")
    e["cond"] = [r.u32("e.cond%d" % i) for i in range(4)]
    e["percent"] = r.u64("e.percent")
    e["sub_percent"] = r.u64("e.sub_percent")
    e["min"] = r.i64("e.min")
    e["max"] = r.i64("e.max")
    e["enchant_level"] = r.u16("e.enchant")
    t = TAIL_SIZES.get(e["result_type"])
    if t is None:
        raise ValueError("unknown result_type %d at 0x%x" % (e["result_type"], r.p))
    e["tail"] = r.d[r.p:r.p + t]
    r.p += t
    if r.trace is not None:
        r.trace.append(("e.tail", r.p - t, r.p, e["tail"].hex()))
    if t >= 4:
        e["tail_key"] = int.from_bytes(e["tail"][:4], "little")
    return e


def parse_dropset(rec, trace=False, r=None):
    if r is None:
        r = Reader(rec, trace)
    d = {}
    d["string_key"] = r.cstr("string_key")
    d["key"] = r.u32("key")
    d["is_blocked"] = r.u8("is_blocked")
    d["is_equal_percent"] = r.u8("is_equal_percent")
    d["drop_roll_type"] = r.u8("drop_roll_type")
    d["nee_slot_count"] = r.u16("nee_slot_count")
    d["drop_tag_name_hash"] = r.u32("drop_tag_name_hash")
    d["drop_roll_count"] = r.u32("drop_roll_count")
    d["entries"] = r.array(parse_entry, "entries")
    d["need_weight"] = r.u64("need_weight")
    d["total_drop_rate"] = r.u64("total_drop_rate")
    d["roll_dice_skip_rate"] = r.u64("roll_dice_skip_rate")
    d["drop_condition_string"] = r.u32("drop_condition_string")
    d["original_string"] = r.cstr("original_string")
    d["_end"] = r.p
    d["_len"] = len(rec)
    return d, r


def parse_all():
    out, bad = [], []
    for key, rec in load_table("dropsetinfo"):
        try:
            d, r = parse_dropset(rec)
            if r.p != len(rec):
                bad.append((key, rec, "end 0x%x != len 0x%x" % (r.p, len(rec))))
            out.append(d)
        except Exception as e:  # noqa
            bad.append((key, rec, repr(e)))
    return out, bad


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sets, bad = parse_all()
    print("parsed", len(sets), "bad", len(bad))
    for key, rec, why in bad[:4]:
        print("=" * 90); print("key", key, "len", len(rec), why)
        tr = Reader(rec, True)
        try:
            parse_dropset(rec, trace=True, r=tr)
        except Exception as e:  # noqa
            print("  exception:", repr(e))
        for name, s, e, v in tr.trace[-30:]:
            print("  %-28s 0x%04x-0x%04x %s" % (name, s, e, repr(v)[:70]))
        pos = tr.trace[-1][2] if tr.trace else 0
        print(hexdump(rec, max(0, pos - 48), min(len(rec), pos + 120)))
    good = [d for d in sets if d["_end"] == d["_len"]]
    ents = [e for d in good for e in d["entries"]]
    print("entries", len(ents))
    print("result types:", collections.Counter(e["result_type"] for e in ents).most_common())
    print("tag:", collections.Counter(e["tag"] for e in ents).most_common(3))
    print("cond nonzero:", sum(1 for e in ents if any(e["cond"])), "examples", [(e["cond"], e["result_type"]) for e in ents if any(e["cond"])][:5])
    print("tail_key == key_raw:", sum(1 for e in ents if e.get("tail_key") == e["key_raw"]), "of", len(ents))
    print("roll_type:", collections.Counter(d["drop_roll_type"] for d in good).most_common())
    print("roll_count:", collections.Counter(d["drop_roll_count"] for d in good).most_common(8))
    print("total_drop_rate:", collections.Counter(d["total_drop_rate"] for d in good).most_common(8))
    print("is_equal_percent:", collections.Counter(d["is_equal_percent"] for d in good).most_common())
    print("nee_slot_count:", collections.Counter(d["nee_slot_count"] for d in good).most_common(5))
    print("need_weight:", collections.Counter(d["need_weight"] for d in good).most_common(5))
    print("skip_rate:", collections.Counter(d["roll_dice_skip_rate"] for d in good).most_common(5))
    print("condition strings:", collections.Counter(d["drop_condition_string"] for d in good).most_common(6))
    print("tag hash:", collections.Counter(d["drop_tag_name_hash"] for d in good).most_common(6))
    print("percent values:", collections.Counter(e["percent"] for e in ents).most_common(12))
    print("sub_percent values:", collections.Counter(e["sub_percent"] for e in ents).most_common(6))
    print("enchant:", collections.Counter(e["enchant_level"] for e in ents).most_common(6))
    print("entries per set:", collections.Counter(len(d["entries"]) for d in good).most_common(12))
