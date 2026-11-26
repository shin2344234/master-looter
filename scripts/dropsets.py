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


# 1.0.0.3663 added a u8 to the set header and one to every entry, before the
# tail. Every one of them reads 0 in that build. The header byte sits after
# is_blocked or is_equal_percent, which read the same because both are always
# 0; this takes the second. parse_all decides the layout from the first sets.
SET_3663 = False

# 1.0.0.3840 took the 3663 bytes out again (the set header is the 3392 one, three
# bytes before nee_slot_count, and the entry has no byte before its tail) and cut the
# four u32 after result_type to seven bytes, which read as u32, u16 and u8 and are zero
# in every entry. parse_all decides this from the first sets as well.
SET_3840 = False

# 1.0.0.3840 stores an entry's conditions inline instead of as conditioninfo keys. An entry
# holds three refs and then a u32. A ref is one byte, 0 for none, or 1 followed by a byte
# that reads 1, a condition tree and three flag bytes. A tree node is a class byte. Classes
# 0 and 1 are binary operators (the game reads them as and/or) and each is followed by its
# two operands. Class 2 wraps one operand (not). Class 3 is a leaf, a kind byte and a payload
# whose size depends on the kind. The sizes below are the ones the 3840 data uses, and all
# 17,192 sets end exactly on their last byte with them. An unseen kind raises, so a new
# build fails loudly instead of misparsing.
LEAF_PAYLOAD_3840 = {0x08: 6, 0x0B: 8, 0x24: 11, 0x27: 18, 0x3E: 4, 0x84: 8, 0xA8: 8}
LEAF_NAMES_3840 = {0x24: "CheckKnowledge", 0x27: "CheckHaveItem", 0x3E: "CheckSpawnPositionRegion"}


def _read_node(r, depth=0):
    if depth > 64:
        raise ValueError("condition tree too deep at 0x%x" % r.p)
    c = r.u8("c.class")
    if c in (0, 1):
        a = _read_node(r, depth + 1)
        b = _read_node(r, depth + 1)
        return {"cls": c, "a": a, "b": b}
    if c == 2:
        return {"cls": 2, "a": _read_node(r, depth + 1)}
    if c == 3:
        kind = r.u8("c.kind")
        n = LEAF_PAYLOAD_3840.get(kind)
        if n is None:
            raise ValueError("unknown condition leaf kind 0x%02x at 0x%x" % (kind, r.p))
        pay = r.d[r.p:r.p + n]
        if len(pay) != n:
            raise ValueError("short condition leaf at 0x%x" % r.p)
        r.p += n
        return {"cls": 3, "kind": kind, "payload": pay}
    raise ValueError("unknown condition class %d at 0x%x" % (c, r.p))


def _read_ref(r):
    present = r.u8("c.ref")
    if present == 0:
        return None
    if present != 1 or r.u8("c.ref2") != 1:
        raise ValueError("bad condition ref at 0x%x" % r.p)
    tree = _read_node(r)
    flags = bytes(r.u8("c.flag") for _ in range(3))
    return {"tree": tree, "flags": flags}


def parse_entry(r, v3663=None):
    if v3663 is None:
        v3663 = SET_3663
    e = {}
    e["tag"] = r.u8("e.tag")
    e["key_raw"] = r.u64("e.key_raw")
    e["result_type"] = r.u8("e.type")
    if SET_3840:
        e["cond_tree"] = [_read_ref(r) for _ in range(3)]
        e["cond_tail"] = r.u32("e.cond_tail")
        e["cond"] = []
    else:
        e["cond"] = [r.u32("e.cond%d" % i) for i in range(4)]
    e["percent"] = r.u64("e.percent")
    e["sub_percent"] = r.u64("e.sub_percent")
    e["min"] = r.i64("e.min")
    e["max"] = r.i64("e.max")
    e["enchant_level"] = r.u16("e.enchant")
    if v3663:
        e["unk_3663"] = r.u8("e.unk_3663")
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


def parse_dropset(rec, trace=False, r=None, v3663=None):
    if v3663 is None:
        v3663 = SET_3663
    if r is None:
        r = Reader(rec, trace)
    d = {}
    d["string_key"] = r.cstr("string_key")
    d["key"] = r.u32("key")
    d["is_blocked"] = r.u8("is_blocked")
    d["is_equal_percent"] = r.u8("is_equal_percent")
    if v3663:
        d["unk_3663"] = r.u8("unk_3663")
    d["drop_roll_type"] = r.u8("drop_roll_type")
    d["nee_slot_count"] = r.u16("nee_slot_count")
    d["drop_tag_name_hash"] = r.u32("drop_tag_name_hash")
    d["drop_roll_count"] = r.u32("drop_roll_count")
    d["entries"] = r.array(lambda r: parse_entry(r, v3663), "entries")
    d["need_weight"] = r.u64("need_weight")
    d["total_drop_rate"] = r.u64("total_drop_rate")
    d["roll_dice_skip_rate"] = r.u64("roll_dice_skip_rate")
    d["drop_condition_string"] = r.u32("drop_condition_string")
    d["original_string"] = r.cstr("original_string")
    d["_end"] = r.p
    d["_len"] = len(rec)
    return d, r


def _fits(rec, v3663, v3840=False):
    global SET_3840
    keep = SET_3840
    SET_3840 = v3840
    try:
        _, r = parse_dropset(rec, v3663=v3663)
        return r.p == len(rec)
    except Exception:  # noqa
        return False
    finally:
        SET_3840 = keep


def detect(rows=None):
    """Which layout the extracted dropsetinfo holds. Also sets SET_3663, so a
    caller that reads entries embedded in other tables gets the same layout."""
    global SET_3663
    rows = rows if rows is not None else load_table("dropsetinfo")
    sample = [rec for _, rec in rows[:64]]
    global SET_3840
    score = {"3663": sum(_fits(x, True) for x in sample), "3392": sum(_fits(x, False) for x in sample),
             "3840": sum(_fits(x, False, True) for x in sample)}
    best = max(score, key=score.get)
    SET_3840 = best == "3840"
    SET_3663 = best == "3663"
    return SET_3663


def parse_all():
    global SET_3663
    out, bad = [], []
    rows = load_table("dropsetinfo")
    SET_3663 = detect(rows)
    for key, rec in rows:
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
