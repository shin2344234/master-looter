"""Readers for Crimson Desert static-info tables (ItemInfo layout for exe 1.0.0.3392).

Inputs are the files extracted from archive group 0008 (gamedata/*.staticinfobody +
*.staticinfoheader) and the English string tables from group 0020 (gamedata/*.paloc).

The ItemInfo layout was rebuilt in the patch after 1.0.0.2976 and follows the game's own
loader at RVA +0x17133B0 of exe 1.0.0.3392 (the string key comes first, then the u32 key;
table references are u32 keys on disk; most lists carry a u8 presence flag). It no longer
reads the 2976 layout. Every record is parsed against the byte range the header gives it,
so a layout error shows up as an end-offset mismatch.

The DLC of 1.0.0.3392 ships its own rows for many tables, <name>.dlc_01.staticinfo*, and its
own string tables, <name>.dlc_01.paloc, in dlc/dlc_01 of the install. Extracted into
extracted-dlc_01 beside the base extraction, load_table and load_paloc add them by default:
DLC-only rows after the base rows, and the base record wherever a key is in both (the 242
items the DLC repeats differ only in faction pricing and group lists). Nobody has yet shown
where the game puts DLC rows in a runtime table, so a DLC-only row has no runtime row id;
callers that number rows by header position pass dlc=False.
"""
import os
import struct
import sys

EXTRACTED = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "extracted")
GAMEDATA = os.path.join(EXTRACTED, "0008", "gamedata")
LOCDIR = os.path.join(EXTRACTED, "0020", "gamedata")
DLC = "dlc_01"
DLC_EXTRACTED = EXTRACTED + "-" + DLC
DLC_GAMEDATA = os.path.join(DLC_EXTRACTED, "0008", "gamedata")
DLC_LOCDIR = os.path.join(DLC_EXTRACTED, "0020", "gamedata")


# ----------------------------------------------------------------------------- paloc
def load_paloc(path, dlc=True):
    """Return {int_key: text}. Entry: u64 category, u32 keylen, key, u32 vallen, val.
    Trailing u32 = entry count. Item strings are keyed (item_key << 32) | sub, with
    sub 0x70 = name, 0x71 = description, 0x72 = description 2. A base string table
    also takes the DLC's strings for keys it lacks."""
    out = _load_paloc(path)
    if dlc and os.path.dirname(os.path.abspath(path)) == os.path.abspath(LOCDIR):
        stem = os.path.basename(path)[:-len(".paloc")]
        extra = os.path.join(DLC_LOCDIR, "%s.%s.paloc" % (stem, DLC))
        if os.path.exists(extra):
            for k, v in _load_paloc(extra).items():
                out.setdefault(k, v)
    return out


def _load_paloc(path):
    d = open(path, "rb").read()
    cnt = struct.unpack_from("<I", d, len(d) - 4)[0]
    off = 0
    out = {}
    for _ in range(cnt):
        off += 8
        kl, = struct.unpack_from("<I", d, off); off += 4
        key = d[off:off + kl].decode("utf-8", "replace"); off += kl
        vl, = struct.unpack_from("<I", d, off); off += 4
        val = d[off:off + vl].decode("utf-8", "replace"); off += vl
        out[int(key)] = val
    assert off == len(d) - 4, (path, off, len(d))
    return out


# ----------------------------------------------------------------------------- tables
UINT_COUNT_TABLES = {
    "characterappearanceindexinfo", "globalstagesequencerinfo", "sequencerspawninfo",
    "sheetmusicinfo", "spawningpoolautospawninfo", "itemuseinfo", "terrainregionautospawninfo",
    "textguideinfo", "validscheduleaction", "stageinfo", "questinfo", "gimmickeventtableinfo",
    "reviepointinfo", "aidialogstringinfo", "dialogsetinfo", "vibratepatterninfo",
    "platformachievementinfo", "levelgimmicksceneobjectinfo", "fieldlevelnametableinfo",
    "levelinfo", "board", "gameplaytrigger", "characterchange", "materialrelationinfo",
}


def load_table(name, gamedata=GAMEDATA, dlc=True):
    """Return list of (key, record_bytes) in header order, then the DLC's rows for keys
    the base table lacks when reading the base extraction (see the module docstring)."""
    rows = _load_table(name, gamedata)
    if dlc and os.path.abspath(gamedata) == os.path.abspath(GAMEDATA):
        extra = "%s.%s" % (name, DLC)
        if os.path.exists(os.path.join(DLC_GAMEDATA, extra + ".staticinfoheader")):
            have = {k for k, _ in rows}
            rows += [(k, r) for k, r in _load_table(extra, DLC_GAMEDATA, name) if k not in have]
    return rows


def dlc_keys(name):
    """Keys present only in the DLC's rows of a table."""
    base = {k for k, _ in load_table(name, dlc=False)}
    return {k for k, _ in load_table(name) if k not in base}


def _load_table(name, gamedata, kind=None):
    kind = kind or name
    h = open(os.path.join(gamedata, name + ".staticinfoheader"), "rb").read()
    cs = 4 if kind in UINT_COUNT_TABLES else 2
    cnt = int.from_bytes(h[:cs], "little")
    rest = len(h) - cs
    if cnt == 0 and rest == 0:
        return []   # the DLC ships a bare count of zero, with no body file, for tables it adds nothing to
    b = open(os.path.join(gamedata, name + ".staticinfobody"), "rb").read()
    assert cnt and rest % cnt == 0, (name, cnt, rest)
    ks = rest // cnt - 4
    assert ks in (1, 2, 4, 8), (name, ks)
    ents = []
    p = cs
    for _ in range(cnt):
        key = int.from_bytes(h[p:p + ks], "little")
        off = struct.unpack_from("<I", h, p + ks)[0]
        ents.append((key, off))
        p += ks + 4
    offs = sorted(o for _, o in ents) + [len(b)]
    nxt = {o: offs[i + 1] for i, o in enumerate(offs[:-1])}
    return [(k, b[o:nxt[o]]) for k, o in ents]


# ----------------------------------------------------------------------------- reader
class Reader:
    __slots__ = ("d", "p", "trace")

    def __init__(self, data, trace=False):
        self.d = data
        self.p = 0
        self.trace = [] if trace else None

    def _rec(self, name, start, val):
        if self.trace is not None:
            self.trace.append((name, start, self.p, val))

    def u8(self, n=""):
        v = self.d[self.p]; s = self.p; self.p += 1; self._rec(n, s, v); return v

    def i8(self, n=""):
        v = struct.unpack_from("<b", self.d, self.p)[0]; s = self.p; self.p += 1; self._rec(n, s, v); return v

    def u16(self, n=""):
        v = struct.unpack_from("<H", self.d, self.p)[0]; s = self.p; self.p += 2; self._rec(n, s, v); return v

    def u32(self, n=""):
        v = struct.unpack_from("<I", self.d, self.p)[0]; s = self.p; self.p += 4; self._rec(n, s, v); return v

    def i32(self, n=""):
        v = struct.unpack_from("<i", self.d, self.p)[0]; s = self.p; self.p += 4; self._rec(n, s, v); return v

    def u64(self, n=""):
        v = struct.unpack_from("<Q", self.d, self.p)[0]; s = self.p; self.p += 8; self._rec(n, s, v); return v

    def i64(self, n=""):
        v = struct.unpack_from("<q", self.d, self.p)[0]; s = self.p; self.p += 8; self._rec(n, s, v); return v

    def f32(self, n=""):
        v = struct.unpack_from("<f", self.d, self.p)[0]; s = self.p; self.p += 4; self._rec(n, s, v); return v

    def cstr(self, n=""):
        s = self.p
        l = struct.unpack_from("<I", self.d, self.p)[0]
        if l > len(self.d) - self.p - 4:
            raise ValueError("cstr len %d at 0x%x (%s)" % (l, self.p, n))
        v = self.d[self.p + 4:self.p + 4 + l].decode("utf-8", "replace")
        self.p += 4 + l
        self._rec(n, s, v)
        return v

    def locstr(self, n=""):
        s = self.p
        cat = self.d[self.p]
        idx = struct.unpack_from("<Q", self.d, self.p + 1)[0]
        self.p += 9
        dflt = self.cstr()
        v = (cat, idx, dflt)
        if self.trace is not None:
            self.trace.pop()
        self._rec(n, s, v)
        return v

    def array(self, fn, n=""):
        s = self.p
        cnt = struct.unpack_from("<I", self.d, self.p)[0]
        if cnt > len(self.d) - self.p:
            raise ValueError("array count %d at 0x%x (%s)" % (cnt, self.p, n))
        self.p += 4
        self._rec(n + ".count", s, cnt)
        return [fn(self) for _ in range(cnt)]

    def optional(self, fn, n=""):
        flag = self.u8(n + ".flag")
        return fn(self) if flag else None


# ----------------------------------------------------------------------------- sub-structs
# Layout from the ItemInfo loader at RVA +0x17133B0 of exe 1.0.0.3392 and the helpers it
# calls. A table reference is a u32 key on disk whatever its width in memory, so every
# reference below reads four bytes. Most list helpers read a u8 presence flag first and
# the array (u32 count, then elements) only when it is set; a few read the count directly.
def u32_list(r, n=""):
    return r.array(lambda r: r.u32(), n)


def flag_array(r, fn, n=""):
    """u8 presence flag, then a counted array when set (empty list when not)."""
    return r.array(fn, n) if r.u8(n + ".flag") else []


def flag_u32_list(r, n=""):
    return flag_array(r, lambda r: r.u32(), n)


def occupied_equip_slot(r):
    return dict(equip_slot_name_key=r.u32("oes.key"), index_list=r.array(lambda r: r.u8(), "oes.idx"))


def item_icon(r):
    return dict(icon_path=r.u32("icon.path"), highlight_icon_path=r.u32("icon.hl"),
                check_usable=r.u8("icon.usable"),
                gimmick_state_list=u32_list(r, "icon.states"),
                check_exist_sealed_data=r.u8("icon.sealed"))


def passive_skill(r):
    return dict(skill=r.u32(), level=r.u32())


def reserve_slot_target(r):
    return dict(reserve_slot_info=r.u32(), condition_info=r.u32())


def socket_material(r):
    return dict(item=r.u32(), value=r.u64())


def enchant_stat_change(r):
    return dict(stat=r.u32(), change_mb=r.i64())


def enchant_level_change(r):
    return dict(stat=r.u32(), change_mb=r.i8())


def enchant_stat_data(r):
    return dict(max_stat_list=r.array(enchant_stat_change, "esd.max"),
                regen_stat_list=r.array(enchant_stat_change, "esd.regen"),
                stat_list_static=r.array(enchant_stat_change, "esd.static"),
                stat_list_static_level=r.array(enchant_level_change, "esd.level"))


def price_floor(r):
    return dict(price=r.u64("pf.price"), sym_no=r.u32("pf.sym"), item_info_wrapper=r.u32("pf.wrap"))


def item_price(r):
    return dict(key=r.u32("ip.key"), price=price_floor(r))


def faction_price(r):
    # Faction price and cost rows: a key and three u64 values (no sym_no/wrapper pair).
    return dict(key=r.u32("fp.key"), values=(r.u64(), r.u64(), r.u64()))


def pair_u32(r):
    return dict(key=r.u32(), value=r.u32())


def enchant_data(r):
    return dict(level=r.u16("ench.level"), enchant_stat_data=enchant_stat_data(r),
                buy_price_list=r.array(item_price, "ench.buy"),
                equip_buffs=r.array(pair_u32, "ench.buffs"),
                unk_pair_list=r.array(pair_u32, "ench.pairs"),
                unk_u32=r.u32("ench.unk"))


def game_event_execute(r):
    return dict(game_event_type=r.u8(), player_condition=r.u32(), target_condition=r.u32(),
                event_condition=r.u32())


def inventory_change(r):
    return dict(game_event_execute_data=game_event_execute(r), to_inventory_info=r.u16())


def page_data(r):
    return dict(left=r.cstr(), right=r.cstr(), left_knowledge=r.u32(), right_knowledge=r.u32())


def inspect_data(r):
    return dict(item_info=r.u32(), gimmick_info=r.u32(), character_info=r.u32(),
                spawn_reason_hash=r.u32(), socket_name=r.cstr(), speak_character_info=r.u32(),
                inspect_target_tag=r.u32(), reward_own_knowledge=r.u8(), reward_knowledge_info=r.u32(),
                item_desc=r.locstr(), board_key=r.u32(), inspect_action_type=r.u8(),
                gimmick_state_name_hash=r.u32(), target_page_index=r.u32(), is_left_page=r.u8(),
                target_page_related_knowledge_info=r.u32(), enable_read_after_reward=r.u8(),
                refer_to_left_page_inspect_data=r.u8(), inspect_effect_info_key=r.u32(),
                inspect_complete_effect_info_key=r.u32(), unk_u32_a=r.u32(), unk_u32_b=r.u32())


def inspect_action(r):
    return dict(action_name_hash=r.u32(), catch_tag_name_hash=r.u32(), catcher_socket_name=r.cstr(),
                catch_target_socket_name=r.cstr())


def sharpness_data(r):
    return dict(max_sharpness=r.u16("sharp.max"), craft_tool_info=r.u16("sharp.tool"),
                stat_data=enchant_stat_data(r))


def item_bundle(r):
    return dict(count_mb=r.u64(), key=r.u32())


def unit_data(r):
    return dict(ui_component=r.cstr("unit.comp"), minimum=r.u32("unit.min"), icon_path=r.u32("unit.icon"),
                unk_hash_110=r.u32("unit.unk110"), item_name=r.locstr("unit.name"), item_desc=r.locstr("unit.desc"))


def money_unit_entry(r):
    return dict(key=r.u32(), value=unit_data(r))


def money_type_define(r):
    return dict(price_floor_value=r.u64(), unit_data_list_map=r.array(money_unit_entry, "money.units"))


def prefab_data(r):
    return dict(scale=(r.f32(), r.f32(), r.f32()),
                prefab_names=u32_list(r, "prefab.names"),
                animation_path_list=u32_list(r, "prefab.anims"),
                equip_slot_list=r.array(lambda r: r.u16(), "prefab.slots"),
                tribe_gender_list=u32_list(r, "prefab.tribes"),
                docking_prefab_switch_name=r.u32("prefab.switch"),
                flag_a=r.u8("prefab.flag_a"), flag_b=r.u8("prefab.flag_b"), flag_c=r.u8("prefab.flag_c"))


def repair_data(r):
    return dict(resource_item_info=r.u32(), repair_value=r.u16(), repair_style=r.u8(),
                resource_item_count=r.u64())


def sub_item(r):
    # 0x24A3F80: u8 type, then a u32 key for types 0, 3 and 9; type 0x12 carries nothing.
    t = r.u8("subitem.type")
    if t in (0, 3, 9):
        return dict(type_id=t, value=r.u32("subitem.value"))
    if t == 18:
        return dict(type_id=t, value=None)
    raise ValueError("unknown SubItem type %d at 0x%x" % (t, r.p - 1))


def drop_default(r):
    return dict(drop_enchant_level=r.u16("drop.ench"), socket_item_list=u32_list(r, "drop.sockets"),
                add_socket_material_item_list=r.array(socket_material, "drop.sockmat"),
                default_sub_item=sub_item(r), socket_valid_count=r.u8("drop.validcnt"), use_socket=r.u8("drop.use"))


def sealable(r):
    t = r.u8("seal.type")
    item_key = r.u32("seal.item")
    unk = r.u64("seal.unk")
    if t in (0, 1, 3, 4):
        v = r.u32("seal.value")
    elif t == 2:
        v = r.cstr("seal.str")
    else:
        raise ValueError("unknown SealableItemInfo type %d at 0x%x" % (t, r.p))
    return dict(type_tag=t, item_key=item_key, unknown0=unk, value=v)


def docking_child(r):
    return dict(gimmick_info_key=r.u32(), character_key=r.u32(), item_key=r.u32(),
                attach_parent_socket_name=r.cstr(), attach_child_socket_name=r.cstr(),
                docking_tag_name_hash=[r.u32() for _ in range(4)], docking_equip_slot_no=r.u16(),
                spawn_distance_level=r.u32(), is_item_equip_docking_gimmick=r.u8(),
                send_damage_to_parent=r.u8(), is_body_part=r.u8(), docking_type=r.u8(),
                is_summoner_team=r.u8(), is_player_only=r.u8(), is_npc_only=r.u32(),
                unk_condition=r.u32(),
                is_sync_break_parent=r.u8(), hit_part=r.u8(), detected_by_npc=r.u8(), is_bag_docking=r.u8(),
                enable_collision=r.u8(), disable_collision_with_other_gimmick=r.u8(), docking_slot_key=r.cstr(),
                inherit_summoner=r.u8(), summon_tag_name_hash=[r.u32() for _ in range(4)],
                animation_root_bone_name=r.cstr(), unk_trailing_u8=r.u8())


def pattern_param(r):
    return dict(flag=r.u8(), param_string=r.locstr())


def pattern_description(r):
    return dict(pattern_description_info=r.u32(), param_string_list=r.array(pattern_param, "pattern.params"))


def faction_management(r):
    return dict(is_valid=r.u8("fm.valid"), price_list=r.array(faction_price, "fm.price"),
                cost_list=r.array(faction_price, "fm.cost"), tier=r.u32("fm.tier"),
                unk_u16_list=r.array(lambda r: r.u16(), "fm.u16s"))


# ----------------------------------------------------------------------------- ItemInfo
# The loader reads fields in this order; the names follow its error strings.
_U8_FLAGS = ("is_reward_loot_drop", "lost_item_recovery_type", "is_editable_grime", "is_dyeable",
             "discardable", "use_average_price", "is_housing_only", "is_destroy_when_broken",
             "discard_attach_terrain", "is_blocked_store_sell", "enable_equip_in_clone_actor",
             "is_logout_at_use_item", "is_preserved_on_extract", "is_has_item_use_data_inventory_buff",
             "is_preorder_item", "is_tower_shield_item", "is_shield_item",
             "hide_from_inventory_on_pop_item", "is_save_game_data_at_use_item",
             "enable_alert_system_to_ui", "is_wild", "is_editor_usable", "use_map_icon_alert",
             "is_extract_able_item", "use_immediately", "apply_max_stack_cap",
             "is_all_gimmick_sealable", "is_register_trade_market", "can_disassemble",
             "delete_by_gimmick_unlock", "use_drop_set_target", "item_type", "knowledge_obtain_type",
             "item_tier", "usable_alert_type", "item_charge_type", "apply_drop_stat_type")


def parse_item(rec, trace=False, r=None):
    if r is None:
        r = Reader(rec, trace)
    it = {}
    it["string_key"] = r.cstr("string_key")
    it["key"] = r.u32("key")
    it["is_blocked"] = r.u8("is_blocked")
    it["max_endurance"] = r.u16("max_endurance")
    it["max_stack_count"] = r.u64("max_stack_count")
    for name in _U8_FLAGS:
        it[name] = r.u8(name)
    it["look_detail_mission_info"] = r.u32("look_detail_mission_info")
    it["look_detail_game_advice_info_wrapper"] = r.u32("look_detail_game_advice_info_wrapper")
    it["convert_item_info_by_drop_npc"] = r.u32("convert_item_info_by_drop_npc")
    it["unpacked_item_info"] = r.u32("unpacked_item_info")
    it["item_effect_info"] = r.u32("item_effect_info")
    it["broken_item_prefix_string"] = r.u32("broken_item_prefix_string")
    it["equip_type_info"] = r.u32("equip_type_info")
    it["material_match_info"] = r.u32("material_match_info")
    it["map_icon_path"] = r.u32("map_icon_path")
    it["packed_item_info"] = r.u32("packed_item_info")
    it["gimmick_info"] = r.u32("gimmick_info")
    it["destroy_effec_info"] = r.u32("destroy_effec_info")
    it["extract_additional_drop_set_info"] = r.u32("extract_additional_drop_set_info")
    it["minimum_extract_enchant_level"] = r.u16("minimum_extract_enchant_level")
    it["category_info"] = r.u16("category_info")
    it["gimmick_unlock_message_local_string_info"] = r.u32("gimmick_unlock_message_local_string_info")
    it["knowledge_info"] = r.u32("knowledge_info")
    it["max_drop_result_sub_item_count"] = r.u32("max_drop_result_sub_item_count")
    it["equipable_hash"] = r.u32("equipable_hash")
    it["default_sub_item"] = sub_item(r)
    it["discard_offset_y"] = r.f32("discard_offset_y")
    it["stage_info"] = r.u32("stage_info")
    it["material_key"] = r.u32("material_key")
    it["shared_cool_time_group_name_hash"] = r.u32("shared_cool_time_group_name_hash")
    it["equipable_level"] = r.u32("equipable_level")
    it["unk_u32_80"] = r.u32("unk_u32_80")
    it["unk_u32_84"] = r.u32("unk_u32_84")
    it["unk_u32_88"] = r.u32("unk_u32_88")
    it["item_name"] = r.locstr("item_name")
    it["item_desc"] = r.locstr("item_desc")
    it["item_desc2"] = r.locstr("item_desc2") if r.u8("item_desc2.flag") else None
    it["item_memo"] = r.cstr("item_memo")
    it["filter_type"] = r.cstr("filter_type")
    it["default_texture_path"] = r.u32("default_texture_path")
    it["emoji_texture_id"] = r.u32("emoji_texture_id")
    it["occupied_equip_slot_data_list"] = flag_array(r, occupied_equip_slot, "occupied_equip_slot_data_list")
    it["item_tag_list"] = flag_u32_list(r, "item_tag_list")
    it["item_use_info_list"] = flag_u32_list(r, "item_use_info_list")
    it["item_icon_list"] = r.array(item_icon, "item_icon_list")
    it["equip_passive_skill_list"] = flag_array(r, passive_skill, "equip_passive_skill_list")
    it["gimmick_tag_list"] = flag_array(r, lambda r: r.cstr(), "gimmick_tag_list")
    it["sealable_item_info_list"] = flag_array(r, sealable, "sealable_item_info_list")
    it["sealable_character_info_list"] = flag_array(r, sealable, "sealable_character_info_list")
    it["sealable_gimmick_info_list"] = flag_array(r, sealable, "sealable_gimmick_info_list")
    it["sealable_gimmick_tag_list"] = flag_array(r, sealable, "sealable_gimmick_tag_list")
    it["sealable_tribe_info_list"] = flag_array(r, sealable, "sealable_tribe_info_list")
    it["sealable_money_info_list"] = flag_u32_list(r, "sealable_money_info_list")
    it["transmutation_material_gimmick_list"] = flag_u32_list(r, "transmutation_material_gimmick_list")
    it["transmutation_material_item_list"] = flag_u32_list(r, "transmutation_material_item_list")
    it["transmutation_material_item_group_list"] = flag_array(
        r, lambda r: dict(item_group=r.u16(), count=r.u32()), "transmutation_material_item_group_list")
    it["multi_change_info_list"] = flag_u32_list(r, "multi_change_info_list")
    it["reserve_slot_target_data_list"] = flag_array(r, reserve_slot_target, "reserve_slot_target_data_list")
    it["drop_default_data"] = r.optional(drop_default, "drop_default_data")
    it["enchant_data_list"] = flag_array(r, enchant_data, "enchant_data_list")
    it["price_list"] = r.array(item_price, "price_list")
    it["docking_child_data"] = r.optional(docking_child, "docking_child_data")
    it["change_character_docking_child_data_list"] = flag_array(
        r, docking_child, "change_character_docking_child_data_list")
    it["inventory_change_data"] = r.optional(inventory_change, "inventory_change_data")
    it["fixed_page_data_list"] = flag_array(r, page_data, "fixed_page_data_list")
    it["dynamic_page_data_list"] = flag_array(r, page_data, "dynamic_page_data_list")
    it["inspect_data_list"] = flag_array(r, inspect_data, "inspect_data_list")
    it["inspect_action"] = r.optional(inspect_action, "inspect_action")
    it["cooltime_data"] = (r.i64("cooltime"), r.i64("cooltime_b"), r.i64("cooltime_c")) if r.u8("cooltime.flag") else None
    it["cooltime"] = it["cooltime_data"][0] if it["cooltime_data"] else 0
    it["sharpness_data"] = r.optional(sharpness_data, "sharpness_data")
    it["hackable_character_group_info_list"] = flag_array(
        r, lambda r: r.u16(), "hackable_character_group_info_list")
    it["item_group_info_list"] = r.array(lambda r: r.u16(), "item_group_info_list")
    it["pattern_description_data_list"] = flag_array(r, pattern_description, "pattern_description_data_list")
    it["item_bundle_data_list"] = flag_array(r, item_bundle, "item_bundle_data_list")
    it["money_type_define"] = r.optional(money_type_define, "money_type_define")
    it["faction_management_data"] = r.optional(faction_management, "faction_management_data")
    it["respawn_time_seconds"] = r.i64("respawn_time_seconds")
    it["repair_data_list"] = flag_array(r, repair_data, "repair_data_list")
    it["prefab_data_list"] = r.array(prefab_data, "prefab_data_list")
    it["push_inventory_types"] = [r.u16("push_inventory_type_%d" % i) for i in range(12)]
    # 1.0.0.3392 has no _isImportantItem (the string is gone from the exe). The u8 that
    # replaced it is _lostItemRecoveryType: across the 6,805 items present in both 2976
    # and 3392, old is_important_item == 1 exactly when lost_item_recovery_type == 0
    # (654 of 654; every other item reads 2, plus ShinyStone_02 at 1). The old key is
    # derived from it so downstream scripts keep their meaning.
    it["is_important_item"] = 1 if it["lost_item_recovery_type"] == 0 else 0
    it["_end"] = r.p
    it["_len"] = len(rec)
    return it, r


def parse_all_items(gamedata=GAMEDATA):
    out, bad = [], []
    for key, rec in load_table("iteminfo", gamedata):
        try:
            it, r = parse_item(rec)
            if r.p != len(rec):
                bad.append((key, rec, "end 0x%x != len 0x%x" % (r.p, len(rec))))
            out.append(it)
        except Exception as e:  # noqa
            bad.append((key, rec, repr(e)))
    return out, bad


def hexdump(b, start=0, end=None, base=0):
    end = len(b) if end is None else end
    lines = []
    for i in range(start, end, 16):
        chunk = b[i:min(i + 16, end)]
        lines.append("%04x: %-48s %s" % (base + i, chunk.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)))
    return "\n".join(lines)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "check":
        items, bad = parse_all_items()
        print("parsed", len(items), "bad", len(bad))
        show = int(sys.argv[2]) if len(sys.argv) > 2 else 3
        for key, rec, why in bad[:show]:
            print("=" * 100)
            print("key", key, "len", len(rec), why)
            tr = Reader(rec, True)
            try:
                parse_item(rec, trace=True, r=tr)
            except Exception as e:  # noqa
                print("  exception:", repr(e))
            trace = tr.trace
            for name, s, e, v in trace[-45:]:
                print("  %-45s 0x%04x-0x%04x %s" % (name, s, e, repr(v)[:80]))
            pos = trace[-1][2] if trace else 0
            print(hexdump(rec, max(0, pos - 48), min(len(rec), pos + 96)))
    elif cmd == "summary":
        import collections, re
        items, bad = parse_all_items()
        print("parsed", len(items), "bad", len(bad))
        c = collections.Counter()
        ex = {}
        for key, rec, why in bad:
            tr = Reader(rec, True)
            try:
                parse_item(rec, trace=True, r=tr)
            except Exception as e:  # noqa
                pass
            last = tr.trace[-1][0] if tr.trace else "?"
            sig = (re.sub(r"\d+", "N", why), last)
            c[sig] += 1
            ex.setdefault(sig, key)
        for sig, n in c.most_common():
            print(n, sig, "example key", ex[sig])
    elif cmd == "categories":
        for key, rec in load_table("categoryinfo"):
            r = Reader(rec)
            s = r.cstr(); k = r.u16(); blocked = r.u8(); a = r.u16(); b = r.u16(); c = r.u16()
            assert r.p == len(rec), (key, r.p, len(rec))
            print(k, blocked, repr(s), a, b, c)
