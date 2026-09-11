"""Build the Master Looter item database and tag every item.

Reads the extracted 2.01.00 tables via cdtables.py and writes to ../data:
  items_tagged.csv      one row per item, for review
  items_tagged.json     same rows as JSON (plus group ids and raw flags)
  tag_summary.md        tag vocabulary, rule list, counts
  unmapped_groups.txt   item groups no rule touches (for refining the rules)

Tags come from five sources, in this order:
  1. item-group membership (every item lists its whole group ancestry, so rules
     on a parent group reach all of its children),
  2. string_key patterns (Quest_, Item_gimmick_, Test/Dev names ...),
  3. the equip-type table (weapon class, armor slot),
  4. record flags (important, no-sell, housing-only, stackable, tier ...),
  5. data/class_overrides.csv: hand-made corrections per item (a class, tags
     to add, tags to remove) for the items the rules get wrong.
The class column is the first tag present in CLASS_ORDER unless an override
names one.
"""
import collections
import csv
import json
import os
import re
import sys

from cdtables import GAMEDATA, LOCDIR, Reader, load_paloc, load_table, parse_all_items

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")

# ------------------------------------------------------------------ group -> tags
# exact group string_key -> tags
GROUP_TAGS = {
    "ItemGroup_Category_Equipment": ["equipment"],
    "ItemGroup_Category_Food": ["consumable"],
    "ItemGroup_Category_Material": ["material"],
    "ItemGroup_Category_Document": ["document"],
    # equipment
    "ItemGroup_Equip_Weapon": ["weapon"],
    "ItemGroup_Equip_Weapon_OneHand": ["one-hand"],
    "ItemGroup_Equip_Weapon_TwoHand": ["two-hand"],
    "ItemGroup_Equip_Weapon_Range": ["ranged"],
    "ItemGroup_Equip_Weapon_Shield": ["shield"],
    "ItemGroup_Equip_SpecialWeapon": ["weapon", "special"],
    "ItemGroup_Equip_Armor": ["armor"],
    "ItemGroup_Equip_Armor_Mon": ["armor", "damaged-gear"],
    "ItemGroup_Equip_SpecialArmor": ["armor", "special"],
    "ItemGroup_Equip_Armor_Player_Helm": ["helm"],
    "ItemGroup_Equip_Armor_Player_Armor": ["body-armor"],
    "ItemGroup_Equip_Armor_Player_Gloves": ["gloves"],
    "ItemGroup_Equip_Armor_Player_Boots": ["boots"],
    "ItemGroup_Equip_Armor_Player_Cloak": ["cloak"],
    "ItemGroup_Equip_Armor_All_Plate": ["plate"],
    "ItemGroup_Equip_Armor_All_Leather": ["leather"],
    "ItemGroup_Equip_Armor_All_Fabric": ["cloth"],
    "ItemGroup_Equip_accessory": ["accessory"],
    "ItemGroup_equip_accessory_Necklace": ["necklace"],
    "ItemGroup_Equip_accessory_Earring": ["earring"],
    "ItemGroup_equip_accessory_Ring": ["ring"],
    "ItemGroup_Equip_BackPack": ["backpack"],
    "ItemGroup_Equip_Tool": ["tool"],
    "ItemGroup_Equip_Tool_NPC": ["tool", "npc-tool"],
    "ItemGroup_Equip_Riding": ["mount-gear"],
    "ItemGroup_Equip_Horse": ["mount-gear"],
    "ItemGroup_Equip_Pet_Armor": ["pet-gear"],
    "ItemGroup_Vehicle_Special": ["vehicle-part"],
    "ItemGroup_ETC_Quest_Equip": ["equipment", "quest"],
    "ItemGroup_ETC_Quest_Equip_Armor": ["equipment", "quest"],
    "ItemGroup_ETC_Quest_Equip_Accessory": ["equipment", "quest"],
    "ItemGroup_ETC_Quest_Equip_Special": ["equipment", "quest"],
    "ItemGroup_ETC_Quest_Equip_Horse": ["mount-gear", "quest"],
    # consumables
    "ItemGroup_KoreaFood": ["food"],
    "ItemGroup_Food_Drink": ["drink"],
    "ItemGroup_Food_WildFryingpan": ["field-cooked"],
    "ItemGroup_Food_WildPot": ["field-cooked"],
    "ItemGroup_potion": ["potion"],
    "ItemGroup_Alchemy_New": ["potion", "elixir"],
    "ItemGroup_Food_Horse": ["mount-feed"],
    "ItemGroup_Vehicle_Cooltime_Group": ["mount-utility"],
    # materials
    "ItemGroup_Material_Food": ["ingredient"],
    "ItemGroup_Material_Food_Fish": ["fish"],
    "ItemGroup_Material_Food_SeaFood": ["seafood"],
    "ItemGroup_Material_Food_Vegetable": ["vegetable"],
    "ItemGroup_Material_Food_Fruit": ["fruit"],
    "ItemGroup_Material_Food_Grain": ["grain"],
    "ItemGroup_Material_Food_Honey": ["honey"],
    "ItemGroup_Material_Food_Meat": ["meat"],
    "ItemGroup_Material_Food_Additive": ["cooking-basic"],
    "ItemGroup_Metarial_Medical": ["alchemy-material"],
    "ItemGroup_Material_Medical": ["herb"],
    "ItemGroup_Material_Medical_Insect": ["insect"],
    "ItemGroup_Material_Medical_Amphibians": ["amphibian"],
    "ItemGroup_Material_Alchemy_Sub": ["catalyst"],
    "ItemGroup_Metarial_Object": ["crafting-material"],
    "ItemGroup_Material_Object_Ore": ["ore"],
    "ItemGroup_Material_Object_Jewel": ["jewel"],
    # documents
    "ItemGroup_ETC_Document": ["document"],
    "ItemGroup_ETC_Quest_Document": ["quest"],
    "ItemGroup_ETC_LegendaryAnimal_Report": ["legendary-animal-report"],
    "ItemGroup_ETC_Document_WallPaper": ["poster"],
    "ItemGroup_ETC_Paper_SkillLearn": ["skill-poster"],
    "ItemGroup_ETC_Paper_Quest": ["quest"],
    "ItemGroup_ETC_Paper_Empty_Quest": ["quest"],
    "ItemGroup_ETC_Paper_Normal": ["note"],
    "ItemGroup_ETC_Document_Wanted": ["bounty-notice"],
    "ItemGroup_ETC_Book": ["book"],
    "ItemGroup_ETC_Book_Quest": ["quest"],
    "ItemGroup_ETC_Craft_Recipe": ["recipe"],
    "ItemGroup_ETC_Recipe_Food": ["recipe-food"],
    "ItemGroup_ETC_Recipe_Potion": ["recipe-potion"],
    "ItemGroup_ETC_Recipe_ReviveItem": ["recipe-potion"],
    "ItemGroup_ETC_Recipe_Special": ["recipe-potion"],
    "ItemGroup_ETC_Recipe_Artifact_Normal": ["recipe-abyss-gear"],
    "ItemGroup_ETC_Recipe_AbyssGear": ["recipe-abyss-gear"],
    "ItemGroup_ETC_Recipe_ResistanceArmor": ["recipe-armor"],
    "ItemGroup_ETC_TreasureMap": ["treasure-map"],
    "ItemGroup_ETC_TreasureMap_Quest": ["quest"],
    "ItemGroup_ETC_Book_Recipe": ["recipe-book"],
    "ItemGroup_EquipRecipe_Book": ["recipe-book"],
    # others
    "ItemGroup_AbyssItem_KuKuPot": ["kuku-pot-item"],
    "ItemGroup_AbyssItem_KuKuPot_Core": ["kuku-core"],
    "ItemGroup_AbyssItem_KuKuPot_PowerCore": ["kuku-power-core"],
    "ItemGroup_Control": ["abyss-item"],
    "ItemGroup_AbyssItem": ["abyss-item"],
    "ItemGroup_Housing": ["seed"],
    "ItemGroup_Housing_Seed": ["seed"],
    "ItemGroup_Animal_Item": ["animal"],
    "ItemGroup_Animal": ["animal"],
    "ItemGroup_ETC_Key": ["key"],
    "ItemGroup_ETC_Key_Quest": ["quest"],
    "ItemGroup_ETC_Key_Cube": ["cube-key"],
    "ItemGroup_Ammo": ["ammo"],
    "ItemGroup_Ammo_Explosion": ["explosive"],
    "ItemGroup_Bomb_Bullet": ["explosive"],
    "ItemGroup_Ammo_Arrow": ["arrow"],
    "ItemGroup_Ammo_Arrow_Monster": ["arrow", "npc-only"],
    "ItemGroup_Ammo_Bullet": ["bullet"],
    "ItemGroup_Ammo_CannonBall": ["cannonball"],
    "ItemGroup_Ammo_MagicBullet": ["magic-bullet"],
    "ItemGroup_Ammo_Package": ["ammo-bundle"],
    "ItemGroup_bag": ["bag"],
    "ItemGroup_Collection_Chest_Tier1": ["chest"],
    "ItemGroup_AbyssGear": ["abyss-gear"],
    "ItemGroup_Collection": ["furniture"],
    "ItemGroup_Collection_Deco": ["decoration"],
    "ItemGroup_Collection_Light": ["light"],
    "ItemGroup_Collection_Storage": ["storage"],
    "ItemGroup_trade_Unpack": ["trade-good"],
    "ItemGroup_trade_Packed": ["trade-good", "packaged"],
    "ItemGroup_Unique_Disaster_Trade": ["trade-good", "special"],
    "ItemGroup_ETC_Quest_Equip_Special_Boss": ["treasure"],
    "ItemGroup_Boss_Reward": ["treasure", "boss-reward"],
    "ItemGroup_Riding_Animal": ["mount-summon"],
    "ItemGroup_ETC_KuKuPot_All": ["kuku-pot"],
    "ItemGroup_ETC_KuKuPot": ["kuku-pot"],
    "ItemGroup_ETC_Quest_Memory": ["keepsake"],
    "ItemGroup_ETC_Visione_Memory_Quest": ["memory-fragment"],
    "ItemGroup_ETC_Quest_Normal": ["quest"],
    "ItemGroup_Collection_Quest_Reward": ["quest-reward"],
    "ItemGroup_ETC_Customize_Damian": ["character-exclusive"],
    "ItemGroup_ETC_Customize_Kliff": ["character-exclusive"],
    "ItemGroup_ETC_Enchant_Coin": ["token"],
    "ItemGroup_Enchant_Coin": ["refinement-token"],
    "ItemGroup_money": ["currency"],
    "ItemGroup_Camp_Money": ["camp-resource"],
    "ItemGroup_Money_Contribution": ["contribution"],
    "ItemGroup_Kuku_Money": ["kuku-currency"],
    "ItemGroup_Money_Pack": ["currency-pack"],
    "ItemGroup_Sealed_Artifact": ["sealed-artifact"],
    "ItemGroup_ETC_Artifact": ["artifact"],
    "ItemGroup_ETC_Visione_Equip": ["visione"],
    "ItemGroup_ETC_Visione_Immediately_Quest": ["visione", "quest"],
    "ItemGroup_Equip_SpecialArmor_Important": ["key-item"],
    "ItemGroup_Equip_SpecialArmor_Important_Band": ["key-item"],
    "ItemGroup_Goods": ["goods"],
    "ItemGroup_Collection_DyeWater": ["dye"],
    "ItemGroup_Material_Object_Bone": ["bone"],
    "ItemGroup_Material_Object_WoodBranch": ["wood"],
    "ItemGroup_Material_Food_BirdMeat": ["meat"],
    "ItemGroup_Meat_Disaster_Trade": ["trade-good"],
    "ItemGroup_ETC_Book_Normal_Blank": ["book"],
    "ItemGroup_ETC_Criminal_DropItem": ["document"],
    "ItemGroup_ETC_Lure": ["bait"],
    "ItemGroup_Tumble_Weed": ["gimmick"],
    "ItemGroup_Equip_AnimalSpirit": ["animal-spirit"],
    "ItemGroup_Collection_viewingstone_Tier1": ["decoration"],
    "ItemGroup_System_Consume_HP": ["restores-hp"],
    "ItemGroup_System_Consume_MP": ["restores-mp"],
    "ItemGroup_System_Consume_Stamina": ["restores-stamina"],
    "ItemGroup_System_Consume_Buff": ["buff"],
    "ItemGroup_Collection_LifeDeco": ["household"],
    "ItemGroup_Collection_Picture": ["painting"],
    "ItemGroup_Collection_Bottle": ["container"],
    "ItemGroup_Collection_Ceramic": ["container"],
    "ItemGroup_Collection_Bowl": ["container"],
    "ItemGroup_Collection_Cup": ["container"],
    "ItemGroup_Collection_Lamp": ["lamp"],
    "ItemGroup_Collection_Glasscraft": ["ornament"],
    "ItemGroup_Collection_FlowerPot": ["flower-pot"],
    "ItemGroup_Collection_FlowerPot_Low_Friendly": ["flower-pot"],
    "ItemGroup_Collection_DecoObject": ["household"],
    "ItemGroup_Collection_Tool": ["household"],
    "ItemGroup_Collection_Cook": ["cooking-facility"],
    "ItemGroup_Equip_Dev_Armor": ["dev"],
    "ItemGroup_Equip_Dev_Acc": ["dev"],
    "ItemGroup_Equip_Special": ["special"],
    "ItemGroup_Equip_Special_Pack": ["special"],
    "ItemGroup_Equip_Special_KuKu": ["kuku-gear"],
    "ItemGroup_Equip_contributionitem": ["contribution-reward"],
    "ItemGroup_Equip_Horse_Armor": ["mount-gear"],
    "ItemGroup_Equip_Horse_Parts": ["mount-gear"],
    "ItemGroup_Material_Medical_Curative": ["herb"],
    "ItemGroup_Platform_Special": ["platform-bonus"],
    "ItemGroup_trade": ["trade-good"],
    "ItemGroup_Unique_Trade": ["trade-good", "special"],
    "ItemGroup_Food_Horse_Add": ["mount-feed"],
    "ItemGroup_Equip_Weapon_TwoHandFlag": ["banner"],
    "ItemGroup_Material_Food_Fish_Tier5": ["fish", "legendary-fish"],
    "ItemGroup_Material_UnTakeable": ["herb", "mushroom", "poisonous"],
    "ItemGroup_Material_Food_Mushroom_Tier1": ["mushroom"],
    "ItemGroup_Material_Food_SeaFood_ShellFish_Tier2": ["shellfish"],
    "ItemGroup_Material_Food_SeaFood_Clam": ["clam"],
    "ItemGroup_Material_Food_egg_Tier1": ["egg"],
    "ItemGroup_Material_Food_Dairy_Tier1": ["dairy"],
    "ItemGroup_Housing_Fertilizer": ["fertilizer"],
    "ItemGroup_Food_Horse_Only": ["mount-feed"],
    "ItemGroup_ExpansionBag": ["bag"],
    "ItemGroup_ExpansionFarmSlot": ["bag"],
}

# prefix rules (group string_key startswith) -> tags; applied after exact matches
GROUP_PREFIX_TAGS = [
    ("ItemGroup_Material_Alchemy_", ["reagent"]),
    ("ItemGroup_ETC_Recipe_Furniture", ["recipe-furniture"]),
    ("ItemGroup_Material_Object_Wood", ["wood"]),
    ("ItemGroup_Material_Object_Leather", ["hide"]),
    ("ItemGroup_Material_Object_Metal", ["metal"]),
    ("ItemGroup_Material_Object_Stone", ["stone"]),
    ("ItemGroup_Material_Object_Fabric", ["fabric"]),
    ("ItemGroup_Material_Object_Ore", ["ore"]),
    ("ItemGroup_Material_Object_Jewel", ["jewel"]),
    ("ItemGroup_Material_Object_Bone", ["bone"]),
    ("ItemGroup_Collection_Dye", ["dye"]),
    ("ItemGroup_Equip_StealthArmor", ["stealth-gear"]),
    ("ItemGroup_Item_Contributionitem", ["contribution-reward"]),
    ("ItemGroup_Equip_SpecialArmor_KuKu", ["kuku-gear"]),
    ("ItemGroup_Equip_SpecialWeapon_TwoHandSpear_KuKu", ["kuku-gear"]),
    ("ItemGroup_Equip_KuKubird", ["kuku-gear"]),
    ("ItemGroup_Equip_Weapon_TwoHandSpear_Flag", ["banner"]),
    ("ItemGroup_Equip_Vehicle_Special_ATAG", ["atag"]),
    ("ItemGroup_Legendary", ["legendary"]),
    ("ItemGroup_Food_Store", ["store-food"]),
    ("ItemGroup_Rare_Collect", ["rare-gather"]),
    ("ItemGroup_Furniture", ["furniture"]),
    ("ItemGroup_Equip_Human_", ["npc-weapon-pool"]),
    ("ItemGroup_Equip_Dwarf_", ["npc-weapon-pool"]),
    ("ItemGroup_Equip_Goblin_", ["npc-weapon-pool"]),
    ("ItemGroup_Equip_Orc_", ["npc-weapon-pool"]),
    ("ItemGroup_Equip_Troll_", ["npc-weapon-pool"]),
    ("ItemGroup_AbyssGear", ["abyss-gear"]),
    ("ItemGroup_RestArea", ["food", "rest-area"]),
    ("ItemGroup_AbyssArtifact_buff", ["stat-boost"]),
    ("ItemGroup_Random_AbyssBox", ["abyss-gear-box"]),
    ("ItemGroup_Equip_Weapon", ["weapon"]),
    ("ItemGroup_Equip_SpecialWeapon", ["weapon"]),
    ("ItemGroup_Collection_Dye", ["dye"]),
]

# equip type string_key -> tags
EQUIP_TAGS = {
    "OneHandSword": ["sword"], "TwoHandSword": ["longsword"], "TwoHandGiantSword": ["greatsword"],
    "OneHandAxe": ["axe"], "TwoHandAxe": ["greataxe"], "TwoHandGiantAxe": ["greataxe"],
    "OneHandMace": ["mace"], "TwoHandMace": ["club"], "TwoHandGiantMace": ["club"],
    "TwoHandWarHammer": ["warhammer"], "TwoHandHammer": ["greathammer"], "TwoHandGiantHammer": ["greathammer"],
    "OneHandHammer": ["hammer"], "OneHandRapier": ["rapier"], "OneHandDagger": ["dagger"],
    "TwoHandSpear": ["spear"], "TwoHandGiantSpear": ["spear"], "TwoHandPike": ["pike"],
    "TwoHandHalberd": ["halberd"], "TwoHandScythe": ["scythe"], "TwoHandFlag": ["banner"],
    "OneHandFlail": ["flail"], "TwoHandFlail": ["flail"], "OneHandFist": ["fist"], "Gauntlet": ["fist"],
    "OneHandFan": ["fan"], "OneHandBow": ["bow"], "OneHandCrossBow": ["crossbow"],
    "OneHandPistol": ["pistol"], "OneHandMusket": ["musket"], "OneHandShotgun": ["shotgun"],
    "OneHandSpeargun": ["harpoon-gun"], "OneHandCannon": ["blaster"], "TwoHandCannon": ["cannon"],
    "TwoHandFlamethrower": ["thrower"], "TwoHandIcethrower": ["thrower"], "TwoHandLightningthrower": ["thrower"],
    "TwoHandBlowPipe": ["blowpipe"], "OneHandBomb": ["bomb-weapon"], "OneHandTorch": ["torch"],
    "OneHandShield": ["shield"], "OneHandShieldRight": ["shield"], "OneHandTowerShield": ["large-shield"],
    "OneHandDrill": ["drill"], "OneHandSaw": ["chainsaw"], "Lantern": ["lantern"],
    "Helm": ["helm"], "Upperbody": ["body-armor"], "Hand": ["gloves"], "Foot": ["boots"], "Cloak": ["cloak"],
    "Earring": ["earring"], "Necklace": ["necklace"], "Ring": ["ring"], "Bracelet": ["bracelet"],
    "Glass": ["eyewear"], "Mask": ["mask"], "BackPack": ["backpack"], "SprayBag": ["sprayer"],
    "PetHelm": ["pet-gear"], "PetArmor": ["pet-gear"], "PetAccessory": ["pet-gear"],
    "HorseArmor": ["mount-gear"], "HorseHelm": ["mount-gear"], "HorseSaddle": ["mount-gear"],
    "HorseStirrup": ["mount-gear"], "HorseShoe": ["mount-gear"], "SpecialVehicleArmor": ["mount-gear"],
    "DragonArmor": ["mount-gear"], "Battery": ["battery"], "Ammo": ["ammo"], "HiddenEquip": ["hidden-equip"],
}

# item_type byte -> human label (from the dominant item groups of each value on 2.01.00)
ITEM_TYPE_LABELS = {
    0: "arrow/ammo", 1: "amphibian", 2: "small animal", 3: "body armor", 4: "abyss artifact", 5: "one-hand axe",
    6: "backpack", 7: "boss keepsake", 8: "recipe book", 9: "boots", 10: "bow", 11: "crossbow", 12: "dagger",
    13: "stat boost", 14: "dragon armor", 15: "earring", 16: "fish", 17: "fishing rod", 18: "fan/gauntlet/blaster",
    19: "food/plant", 21: "eyewear/circlet", 22: "gloves", 23: "bomb", 24: "helm", 25: "mount gear",
    26: "insect", 27: "key (door)", 28: "gadget", 29: "mace", 30: "mask", 31: "misc object", 32: "currency",
    33: "musket", 34: "necklace", 35: "ammo bundle", 36: "diary", 37: "permit", 38: "pet armor", 39: "pet helm",
    40: "pistol", 41: "potion", 42: "notice paper", 43: "book", 44: "key", 45: "quest paper", 46: "letter",
    47: "rapier", 48: "recipe", 49: "ring", 50: "atag part", 52: "shield", 53: "shotgun", 54: "bag/contract",
    55: "sprayer pack", 56: "one-hand sword", 57: "tool", 58: "prop tool", 59: "torch", 60: "large shield",
    61: "packaged trade good", 62: "trade good", 63: "treasure map", 64: "halberd", 65: "greataxe",
    68: "greatsword", 69: "greathammer", 70: "spear/banner", 71: "thrower", 72: "longsword", 73: "warhammer",
    74: "abyss gear", 102: "gimmick part", 103: "machine part", 104: "pet sigil",
}

# priority order used to pick the single "class" column (first tag present wins)
CLASS_ORDER = [
    "dev", "currency", "camp-resource", "contribution", "kuku-currency", "token", "refinement-token",
    "keepsake", "memory-fragment", "treasure", "key-item",
    "key", "cube-key",
    # recipe kinds before the generic bucket, so each kind is its own class
    "skill-book", "recipe-book", "recipe-food", "recipe-potion", "recipe-furniture", "recipe-abyss-gear", "recipe-armor",
    "recipe", "book", "treasure-map", "bounty-notice", "skill-poster", "poster",
    "legendary-animal-report", "contract", "note", "document",
    "ammo-bundle", "explosive", "arrow", "bullet", "cannonball", "magic-bullet", "ammo",
    "damaged-gear", "shield", "weapon", "helm", "body-armor", "gloves", "boots", "cloak", "armor",
    "necklace", "earring", "ring", "bracelet", "eyewear", "mask", "accessory",
    "backpack", "sprayer", "bag", "tool", "npc-tool", "mount-gear", "mount-utility", "mount-summon",
    "pet-gear", "vehicle-part", "atag", "visione",
    "drink", "field-cooked", "store-food", "food", "elixir", "potion", "stat-boost", "consumable",
    # food before mount feed: an apple is a fruit that horses also eat
    "fish", "seafood", "meat", "vegetable", "fruit", "grain", "honey", "cooking-basic", "ingredient", "mount-feed",
    "insect", "amphibian", "herb",
    # the specific material classes before the alchemy buckets, so Iron Ore is ore and Stone is stone
    "ore", "jewel", "wood", "hide", "metal", "stone", "fabric", "bone",
    "catalyst", "reagent", "alchemy-material", "crafting-material", "material",
    "dye", "painting", "container", "lamp", "ornament", "flower-pot", "household", "cooking-facility",
    "decoration", "light", "storage", "chest", "furniture", "bait", "animal-spirit",
    "sealed-artifact", "artifact", "abyss-gear-box", "abyss-gear", "kuku-power-core", "kuku-core", "kuku-pot-item", "kuku-pot",
    "kuku-gear", "abyss-item", "trade-good", "animal", "seed", "gimmick",
    "quest", "quest-reward", "platform-bonus", "special", "goods", "equipment",
]

# species detail for caught creatures, from the English name of insect-class items
INSECT_NAME_TAGS = [
    (re.compile(r"Butterfly|Swallowtail$"), "butterfly"), (re.compile(r"Moth\b"), "moth"),
    (re.compile(r"Beetle"), "beetle"), (re.compile(r"Dragonfly"), "dragonfly"),
    (re.compile(r"bee\b|Hornet", re.I), "bee"), (re.compile(r"\bFly$"), "fly"),
    (re.compile(r"Centipede"), "centipede"), (re.compile(r"Spider|Scorpion"), "arachnid"),
    (re.compile(r"Snail"), "mollusc"), (re.compile(r"Wharf Roach"), "crustacean"),
]

DEV_RE = re.compile(r"(^|_)(Test|Dev|QA|Specialty|Dummy|Debug|Sample)(_|$)|_QA$|_T\d_QA|^TestNeck|^Testarmor|^Socket_Test", re.I)


def load_groups():
    gloc = load_paloc(os.path.join(LOCDIR, "itemgroup.paloc"))
    groups = {}
    for k, rec in load_table("itemgroupinfo"):
        r = Reader(rec)
        key = r.u16(); s = r.cstr(); r.u8(); r.locstr()
        subs = r.array(lambda r: r.u16()); items = r.array(lambda r: r.u32())
        groups[key] = dict(key=key, name=s, eng=gloc.get((key << 32) | 0x80), subs=set(subs), items=items)
    return groups


def load_equip_types():
    et = {}
    for k, rec in load_table("equiptypeinfo"):
        r = Reader(rec); kk = r.u32(); et[kk] = r.cstr()
    return et


def load_categories():
    cats = {}
    for k, rec in load_table("categoryinfo"):
        r = Reader(rec); kk = r.u16(); r.cstr(); r.u8(); cats[kk] = (r.u16(), r.u16(), r.u16())
    return cats


def tag_item(it, groups, equip_types, name_en):
    tags = set()
    gnames = [groups[g]["name"] for g in it["item_group_info_list"] if g in groups]
    for gn in gnames:
        tags.update(GROUP_TAGS.get(gn, ()))
        for prefix, ts in GROUP_PREFIX_TAGS:
            if gn.startswith(prefix):
                tags.update(ts)
    if any("MiniGame" in gn or "Minigame" in gn for gn in gnames):
        tags.add("minigame")
    sk = it["string_key"]
    if sk.startswith("Quest_") or "_Quest_" in sk:
        tags.add("quest")
    if sk.startswith(("Item_gimmick_", "Item_puzzle_", "Item_cd_")) or "_gimmick_" in sk:
        tags.add("gimmick")
    if sk.startswith("Recipe_"):
        tags.add("recipe")
    # The "... of the World, Vol. N" series teaches a crafting skill; the rest
    # of its group are single blueprints. A player who wants the skill books
    # rarely wants the blueprints, so the series is a class of its own.
    if "recipe-book" in tags and name_en and " of the World" in name_en:
        tags.add("skill-book")
    if sk.startswith("Money_") or it["money_type_define"]:
        tags.add("currency")
    if sk.startswith("Trade_"):
        tags.add("trade-good")
    if sk.startswith("Collection_Prop_"):
        tags.add("collectible")
    if sk.startswith("Legendary_Animal_"):
        tags.add("legendary-animal")
    if name_en is None or DEV_RE.search(sk) or not it["is_editor_usable"]:
        tags.add("dev")
    et = equip_types.get(it["equip_type_info"])
    if et:
        tags.update(EQUIP_TAGS.get(et, ()))
        if et.startswith("Tool"):
            tags.add("tool")
        if et.startswith("Robot"):
            tags.add("atag")
    if "insect" in tags and name_en:
        for pat, t in INSECT_NAME_TAGS:
            if pat.search(name_en):
                tags.add(t)
    # attribute tags
    tags.add("tier-%d" % it["item_tier"])
    if it["max_stack_count"] > 1:
        tags.add("stackable")
    if it["is_important_item"]:
        tags.add("important")
    if it["is_blocked_store_sell"]:
        tags.add("no-sell")
    if not it["discardable"]:
        tags.add("no-discard")
    if it["is_housing_only"]:
        tags.add("housing-only")
    if it["is_wild"]:
        tags.add("wild")
    if it["is_preorder_item"]:
        tags.add("preorder")
    if it["is_extract_able_item"]:
        tags.add("extractable")
    if it["use_immediately"]:
        tags.add("use-immediately")
    if it["hide_from_inventory_on_pop_item"]:
        tags.add("hidden")
    if it["knowledge_info"]:
        tags.add("knowledge")
    if it["docking_child_data"]:
        tags.add("docking")
    if it["is_blocked"]:
        tags.add("blocked")
    return tags


def load_overrides():
    """data/class_overrides.csv: per-item corrections applied after every rule.
    Columns: string_key, class (empty keeps the computed one), add_tags,
    remove_tags (both space separated), note."""
    path = os.path.join(DATA, "class_overrides.csv")
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            sk = (r.get("string_key") or "").strip()
            if not sk:
                continue
            out[sk] = dict(klass=(r.get("class") or "").strip(), add=set((r.get("add_tags") or "").split()),
                           remove=set((r.get("remove_tags") or "").split()))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    os.makedirs(DATA, exist_ok=True)
    items, bad = parse_all_items()
    assert not bad and all(it["_end"] == it["_len"] for it in items), "parser is not clean; run cdtables.py summary"
    loc = load_paloc(os.path.join(LOCDIR, "item.paloc"))
    groups = load_groups()
    equip_types = load_equip_types()
    cats = load_categories()
    parent = {c: k for k, g in groups.items() for c in g["subs"]}
    overrides = load_overrides()
    used = set()

    rows = []
    tag_counter = collections.Counter()
    class_counter = collections.Counter()
    for it in items:
        key = it["key"]
        name_en = loc.get((key << 32) | 0x70)
        desc_en = loc.get((key << 32) | 0x71) or ""
        tags = tag_item(it, groups, equip_types, name_en)
        ov = overrides.get(it["string_key"])
        if ov:
            used.add(it["string_key"])
            tags |= ov["add"]
            tags -= ov["remove"]
            if ov["klass"]:
                tags.add(ov["klass"])   # the class is always one of the tags
        klass = (ov["klass"] if ov and ov["klass"] else None) or next((t for t in CLASS_ORDER if t in tags), "other")
        gs = [g for g in it["item_group_info_list"] if g in groups]
        leaf = [g for g in gs if not (groups[g]["subs"] & set(gs))]
        roots = sorted({groups[g]["name"] for g in gs if g not in parent})
        prices = {p["key"]: p["price"]["price"] for p in it["price_list"]}
        cat = cats.get(it["category_info"], (None, None, None))
        row = dict(
            key=key,
            string_key=it["string_key"],
            name=name_en or "",
            klass=klass,
            tags=" ".join(sorted(tags)),
            item_type=it["item_type"],
            item_type_label=ITEM_TYPE_LABELS.get(it["item_type"], "type-%d" % it["item_type"]),
            equip_type=equip_types.get(it["equip_type_info"], "") if it["equip_type_info"] else "",
            tier=it["item_tier"],
            max_stack=it["max_stack_count"] if it["max_stack_count"] < 10**12 else -1,
            value_copper=prices.get(1, ""),
            other_prices=";".join("%d:%d" % (k, v) for k, v in sorted(prices.items()) if k != 1),
            category=it["category_info"],
            category_main=cat[0], category_middle=cat[1], category_sub=cat[2],
            leaf_groups=" | ".join(sorted({(groups[g]["eng"] or groups[g]["name"]) for g in leaf})),
            leaf_group_keys=" | ".join(sorted({groups[g]["name"] for g in leaf})),
            root_groups=" | ".join(r.replace("ItemGroup_", "") for r in roots),
            important=it["is_important_item"], no_sell=it["is_blocked_store_sell"], discardable=it["discardable"],
            housing_only=it["is_housing_only"], wild=it["is_wild"], knowledge=1 if it["knowledge_info"] else 0,
            desc=desc_en.replace("\n", " ")[:300],
        )
        rows.append(row)
        tag_counter.update(tags)
        class_counter[klass] += 1

    for sk, ov in overrides.items():
        if sk not in used:
            print("class_overrides.csv names an unknown item:", sk)
        if ov["klass"] and ov["klass"] not in CLASS_ORDER:
            print("class_overrides.csv uses a class outside CLASS_ORDER:", sk, ov["klass"])
    print("overrides applied:", len(used), "of", len(overrides))

    # CSV
    fields = list(rows[0].keys())
    with open(os.path.join(DATA, "items_tagged.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    # JSON (adds group ids)
    for r, it in zip(rows, items):
        r["group_keys"] = it["item_group_info_list"]
    with open(os.path.join(DATA, "items_tagged.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, ensure_ascii=False, indent=0)

    # unmapped groups: groups no rule names, with item counts
    mapped = set(GROUP_TAGS)
    unm = []
    for g in groups.values():
        if g["name"] in mapped or any(g["name"].startswith(p) for p, _ in GROUP_PREFIX_TAGS):
            continue
        unm.append((len(g["items"]), g["key"], g["name"], g["eng"]))
    unm.sort(reverse=True)
    with open(os.path.join(DATA, "unmapped_groups.txt"), "w", encoding="utf-8") as f:
        for n, k, name, eng in unm:
            f.write("%5d [%d] %s | %s\n" % (n, k, name, eng))

    # summary
    lines = ["# Master Looter item tags, build 2.01.00", "",
             "%d items parsed from iteminfo.staticinfobody; %d have an English name." % (
                 len(rows), sum(1 for r in rows if r["name"])), "",
             "## Class column (one per item, first match in priority order)", ""]
    for k, n in class_counter.most_common():
        lines.append("- %s: %d" % (k, n))
    lines += ["", "## All tags", ""]
    for k, n in sorted(tag_counter.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append("- %s: %d" % (k, n))
    lines += ["", "## Rules", "",
              "Group rules (item gets the tags of every group in its ancestry):", ""]
    for gn, ts in GROUP_TAGS.items():
        lines.append("- %s -> %s" % (gn, ", ".join(ts)))
    lines += ["", "Group prefix rules:", ""]
    for p, ts in GROUP_PREFIX_TAGS:
        lines.append("- %s* -> %s" % (p, ", ".join(ts)))
    lines += ["", "Name rules: Quest_/_Quest_ -> quest; Item_gimmick_/Item_puzzle_/Item_cd_ -> gimmick; Recipe_ -> recipe; "
              "Money_ or a money definition -> currency; Trade_ -> trade-good; Collection_Prop_ -> collectible; "
              "Legendary_Animal_ -> legendary-animal; Test/Dev/QA names, no English name, or is_editor_usable=0 -> dev.", "",
              "Equip-type rules: the equiptypeinfo string_key maps to a weapon or slot tag (sword, bow, helm, ...).", "",
              "Flag tags: tier-N, stackable (max stack > 1), important, no-sell, no-discard, housing-only, wild, preorder, "
              "extractable, use-immediately, hidden, knowledge (grants a knowledge entry), docking, blocked."]
    with open(os.path.join(DATA, "tag_summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("rows", len(rows), "classes", len(class_counter), "tags", len(tag_counter), "unmapped groups", len(unm))
    print("class counts:", class_counter.most_common())
    print("other examples:", [(r["string_key"], r["leaf_groups"]) for r in rows if r["klass"] == "other"][:25])
    print("top unmapped:", unm[:25])


if __name__ == "__main__":
    main()
