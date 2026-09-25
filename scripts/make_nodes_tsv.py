"""Gather-node table for the plugin (Crimson Desert 2.02.00).

A gather node reports a 16-bit id that is not stable between sessions, so it
cannot be used to remember what a node yields. What the node does carry is the
path of its own prefab, readable from the gimmick component at +0x18 -> +0x38,
and every one of those paths is a row of GimmickInfo. That row names the node
and tags what kind of gathering it is, which is exactly what the Plants, Ore,
Stone and Wood switches need.

Writes mod/data/MasterLooter.nodes.tsv: prefab basename, kind, item string key
when the name gives one, the row's own name, whether the kind came from the
game's own tag, and whether the node breaks. The plugin compiles it in.

Breakability is read off the row the same way everything else here is. A vein
carries SelfForceBreakImpulse or BreakProjectileKey; the ore chunks a vein
drops carry neither, because a player picks those up. The mod drives the swing
and the break at a vein, and doing that to a chunk does nothing at all, which
is what made bismuth look intermittent: the vein broke into chunks, the mod
drove a break at each chunk, and the ore stayed on the ground. Eleven of the
nodes this table calls ore turn out not to break.

Run after build_item_db.py, which produces the items_tagged.csv this reads.
"""

import collections
import csv
import os
import re
import struct
import sys

import cdtables

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
OUT = os.path.join(HERE, "..", "mod", "data", "MasterLooter.nodes.tsv")

# Tags the game puts on a gimmick row, mapped to the switch that owns them.
# A crop or a fruit is picked up as a ground item whether it is on the plant or
# on the floor, which is why they land on `item` and not on `plant`.
TAG_KIND = {
    "collect_wood": "wood", "collect_tree": "wood", "collect_firewood": "wood",
    "collect_mine": "ore", "collect_ore": "ore",
    "collect_botany": "plant",
    "catch_treefruit": "item", "catch_berries": "item", "catch_groundfruit": "item",
    "catch_crops": "item", "catch_vegetable": "item",
}
# A fruit tree or bush you harvest rather than fell. See kind_for.
FRUIT_PLANT = re.compile(r"^gimmick_(unique_)?(tree|bush)_.*(crop_|broad_pear).*_collect")
# Tags above that override collect_botany when a row carries both.
FRUIT_TAGS = ("catch_treefruit", "catch_berries", "catch_groundfruit", "catch_crops", "catch_vegetable")

# What the player can pick up, which the game marks with a catch_ tag naming the
# shape of the thing. Until 13 September 2026 only the five fruit tags above were
# mapped and every other catch_ tag dropped the row out of the table, so 1,228
# prefabs the game itself calls pickups arrived with no kind at all. NodeKind
# then returned Unknown, GatherSwitchOff fell to its default, and the arm loop
# would only touch them with "Unidentified nodes" on. An object of this sort
# arrives empty and has to be armed before anything can read it, so with that
# switch off it was refused as "not ready (node empty)" for ever. LuxDragon
# reported the visible half: loose coins and gold bars never picked up, one of
# the bars logged at half a metre.
#
# Grouped by the switch each belongs under, by counting the tags rather than
# guessing from them:
#   container  519 prefabs, the things you open
#   item       409 prefabs, what you pick up in one hand
#   left out   299 prefabs, pillars, flags, chairs, handles and worn equipment,
#              none of which is loot, and catch_equip in particular is the worn
#              gear family the verdict refuses outright.
# No prefab carries an item tag and a container tag at once, checked, so the
# order they are read in cannot change an answer. Tags are stripped before
# matching: one row spells it "catch_dish " with a trailing space.
CATCH_KIND = {
    "catch_onehand": "pickup", "catch_dish": "pickup", "catch_cup": "pickup",
    "catch_paper": "pickup", "catch_stick": "pickup", "catch_simple": "pickup",
    "catch_quickpickup": "pickup", "catch_onebunch": "pickup",
    "catch_mushroom": "pickup", "catch_pot": "pickup",
    "catch_boxsmall": "container", "catch_boxmiddle": "container",
    "catch_box": "container", "catch_barrel": "container",
    "catch_sack": "container", "catch_itemcontainer": "container",
}

# The engine refuses these by name wherever a prefab has no row of its own, in
# the verdict and again in the arm loop, and both tests are written against the
# absence of a row. So a row has to carry the same answer the name would have
# given, or adding one quietly removes the Containers switch from the thing.
# Same three words as engine.cpp's two container tests.
CONTAINER_WORDS = ("_chest", "_box", "dropset")

# Held back from CATCH_KIND until somebody has watched what happens. Matched
# against the prefab name, and only against the catch path: a camp farm's growth
# phases carry collect tags and keep the kinds they already had.
#
# "camp_farm" is the 20 gimmick_camp_farm_*_seed prefabs. The game tags them
# catch_onehand, which says a player picks one up in one hand and suggests a
# loose seed, but nothing here says whether gathering one lifts something the
# player has planted. Ground items is on by default, so getting this wrong would
# empty somebody's farm without them asking for it. Test before mapping it.
#
# Answered on 19 September 2026: it does. Kuradeon's planted seeds went straight
# back into the bag. The engine refuses every gimmick_camp_farm_*_seed by name
# (PlantedSeed in engine.cpp), in the verdict and in arming, so these stay out
# of the table and no switch can turn them back on.
CATCH_SKIP = ("camp_farm",)

# Tags held back for the same reason, named rather than mapped.
#
# "catch_bouquet" sits on twelve prefabs, and four of them are the wood and
# bamboo branch sockets. Their own yields column names Wood_Branch_Usable and
# its cousins, every one class wood, so filing them as item put them under
# Ground items and turning Wood off stopped reaching them. The other eight look
# like real bouquets. Separating the two wants the yields counted, which is the
# rule this file already follows for a kind's contents, so it waits for that
# rather than for a name guess.
CATCH_HOLD = ("catch_bouquet",)

# Prefabs the game tags as nothing whatsoever, which somebody has nonetheless
# stood in front of and watched offer a pick-up. Written with src "seen", which
# the loader counts as vouched the way it counts the game's own tag, since a
# screenshot of the Take prompt plus a log is not a guess.
#
# Keep this list short and keep the evidence with each entry. LuxDragon, 13
# September 2026: a screenshot of "Coins / Take" on a cave floor, and four logs
# in which every one of these was refused as "not ready (node empty)", one at
# 0.6 m. The minigame coins are deliberately absent: 22 gimmick_minigame_seotda_
# prefabs and the ceelo and cushionball ones carry "coin" in the name and are
# gambling props, so a name rule here would have swept them all in.
# Empty: the folder rule at the end of kind_for covers the coin piles this
# once held, and one rule with evidence behind it beats a list of two.
SEEN_PICKUPS = {}

# Fallback for gather nodes the table does not tag, read off the prefab name.
# Ordered: the first hit wins. Never consulted for a gimmick_attach_ prefab;
# see kind_for().
NAME_KIND = [
    ("wood",  ("_log", "log_", "firewood", "timber", "stump", "branch", "_tree", "tree_")),
    # No ore branch. The game tags every mine and ore gather node it has, so a
    # name guess here can only add a false positive, and it added 49 of them:
    # boss armour plates, gold bars, paper stars, a naval mine, a spear.
    ("stone", ("_rock", "rock_", "_stone", "stone_", "boulder", "pebble")),
    ("plant", ("grass", "herb", "flower", "mushroom", "weed_", "moss", "fern", "vine",
               "leaf", "leaves", "seaweed", "algae", "reed", "bush", "shrub")),
]

STRIP_PREFIX = ("gimmick_", "socket_collection_", "collect_", "collection_", "attach_",
                "nature_", "tree_", "plant_")
STRIP_SUFFIX = re.compile(r"_(scenecollector|hero|big|giant|thin|burnt|small|root|drop|index\d+|\d+)$")


def strings(rec):
    return [s.decode("ascii", "ignore") for s in re.findall(rb"[ -~]{4,}", rec)]


def stem(name):
    s = name.lower()
    for _ in range(4):
        for p in STRIP_PREFIX:
            if s.startswith(p):
                s = s[len(p):]
                break
        else:
            break
    for _ in range(4):
        s2 = STRIP_SUFFIX.sub("", s)
        if s2 == s:
            break
        s = s2
    return s


def prefab_key(path):
    """Basename without the extension, and without the scene-collector wrapper.

    A placed object often points at a wrapper prefab whose name is the gimmick's
    own with `_scenecollector` on the end, in a different folder, so the folder
    is dropped and the suffix normalised away on both sides of the lookup.
    """
    base = path.rsplit("/", 1)[-1].lower()
    if base.endswith(".prefab"):
        base = base[:-7]
    if base.endswith("_scenecollector"):
        base = base[:-len("_scenecollector")]
    return base


KIND_NOUN = {"plant": "Plant", "ore": "Ore", "stone": "Stone", "wood": "Wood", "item": "Crop",
             "pickup": "Object", "container": "Container"}

# Kinds that reached pretty() with no noun of their own, reported at the end of
# a run. Add a kind to KIND_NOUN above and this stays empty.
NOUNLESS = {}


def pretty(name, kind):
    """A label for a node whose socket name gives no item, e.g. a named tree.

    The raw row names run long (gimmick_tree_pine_spruce_norway_hero_drop_01),
    and they are read in the Nearby list and the log, so they are trimmed to
    the part that says something and fall back to the kind when nothing does.
    """
    s = name.lower()
    if s.startswith("gimmick_"):
        s = s[len("gimmick_"):]
    if s.endswith("_scenecollector"):
        s = s[:-len("_scenecollector")]
    s = re.sub(r"_\d+$", "", s).replace("_", " ").strip()
    if not s or len(s) > 32:
        if kind not in KIND_NOUN:
            NOUNLESS[kind] = NOUNLESS.get(kind, 0) + 1
        return KIND_NOUN.get(kind, "Node")
    return s[0].upper() + s[1:]


def load_items():
    by_key, by_name = {}, {}
    path = os.path.join(DATA, "items_tagged.csv")
    if not os.path.exists(path):
        sys.exit("items_tagged.csv not found; run build_item_db.py first")
    for r in csv.DictReader(open(path, encoding="utf-8-sig")):
        by_key[r["string_key"].lower()] = r
        by_name.setdefault(re.sub(r"[^a-z0-9]", "", r["name"].lower()), r)
    return by_key, by_name


# An item is only accepted as a node's yield when its class fits the kind the
# tags gave. Without this a copper vein matches Money_Copper, the coin, and the
# node is then judged by the rules for currency.
KIND_CLASSES = {
    "wood":  {"wood", "crafting-material"},
    "ore":   {"ore", "jewel", "mineral", "metal", "stone"},
    "stone": {"stone", "ore", "jewel", "mineral"},
    "plant": {"herb", "mushroom", "seed", "alchemy-material", "flower", "ingredient",
              "crafting-material", "vegetable", "fruit", "grain"},
    "item":  {"vegetable", "fruit", "grain", "ingredient", "herb", "seed", "trade-good", "goods"},
}


def refers_to(base, name, string_key):
    """Does this node's own prefab or name refer to the item, loosely?

    Loose on purpose: it decides whether to keep a yield, and a false no costs
    nothing worse than the node falling back on its kind's switch, which is what
    it did before the yields column existed.
    """
    ctx = set()
    for src in (base, name):
        ctx |= {w for w in re.split(r"[^a-z0-9]+", src.lower()) if len(w) > 3}
    want = {w for w in re.split(r"[^a-z0-9]+", string_key.lower()) if len(w) > 3}
    if ctx & want:
        return True
    return any(a.startswith(b) or b.startswith(a) for a in ctx for b in want)


def match_item(name, kind, by_key, by_name):
    # The row's own key, before stem() takes it apart. STRIP_PREFIX eats
    # "collection_" and STRIP_SUFFIX eats "_0001", so
    # gimmick_collection_prop_cup_0001 reaches the candidates below as
    # "prop_cup" and never gets the chance to match Collection_Prop_Cup_0001,
    # which is its own name spelled out. That cost the Golden Goblet its class:
    # with no item and no yield the engine could not tell it was a container,
    # so it answered to Ground items alone and the Containers switch missed it.
    #
    # An exact hit on an item key is identity rather than a guess, so it is
    # taken without the class filter the guesses below need. Measured against
    # the shipped table: it resolves 251 rows, every one of them a pickup or
    # container row that had no item at all, 250 of the 251 container-classed
    # or furniture-tagged, and it contradicts none of the rows that already
    # name an item. No row of any other kind gains one.
    exact = name.lower()
    for p in ("gimmick_", "cd_"):
        if exact.startswith(p):
            exact = exact[len(p):]
            break
    it = by_key.get(exact)
    if it:
        return it
    s = stem(name)
    for c in (s, s.replace("_", ""), s.split("_")[-1]):
        if len(c) < 3:
            continue
        it = by_key.get(c) or by_name.get(c)
        if not it:
            continue
        ok = KIND_CLASSES.get(kind, set())
        if it["klass"] in ok or (it.get("tags") and set(it["tags"].split()) & ok):
            return it
        return None      # a near-miss on the name is worse than no item at all
    return None


# Words that never appear on a mineable vein. Without these the ore guess
# claims shop counters, decorative pipework, abyss puzzle platforms and a
# fountain, all of which then get a 25 m reach and twelve arm calls.
# A gimmick the game drives through states and triggers is a mechanism, not
# something lying on the floor. A piece of loot's record holds its name, an id,
# a catch tag and the common logout effect and nothing else; the Marni EMP
# capsule holds GimmickOnEnterState and PreGimmickOn, and the Demeniss knowledge
# tower holds UnnamedTrigger_0 beside its own shake parameters. Read from the
# row's own strings, the way the tags and the break impulse already are.
#
# Checked against the whole table before it was trusted: of the 197 vouched
# pick-ups that name no item at all, 40 carry one of these and every one of
# them is a mechanism. Coins, gold bars, the breakable pot and the Golden
# Goblet carry none, so the things the pick-up path exists for are untouched.
DRIVEN_MARKERS = ("Trigger", "EnterState", "ExitState", "GimmickOn",
                  "GimmickOff", "PreGimmick", "StateChange")

# The column is written for pick-up rows only. The marker is presumably just as
# true of a driven stone or plant, but the 197 rows counted above were pick-ups
# and the verdict reads this on the pick-up path alone, so marking anything else
# would be a claim wider than the count behind it.
#
# ...except inside the folder the game keeps loose pick-ups in, where a trigger
# on an arrow or a smoke bomb is as likely to be the pick-up itself. Eight
# prefabs sit there and they are left alone; the seventeen outside it are the
# capsules, the tower props, the kinetic tools, the traps and the mechanic core.
LOOSE_ITEM_FOLDER = "/00_common/item/"

NOT_A_VEIN = ("pipe", "shop", "npctable", "store", "fountain", "airballoon",
              "platform", "battery", "conductor", "preset", "sandcrawler",
              "visione", "abyss", "magnet", "vehicle", "cannon", "furnace")


def kind_for(tags, name, prefab, folder=""):
    """The kind, and whether the game said so or the name merely suggested it."""
    if prefab in SEEN_PICKUPS:
        return SEEN_PICKUPS[prefab], "seen"
    # Anything growing in a camp farm, whatever else the game says about it.
    #
    # This was the last rule in the order until 15 September 2026, a fallback
    # that only caught rows nothing else had claimed, and that left one plant
    # answering to two switches depending on how grown it was:
    # camp_farm_cacao_01_phase01 came out "farm" and _phase02 came out "wood",
    # because the second phase carries a collect tag the first one does not.
    # Apple, ensete, figs, orange, peach, pear, pomegranate and rubber all split
    # the same way, glowflower landed under Plants, and abyss_stone managed
    # three kinds across its three phases. LuxDragon asked for consistency on
    # the Nexus posts tab and that is what this is.
    #
    # Above the tag mapping on purpose. The tags are right about what the thing
    # is, a fig tree really is wood, and wrong about whose it is, which is the
    # only question this switch asks. Seeds stay out: nothing yet says whether
    # taking one lifts what the player has planted.
    if "camp_farm" in prefab and "_seed" not in prefab:
        return "farm", False
    # A fruit tree or bush you harvest. The game tags these collect_tree, so
    # they came out wood, and with Crops off and Wood on the mod still shook
    # them: Fyreon87's log of 24 September 2026 gathers an apple tree twice at
    # 12:38 and refuses the apples it let fall as "crops off" a second later.
    # The Crops help names apples on the plant, and the tree is the plant. The
    # same eid was gathered three times in eighteen seconds at 12:26, so a
    # harvest leaves the tree standing and the felling guard is not needed.
    # 21 rows: apple, pear, orange, peach, fig, pomegranate, cacao, grape and
    # ensete. The coconut palms have no _collect and stay wood.
    if FRUIT_PLANT.search(prefab):
        return "item", True
    for t in FRUIT_TAGS:
        if t in tags:
            return "item", True
    for t in tags:
        if t in TAG_KIND:
            return TAG_KIND[t], True
    # A gimmick_attach_ prefab is a piece bolted to a creature or a mechanism:
    # stoneworm and stonetoad plating, stoneowl bases, landspider queen rocks,
    # seraphim stones, thorny vines. None of them is something a player
    # gathers, and gathering one empties its visual while whatever carries its
    # damage volume stays put, which is issue #41. The game tags the six real
    # mining spots under this prefix itself, so the guess can only add false
    # positives here, and it added 110 of them.
    if prefab.startswith("gimmick_attach_"):
        return "", False
    # A collect_ tag beats a catch_ one: a mine you also pick from by hand is
    # still a mine. Sorted so a row carrying two of them answers the same way
    # on every run, since `tags` is a set.
    if not any(w in prefab for w in CATCH_SKIP):
        for t in sorted(tags):
            # Skip this tag, not the rest of them. A row carrying a held tag
            # and a real one sorted after it would otherwise come out kindless.
            # No row in the 13,906 does today, which is why this never showed.
            if t.strip() in CATCH_HOLD:
                continue
            if t.strip() in CATCH_KIND:
                kind = CATCH_KIND[t.strip()]
                if kind == "pickup" and any(w in prefab for w in CONTAINER_WORDS):
                    kind = "container"
                return kind, True
    # Tags the table does not know still mean the game has classified this row,
    # and as something other than a gather node. Guessing from the name here
    # would be second-guessing it.
    if tags:
        return "", False
    # Two folders hold nothing a player gathers, and the name guess reached into
    # both on game 1.0.0.2944. /00_common/faction/ is 130 buildings, statues and
    # business nodes, and management_node_her_timberhamsawmill came out as wood
    # because Timberham is a place with "timber" in it. /00_common/spot/ is map
    # locations, and cd_spot_altarstone came out as stone. A wood or stone row
    # answers to a switch that is on by default, so each would have been armed.
    # These were the only two rows either folder gave the table.
    if "/00_common/faction/" in folder or "/00_common/spot/" in folder:
        return "", False
    # A crafting station is never a node. gimmick_craft_millestone_01's three
    # parts sit in /00_common/farm/ and came out as stone because the name has
    # "stone" in it. All 25 gimmick_craft_ prefabs are stations, and those
    # three were the only rows the prefix gave the table.
    if prefab.startswith("gimmick_craft_"):
        return "", False
    low = name.lower()
    for kind, words in NAME_KIND:
        if kind == "ore":
            # Whole segments only. Anywhere-in-the-string is what let a bare
            # "ore" hide inside "core" and "store".
            if any(w in NOT_A_VEIN for w in low.split("_")):
                continue
            if any(t in NOT_A_VEIN for t in NOT_A_VEIN if t in low):
                continue
            if any(seg == w or seg.startswith(w) for seg in low.split("_") for w in words):
                return kind, False
            continue
        if any(w in low for w in words):
            return kind, False
    # Nothing tagged it and nothing in the name says what it is. If it sits in
    # the folder the game keeps loose pick-ups in, that is what it is: the
    # probe of 13 September 2026 found a coin pile there built exactly like
    # every other empty gimmick, and six of them were picked up once the table
    # knew about them. Last in the order on purpose, so a dried herb keeps the
    # plant its name earned it and only the unnameable reach this.
    if "/00_common/item/" in folder:
        if any(w in prefab for w in CONTAINER_WORDS):
            return "container", "folder"
        return "pickup", "folder"
    return "", False


# --------------------------------------------------------------------- yields
# What a node can hand over, read out of the gimmick row itself rather than
# guessed from its name. An item key inside the record wearing one of two
# four-byte markers immediately in front of it is a drop entry; the markers were
# found by taking the 131 rows whose yield the name match already answers and
# asking what sits beside the answer.
#
# Two filters, both earned. Keys below 1000 are the currencies, and a u32 holding
# 1 or 2 turns up all over these records, so Money_Copper was coming back as the
# yield of half the world. And a handful of real items appear against more than a
# thousand prefabs each, which is a shared block in the record and not anything
# those nodes pay: an item that is the yield of everything is the yield of
# nothing. Anything over BOILERPLATE_AT rows is dropped.
#
# Checked against those 131 known rows: 69 sets contain the known item outright,
# 4 name a different item of the same class (Fine_Stone where the name match said
# Stone_Quarry, Wild_Insam where it said Possesion_Insam), and 58 come back empty,
# which is no opinion and costs nothing. 225 prefabs that had no yield at all gain
# one. The engine only ever uses a set to refuse a node when every item in it is
# refused, so a spurious entry makes it more willing to touch the node and never
# less; a missing entry is the risk, which is what the learned [NodeYields] net
# is still there for.
MARKERS = (0x01000000, 0xFFFF0000)
MIN_ITEM_KEY = 1000
BOILERPLATE_AT = 80

# BOILERPLATE_AT asks whether one key turns up in too many records. These two ask
# the question the other way round, about the record, and both came out of issue
# #65: a flower butterfly was listed as yielding an Abyss Artifact, which made the
# mod pass the node over with Skip quest equipment on.
#
# That row marker-matches thirteen item keys. Twelve are common enough to be
# dropped, and the survivor was the artifact, so its one listed yield was not the
# generator learning what a butterfly pays out. It was the frequency filter
# handing back whichever of thirteen guesses happened to be rare.
#
# Two things had to be measured before either rule below, because the obvious
# reading is wrong: 473 of the 542 rows that ship a yield have most of their
# candidates discarded, and they are almost all correct (a mushroom box yields
# Pine Mushroom, a fish basket yields Dried Fish). "Most were discarded" is the
# normal case and is not evidence of anything. Nor is "the yield shares no word
# with the node": a cereal sack yields Barley and a herb sack yields Parsley.

# A record offering more than this many surviving candidates is a dropset or a
# box and is declaring nothing. Counted before choosing the number: 517 rows list
# one yield, 10 list two, 14 list three, and exactly one lists 33, which is
# gimmick_item_dropset_treasurebox_01. There is nothing in between, so this hits
# that row and nothing else.
MAX_YIELDS = 4

# An item the player cannot throw away is the kind a wrong yield does damage
# with, because the mod refuses a node it believes holds one. 45 shipped yields
# are marked no-discard, quest or important; 41 of them are on a node whose own
# name says nothing about it, and those 41 read as a butterfly yielding an abyss
# artifact, a drug bottle yielding a leather cloak, a kuku egg yielding riding
# boots. The four the node does name are right every time: a graymane clue
# yielding a Broken Graymane Token, a hand mirror yielding a Scorched HandMirror.
# So one of these is kept only when the prefab or the row's name refers to it.
GUARDED_TAGS = ("no-discard", "quest", "important")


def yield_candidates(rec, item_keys):
    """Item keys in this record that wear a drop marker, in record order."""
    out = []
    for off in range(4, len(rec) - 3):
        v = struct.unpack_from("<I", rec, off)[0]
        if v not in item_keys:
            continue
        if struct.unpack_from("<I", rec, off - 4)[0] not in MARKERS:
            continue
        if v not in out:
            out.append(v)
    return out


def main():
    rows = cdtables.load_table("gimmickinfo")
    by_key, by_name = load_items()
    out, seen = [], set()
    stats = {"rows": 0, "with_path": 0, "tagged": 0, "by_name": 0, "with_item": 0, "with_yields": 0,
             "yields_dropped_crowded": 0, "yields_dropped_guarded": 0}

    # Every row's candidates first, so the boilerplate can be counted before any
    # of it is written down.
    # by_key is indexed by string key; the record holds the numeric one.
    by_num = {}
    for r in by_key.values():
        try:
            by_num[int(r["key"])] = r
        except (KeyError, ValueError):
            pass
    item_keys = set(by_num)
    # Every tag any record gives a prefab, gathered before anything is decided.
    # gimmick_box_bucket_02a_unbreak has two records, one tagged catch_pot and
    # one tagged catch_barrel, and the loop below keeps the first basename it
    # meets. That made the kind depend on the order the table happens to be in:
    # the pot record comes first, so a barrel was filed as an item. Reading the
    # union means both tags are on the table when kind_for is asked.
    tags_by_base = collections.defaultdict(set)
    # Same for the state and trigger markers, and for the same reason. 107
    # prefabs have more than one gimmickinfo record and 104 of them disagree
    # about whether a marker is there, so reading it off whichever record came
    # first makes the answer depend on the order of the table. No row is
    # affected today, because none of the 107 is a pick-up that names nothing;
    # that is luck, and the tags above were read the same way until a bucket
    # tagged catch_pot in one record and catch_barrel in another proved it.
    driven_by_base = collections.defaultdict(bool)
    # Whether the game files this prefab among its loose items. That is the
    # game's own answer to "is this picked up or harvested", and it is the one
    # signal that separates a stack of firewood from a tree. Unioned across
    # records for the same reason the tags are.
    loose_by_base = collections.defaultdict(bool)
    for _key, rec in rows:
        ss = strings(rec)
        path = next((s[:s.find(".prefab") + len(".prefab")] for s in ss if ".prefab" in s), "")
        if not path:
            continue
        base = prefab_key(path)
        tags_by_base[base].update(s for s in ss if s.startswith(("collect", "catch")))
        loose_by_base[base] |= LOOSE_ITEM_FOLDER in path.lower()
        if LOOSE_ITEM_FOLDER not in path.lower():
            driven_by_base[base] |= any(m in s for m in DRIVEN_MARKERS for s in ss)

    cand_by_path, freq = {}, collections.Counter()
    for _key, rec in rows:
        c = yield_candidates(rec, item_keys)
        if not c:
            continue
        for v in c:
            freq[v] += 1
        for s in (x.decode("ascii", "ignore") for x in re.findall(rb"[ -~]{4,}", rec)):
            i = s.find(".prefab")
            if i >= 0:
                cand_by_path[prefab_key(s[:i + len(".prefab")])] = c
                break
    for key, rec in rows:
        stats["rows"] += 1
        ss = strings(rec)
        if not ss:
            continue
        name = ss[0]
        # Cut at the extension rather than requiring it at the end: the byte
        # after the path is often printable, so strings() hands back
        # "..._scenecollector.prefabxC" and endswith() dropped 47% of the table.
        path = ""
        for s in ss:
            i = s.find(".prefab")
            if i >= 0:
                path = s[:i + len(".prefab")]
                break
        if not path:
            continue
        stats["with_path"] += 1
        tags = {s for s in ss if s.startswith(("collect", "catch"))}
        # Does the game break this node, or is it picked up? A vein carries a
        # self-break impulse or a break projectile; the chunks a vein drops
        # carry neither. Read from the row own strings, like the tags.
        breaks = any(s in ("SelfForceBreakImpulse", "BreakProjectileKey") for s in ss)
        # A plant the game hands over by changing its own state, and never
        # through the collect interaction arming uses. Its handler drops on
        # entering GimmickOn rather than on an attack or a collect, so arming
        # it does nothing at all: LuxDragon armed Palmar Leaves 270 times in
        # one session on 17 September 2026 and not one filled, while three he
        # picked by hand each showed the state event on the plant. Driving that
        # event picks them, confirmed in his game the same evening.
        #
        # Six records in the whole file drop on entering GimmickOn, and the two
        # tagged collect_botany are these plants; the other four are a digging
        # spot, two totems and a memo. The tag is what keeps this to plants.
        base = prefab_key(path)
        # The row's handler XML arrives as one string per line, so it is joined
        # back together before the state is read out of it.
        handler = chr(10).join(ss)
        statepick = ("collect_botany" in (tags_by_base.get(base) or tags)
                     and any('Type="Drop"' in g for g in
                             re.findall(r"<GimmickOn[^>]*>(.*?)</GimmickOn>", handler, re.S)))
        driven = driven_by_base[base]
        kind, vouched = kind_for(tags_by_base.get(base, tags), name, base, path)
        if not kind:
            continue
        stats["tagged" if vouched else "by_name"] += 1
        if base in seen:
            continue
        seen.add(base)
        it = match_item(name, kind, by_key, by_name)
        if it:
            stats["with_item"] += 1
        yields = [by_num[v]["string_key"] for v in cand_by_path.get(base, [])
                  if freq[v] <= BOILERPLATE_AT]
        # A record that offers this many is listing a loot table, not a yield.
        if len(yields) > MAX_YIELDS:
            stats["yields_dropped_crowded"] += 1
            yields = []
        kept = []
        for y in yields:
            row = by_key.get(y.lower())
            guarded = row and (
                set((row.get("tags") or "").split()) & set(GUARDED_TAGS)
                or (row.get("important") or "").strip() in ("1", "True", "true")
                or (row.get("discardable") or "").strip() in ("0", "False", "false"))
            if guarded and not refers_to(base, name, y):
                stats["yields_dropped_guarded"] += 1
                continue
            kept.append(y)
        yields = kept
        if yields:
            stats["with_yields"] += 1
        out.append((base, kind, it["string_key"] if it else "",
                    it["name"] if it else pretty(name, kind),
                    # kind_for returns True for the game's own tag, False for a
                    # name guess, or the name of another source it trusts.
                    vouched if isinstance(vouched, str) else ("tag" if vouched else "name"),
                    "1" if breaks else "0",
                    " ".join(yields),
                    "1" if (driven and kind == "pickup" and not it and not yields) else "0",
                    # Reached with the pick-up verb, whatever kind it is. A
                    # collect_ tag beats a catch_ one when the two disagree
                    # about what a thing is, and that is right: a mine you can
                    # also pick from by hand is still a mine. It says nothing
                    # about how to reach one. 120 firewood prefabs carry
                    # collect_wood and catch_boxsmall at once and the mod
                    # believed the first, so it armed them and waited, and a
                    # pick-up never fills however long you stand there:
                    # LuxDragon watched one take eight arms at three metres, and
                    # it is the last prefab in this folder still doing it on the
                    # release build. Issue #63.
                    #
                    # The folder decides, not the tag pair. 310 prefabs carry
                    # both families and they include 89 trees and 16 mines,
                    # which do fill when armed and must keep doing so. Of the
                    # 450 rows the game files under the loose-item folder, 229
                    # have a kind of their own: 141 wood, 81 container, 6 plant
                    # and 1 stone. No ore and no tree among them.
                    "1" if loose_by_base[base] else "0",
                    "1" if statepick else "0"))
    out.sort()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        # src says whether the game's own gimmick tag gave the kind or the
        # generator guessed it from the prefab name. The engine spends the
        # long ore reach only on the ones the game vouches for.
        f.write("prefab\tkind\titem_key\tname\tsrc\tbreaks\tyields\tdriven\tpickup\tstatepick\n")
        for r in out:
            f.write("\t".join(r) + "\n")
    kinds = {}
    for row in out:
        k = row[1]
        kinds[k] = kinds.get(k, 0) + 1
    print("gimmick rows %(rows)d, with a prefab path %(with_path)d, "
          "classified by tag %(tagged)d, by name %(by_name)d, item resolved %(with_item)d, "
          "drop candidates read %(with_yields)d" % stats)
    print("wrote %d rows to %s" % (len(out), os.path.relpath(OUT, HERE)))
    print("by kind: " + ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    for k in sorted(NOUNLESS):
        print("warning: kind %r has no entry in KIND_NOUN, so %d rows are named "
              "\"Node\" and will read as \"Node node\" in the menu and the log"
              % (k, NOUNLESS[k]))
    ore = [r for r in out if r[1] == "ore"]
    print("ore: %d rows, %d vouched for by the game's own tag, %d that do not break"
          % (len(ore), sum(1 for r in ore if r[4] == "tag"), sum(1 for r in ore if r[5] == "0")))


if __name__ == "__main__":
    main()
