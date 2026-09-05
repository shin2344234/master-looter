"""Readers for Crimson Desert static-info tables (build 2.01.00, exe 1.0.0.2760).

Inputs are the files extracted from archive group 0008 (gamedata/*.staticinfobody +
*.staticinfoheader) and the English string tables from group 0020 (gamedata/*.paloc).

Layout for ItemInfo follows crimson-rs (potter420) for build 1.0.4.1, adjusted for the
fields that DMM 2.3.x knows about on 2.01.00. Every record is parsed against the byte
range the header gives it, so a layout error shows up as an end-offset mismatch.
"""
import os
import struct
import sys

EXTRACTED = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "extracted")
GAMEDATA = os.path.join(EXTRACTED, "0008", "gamedata")
LOCDIR = os.path.join(EXTRACTED, "0020", "gamedata")


# ----------------------------------------------------------------------------- paloc
def load_paloc(path):
    """Return {int_key: text}. Entry: u64 category, u32 keylen, key, u32 vallen, val.
    Trailing u32 = entry count. Item strings are keyed (item_key << 32) | sub, with
    sub 0x70 = name, 0x71 = description, 0x72 = description 2."""
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


def load_table(name, gamedata=GAMEDATA):
    """Return list of (key, record_bytes) in header order."""
    h = open(os.path.join(gamedata, name + ".staticinfoheader"), "rb").read()
    b = open(os.path.join(gamedata, name + ".staticinfobody"), "rb").read()
    cs = 4 if name in UINT_COUNT_TABLES else 2
    cnt = int.from_bytes(h[:cs], "little")
    rest = len(h) - cs
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
def occupied_equip_slot(r):
    return dict(equip_slot_name_key=r.u32("oes.key"), index_list=r.array(lambda r: r.u8(), "oes.idx"))


def item_icon(r):
    return dict(icon_path=r.u32("icon.path"), highlight_icon_path=r.u32("icon.hl"),
                check_usable=r.u8("icon.usable"),
                gimmick_state_list=r.array(lambda r: r.u32(), "icon.states"),
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


def equipment_buff(r):
    return dict(buff=r.u32(), level=r.u32())


def enchant_data(r):
    return dict(level=r.u16("ench.level"), enchant_stat_data=enchant_stat_data(r),
                buy_price_list=r.array(item_price, "ench.buy"),
                equip_buffs=r.array(equipment_buff, "ench.buffs"),
                unk_u32_112=r.u32("ench.unk112"))


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
                inspect_complete_effect_info_key=r.u32())


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
                prefab_names=r.array(lambda r: r.u32(), "prefab.names"),
                animation_path_list=r.array(lambda r: r.u32(), "prefab.anims"),
                equip_slot_list=r.array(lambda r: r.u16(), "prefab.slots"),
                tribe_gender_list=r.array(lambda r: r.u32(), "prefab.tribes"),
                docking_prefab_switch_name=r.u32("prefab.switch"),
                flag_a=r.u8("prefab.flag_a"), flag_b=r.u8("prefab.flag_b"), flag_c=r.u8("prefab.flag_c"))


def look_detail_entry(r):
    return dict(index=r.u8("ld.idx"), text=r.locstr("ld.text"))


def look_detail_advice(r):
    return dict(game_advice_info=r.u32("ld.advice"), entries=r.array(look_detail_entry, "ld.entries"))


def repair_data(r):
    return dict(resource_item_info=r.u32(), repair_value=r.u16(), repair_style=r.u8(),
                resource_item_count=r.u64())


def sub_item(r):
    t = r.u8("subitem.type")
    if t in (0, 3, 9):
        return dict(type_id=t, value=r.u32("subitem.value"))
    if t in (14, 18):
        return dict(type_id=t, value=None)
    raise ValueError("unknown SubItem type %d at 0x%x" % (t, r.p - 1))


def drop_default(r):
    return dict(drop_enchant_level=r.u16("drop.ench"), socket_item_list=r.array(lambda r: r.u32(), "drop.sockets"),
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
                is_sync_break_parent=r.u8(), hit_part=r.u8(), detected_by_npc=r.u8(), is_bag_docking=r.u8(),
                enable_collision=r.u8(), disable_collision_with_other_gimmick=r.u8(), docking_slot_key=r.cstr(),
                inherit_summoner=r.u8(), summon_tag_name_hash=[r.u32() for _ in range(4)],
                animation_root_bone_name=r.cstr())


def pattern_param(r):
    return dict(flag=r.u8(), unk_flag_2=r.u8(), unk_value=(r.u32(), r.u32()), param_string=r.cstr())


def pattern_description(r):
    return dict(pattern_description_info=r.u32(), param_string_list=r.array(pattern_param, "pattern.params"))


def faction_management(r):
    return dict(is_valid=r.u8("fm.valid"), price_list=r.array(item_price, "fm.price"),
                cost_list=r.array(item_price, "fm.cost"), tier=r.u32("fm.tier"))


# ----------------------------------------------------------------------------- ItemInfo
def parse_item(rec, trace=False, r=None):
    if r is None:
        r = Reader(rec, trace)
    it = {}
    it["key"] = r.u32("key")
    it["string_key"] = r.cstr("string_key")
    it["is_blocked"] = r.u8("is_blocked")
    it["max_stack_count"] = r.u64("max_stack_count")
    it["item_name"] = r.locstr("item_name")
    it["broken_item_prefix_string"] = r.u16("broken_item_prefix_string")
    it["inventory_info"] = r.u16("inventory_info")
    it["equip_type_info"] = r.u32("equip_type_info")
    it["occupied_equip_slot_data_list"] = r.array(occupied_equip_slot, "occupied_equip_slot_data_list")
    it["item_tag_list"] = r.array(lambda r: r.u32(), "item_tag_list")
    it["equipable_hash"] = r.u32("equipable_hash")
    it["consumable_type_list"] = r.array(lambda r: r.u32(), "consumable_type_list")
    it["item_use_info_list"] = r.array(lambda r: r.u32(), "item_use_info_list")
    it["item_icon_list"] = r.array(item_icon, "item_icon_list")
    it["map_icon_path"] = r.u32("map_icon_path")
    it["use_map_icon_alert"] = r.u8("use_map_icon_alert")
    it["item_type"] = r.u8("item_type")
    it["material_key"] = r.u32("material_key")
    it["material_match_info"] = r.u32("material_match_info")
    it["item_desc"] = r.locstr("item_desc")
    it["item_desc2"] = r.locstr("item_desc2")
    it["equipable_level"] = r.u32("equipable_level")
    it["category_info"] = r.u16("category_info")
    it["knowledge_info"] = r.u32("knowledge_info")
    it["knowledge_obtain_type"] = r.u8("knowledge_obtain_type")
    it["destroy_effec_info"] = r.u32("destroy_effec_info")
    it["equip_passive_skill_list"] = r.array(passive_skill, "equip_passive_skill_list")
    it["use_immediately"] = r.u8("use_immediately")
    it["apply_max_stack_cap"] = r.u8("apply_max_stack_cap")
    it["extract_additional_drop_set_info"] = r.u32("extract_additional_drop_set_info")
    it["minimum_extract_enchant_level"] = r.u16("minimum_extract_enchant_level")
    it["item_memo"] = r.cstr("item_memo")
    it["filter_type"] = r.cstr("filter_type")
    it["gimmick_info"] = r.u32("gimmick_info")
    it["gimmick_tag_list"] = r.array(lambda r: r.cstr(), "gimmick_tag_list")
    it["max_drop_result_sub_item_count"] = r.u32("max_drop_result_sub_item_count")
    it["use_drop_set_target"] = r.u8("use_drop_set_target")
    it["is_all_gimmick_sealable"] = r.u8("is_all_gimmick_sealable")
    it["sealable_item_info_list"] = r.array(sealable, "sealable_item_info_list")
    it["sealable_character_info_list"] = r.array(sealable, "sealable_character_info_list")
    it["sealable_gimmick_info_list"] = r.array(sealable, "sealable_gimmick_info_list")
    it["sealable_gimmick_tag_list"] = r.array(sealable, "sealable_gimmick_tag_list")
    it["sealable_tribe_info_list"] = r.array(sealable, "sealable_tribe_info_list")
    it["sealable_money_info_list"] = r.array(lambda r: r.u32(), "sealable_money_info_list")
    it["delete_by_gimmick_unlock"] = r.u8("delete_by_gimmick_unlock")
    it["gimmick_unlock_message_local_string_info"] = r.u32("gimmick_unlock_message_local_string_info")
    it["can_disassemble"] = r.u8("can_disassemble")
    it["transmutation_material_gimmick_list"] = r.array(lambda r: r.u32(), "transmutation_material_gimmick_list")
    it["transmutation_material_item_list"] = r.array(lambda r: r.u32(), "transmutation_material_item_list")
    it["transmutation_material_item_group_list"] = r.array(lambda r: r.u16(), "transmutation_material_item_group_list")
    it["is_register_trade_market"] = r.u8("is_register_trade_market")
    it["multi_change_info_list"] = r.array(lambda r: r.u32(), "multi_change_info_list")
    it["is_editor_usable"] = r.u8("is_editor_usable")
    it["discardable"] = r.u8("discardable")
    it["is_dyeable"] = r.u8("is_dyeable")
    it["is_editable_grime"] = r.u8("is_editable_grime")
    it["is_destroy_when_broken"] = r.u8("is_destroy_when_broken")
    it["is_housing_only"] = r.u8("is_housing_only")
    it["is_extract_able_item"] = r.u8("is_extract_able_item")
    it["quick_slot_index"] = r.u8("quick_slot_index")
    it["reserve_slot_target_data_list"] = r.array(reserve_slot_target, "reserve_slot_target_data_list")
    it["item_tier"] = r.u8("item_tier")
    it["is_important_item"] = r.u8("is_important_item")
    it["apply_drop_stat_type"] = r.u8("apply_drop_stat_type")
    it["is_reward_loot_drop"] = r.u8("is_reward_loot_drop")
    it["drop_default_data"] = drop_default(r)
    it["enchant_data_list"] = r.array(enchant_data, "enchant_data_list")
    it["price_list"] = r.array(item_price, "price_list")
    it["docking_child_data"] = r.optional(docking_child, "docking_child_data")
    it["inventory_change_data"] = r.optional(inventory_change, "inventory_change_data")
    it["unk_texture_path"] = r.cstr("unk_texture_path")
    it["fixed_page_data_list"] = r.array(page_data, "fixed_page_data_list")
    it["dynamic_page_data_list"] = r.array(page_data, "dynamic_page_data_list")
    it["inspect_data_list"] = r.array(inspect_data, "inspect_data_list")
    it["inspect_action"] = inspect_action(r)
    it["default_sub_item"] = sub_item(r)
    it["cooltime"] = r.i64("cooltime")
    it["unk_post_cooltime_a"] = r.i64("unk_post_cooltime_a")
    it["unk_post_cooltime_b"] = r.i64("unk_post_cooltime_b")
    it["item_charge_type"] = r.u8("item_charge_type")
    it["usable_alert_type"] = r.u8("usable_alert_type")
    it["sharpness_data"] = sharpness_data(r)
    it["max_charged_useable_count"] = r.u32("max_charged_useable_count")
    it["unk_post_max_charged_a"] = r.u32("unk_post_max_charged_a")
    it["unk_post_max_charged_b"] = r.u32("unk_post_max_charged_b")
    it["hackable_character_group_info_list"] = r.array(lambda r: r.u16(), "hackable_character_group_info_list")
    it["item_group_info_list"] = r.array(lambda r: r.u16(), "item_group_info_list")
    it["discard_offset_y"] = r.f32("discard_offset_y")
    it["discard_attach_terrain"] = r.u8("discard_attach_terrain")
    it["hide_from_inventory_on_pop_item"] = r.u8("hide_from_inventory_on_pop_item")
    it["is_shield_item"] = r.u8("is_shield_item")
    it["is_tower_shield_item"] = r.u8("is_tower_shield_item")
    it["is_wild"] = r.u8("is_wild")
    it["packed_item_info"] = r.u32("packed_item_info")
    it["unpacked_item_info"] = r.u32("unpacked_item_info")
    it["convert_item_info_by_drop_npc"] = r.u32("convert_item_info_by_drop_npc")
    it["pattern_description_data_list"] = r.array(pattern_description, "pattern_description_data_list")
    it["look_detail_game_advice_info_wrapper"] = r.array(look_detail_advice, "look_detail_game_advice_info_wrapper")
    it["look_detail_mission_info"] = r.u32("look_detail_mission_info")
    it["enable_alert_system_to_ui"] = r.u8("enable_alert_system_to_ui")
    it["is_save_game_data_at_use_item"] = r.u8("is_save_game_data_at_use_item")
    it["is_logout_at_use_item"] = r.u8("is_logout_at_use_item")
    it["shared_cool_time_group_name_hash"] = r.u32("shared_cool_time_group_name_hash")
    it["stage_info"] = r.u32("stage_info")
    it["item_bundle_data_list"] = r.array(item_bundle, "item_bundle_data_list")
    it["money_type_define"] = r.optional(money_type_define, "money_type_define")
    it["emoji_texture_id"] = r.cstr("emoji_texture_id")
    it["enable_equip_in_clone_actor"] = r.u8("enable_equip_in_clone_actor")
    it["is_blocked_store_sell"] = r.u8("is_blocked_store_sell")
    it["is_preorder_item"] = r.u8("is_preorder_item")
    it["is_has_item_use_data_inventory_buff"] = r.u8("is_has_item_use_data_inventory_buff")
    it["is_preserved_on_extract"] = r.u8("is_preserved_on_extract")
    it["item_effect_info"] = r.u32("item_effect_info")
    it["faction_management_data"] = faction_management(r)
    it["use_average_price"] = r.u8("use_average_price")
    it["respawn_time_seconds"] = r.i64("respawn_time_seconds")
    it["max_endurance"] = r.u16("max_endurance")
    it["repair_data_list"] = r.array(repair_data, "repair_data_list")
    it["prefab_data_list"] = r.array(prefab_data, "prefab_data_list")
    it["push_inventory_type_116"] = [r.u16("push_inventory_type_%d_116" % i) for i in range(8)]
    it["item_push_inventory_contents_type_113"] = r.u8("item_push_inventory_contents_type_113")
    it["trailing_u8_113"] = r.u8("trailing_u8_113")
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
            k = r.u16(); s = r.cstr(); blocked = r.u8(); a = r.u16(); b = r.u16(); c = r.u16()
            assert r.p == len(rec), (key, r.p, len(rec))
            print(k, blocked, repr(s), a, b, c)
