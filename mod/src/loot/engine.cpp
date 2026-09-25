#include "engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "events.h"
#include "game.h"
#include "hooks.h"
#include "mem.h"
#include "signatures.h"
#include "../core/creaturedb.h"
#include "../core/nodedb.h"
#include "../hooks/xinput_hook.h"
#include "../hooks/dx12_hook.h"
#include "../core/itemdb.h"
#include "../core/mod.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../core/rules.h"
#include "../core/settings.h"
#include "../core/state.h"
#include "../gui/storage_link.h"

namespace ml::loot
{
    using events::Action;
    using game::Vec3;
    using namespace ml::sig;

    // ------------------------------------------------------------ shared ----
    static HANDLE g_thread = nullptr;
    static volatile LONG g_running = 0, g_burst = 0, g_forget = 0;
    static bool g_debugLog = false;          // the snapshot's DebugLog, for the diagnostics in Fill
    static std::mutex g_mu;                 // status, nearby, recent
    static Status g_status;
    static std::vector<Nearby> g_nearby;
    static std::vector<Recent> g_recent;
    static long g_session[4] = {};

    static void Note(const char* fmt, ...)
    {
        char b[96];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        std::lock_guard<std::mutex> lk(g_mu);
        strncpy(g_status.note, b, sizeof g_status.note - 1);
    }

    static void PushRecent(const char* text)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Recent r; strncpy(r.text, text, sizeof r.text - 1); r.text[sizeof r.text - 1] = 0; r.when = GetTickCount();
        g_recent.insert(g_recent.begin(), r);
        if (g_recent.size() > 12) g_recent.pop_back();
    }

    // -------------------------------------------------------- candidates ----
    struct Cand
    {
        uintptr_t ent = 0;
        uint32_t eid = 0, route = 0, iid = 0, parent = 0;
        uint16_t tid = 0;    // item row when this is an item; the gather block's id otherwise
        uint16_t gtid = 0;   // the gather block's own id, which is not stable between sessions
        uint8_t  type = 0xFF, cat = 0, cat2 = 0, dead = 0, locked = 0, gkind = 0;
        bool inter = false, item = false, gather = false, ai = false, noIid = false;
        bool twin = false, heap = false, mine = false, filled = false, banned = false;
        Vec3 pos;
        float d = 0;
        char key[64] = "";    // engine string key from the live table
        char node[160] = "";  // the node's prefab path, which is also its name
        const Item* db = nullptr;
        const NodeType* nodeType = nullptr;  // what the prefab says this node is
        const Creature* species = nullptr;   // the creature named, or a representative of its species word
        bool speciesExact = false;           // `species` is the creature itself, not a stand-in
        const char* speciesClass = nullptr;  // "insect", "fish", "seafood", "animal", "amphibian" or null
    };

    // --- creature species -------------------------------------------------------
    // Nothing in the entity names its species, but the animation and schedule
    // assets it drags along do ("cd_m0002_rat", "schedule_action_goose"), and
    // every game data row keeps its key at +0x08. Every string reachable within
    // two hops of the creature is read and classified by the species words it
    // contains. The result is cached per entity; a miss is remembered too.
    // Items that only ever come from a catch, never from a gather node.
    static bool IsCreatureItem(const Item* it)
    {
        return it && (it->klass == "insect" || it->klass == "fish" || it->klass == "animal" || it->klass == "amphibian" || it->HasTag("shellfish"));
    }

    struct Species { const Creature* row = nullptr; const char* klass = nullptr; bool exact = false; int trust = 0; std::string word, from; };
    static std::unordered_map<uint32_t, Species> g_speciesByEid;
    static std::unordered_set<uint32_t> g_speciesMiss;

    // The category byte narrows what a creature can be: 05 swims or flies
    // (fish, seafood, butterflies, small birds), 09 walks (ground insects,
    // small animals, frogs, geese). A species word that contradicts it is a
    // false match; a byte-05 "animal" is only believed when it is a bird.
    static bool IsBird(const Species& sp)
    {
        static const char* words[] = { "bird", "smallbird", "embriza", "goose", "duck", "chicken", "hen", "rooster", "coot", "crow", "pigeon" };
        for (const char* w : words) if (sp.word == w) return true;
        if (sp.row) if (const Item* it = ItemDb::ByRow(sp.row->itemRow)) return it->HasTag("bird");
        return false;
    }
    // Byte 05 turned out to be a movement state rather than a family: a goose
    // on a pond and a rat crossing a stream are 05 too. An animal is believed
    // there when it is a bird or when its own model string says so.
    static bool Plausible(uint8_t cat2, const Species& sp)
    {
        if (!sp.klass) return false;
        const std::string k = sp.klass;
        if (cat2 == 0x05) return k == "fish" || k == "seafood" || k == "insect" || (k == "animal" && (IsBird(sp) || sp.trust >= 3));
        if (cat2 == 0x09) return k == "insect" || k == "animal" || k == "amphibian" || k == "seafood";
        return true;
    }

    // How far a string may be believed about the creature it hangs off. The
    // creature's own model key sits in one slot and names it outright:
    // cd_fish, cd_m0002_rat, cd_m0003_chicken, cd_common_bird, and for the
    // small bugs, which have no character model, cd_effectmonster_normal. Its
    // movement flag is its own too (UnderWaterOnly, AirOnly). Asset paths and
    // action tables (character/motion/..., upperaction/..., schedule_action_*)
    // leak in from neighbours, so they rank below the key. UI strings name
    // nothing, and one of them, common_icon_keyguide_pc_mouse_x1, taught 0.3.9
    // that beetles were mice.
    static bool HasNoCase(const char* s, const char* sub)
    {
        const size_t n = strlen(sub);
        for (; *s; ++s) if (_strnicmp(s, sub, n) == 0) return true;
        return false;
    }
    static bool EndsWith(const char* s, const char* suffix)
    {
        const size_t n = strlen(s), m = strlen(suffix);
        return n >= m && strcmp(s + n - m, suffix) == 0;
    }
    static int Trust(const char* s)
    {
        if (strchr(s, '-') || strchr(s, ',') || strchr(s, ' ')) return 0;   // CSS-like UI class names
        static const char* ui[] = { "icon", "keyguide", "click", "font", "texture", "ui/", ".dds", "hud", "preset", "cutscene" };
        for (const char* u : ui) if (HasNoCase(s, u)) return 0;
        const bool path = strchr(s, '/') || strchr(s, '.');
        if (!path && !strncmp(s, "cd_", 3)) return 3;                                   // the model key
        if (!path && (EndsWith(s, "Only") || EndsWith(s, "NoWater"))) return 3;         // the movement flag
        return path ? 2 : 1;
    }

    // Reads the strings a slot leads to and keeps the best-trusted species
    // match in `best`. Returns true when `best` improved.
    static bool ReadStringsAt(uintptr_t obj, unsigned off, Species* best, char* probe, size_t probeLen)
    {
        const uintptr_t q = mem::Deref(obj, off);
        if (!q || mem::InImage(q)) return false;
        static const unsigned fields[] = { 0x00, 0x08, 0x10, 0x18, 0x20 };
        char buf[128];
        bool improved = false;
        for (unsigned f : fields)
        {
            if (!mem::ReadEngineString(q + f, buf, sizeof buf)) continue;
            if (probe && probeLen > strlen(probe) + strlen(buf) + 4) { strncat(probe, buf, probeLen - strlen(probe) - 2); strncat(probe, " ", probeLen - strlen(probe) - 1); }
            const int trust = Trust(buf);
            if (trust <= 0 || trust <= best->trust) continue;
            Species sp;
            const Creature* row = CreatureDb::ByKey(buf);
            if (!row) row = CreatureDb::InText(buf);
            if (row)
            {
                static const char* kNames[] = { "insect", "fish", "seafood", "amphibian" };
                sp.row = row; sp.klass = "animal"; sp.exact = true; sp.word = row->stringKey; sp.trust = 4;
                for (const char* k : kNames) if (row->klass == k) sp.klass = k;
            }
            else
            {
                const CreatureDb::Match m = CreatureDb::Classify(buf);
                if (!m.klass) continue;
                sp.row = m.row; sp.klass = m.klass; sp.exact = m.exact; sp.word = m.word; sp.trust = trust;
            }
            sp.from = buf;
            *best = sp;
            improved = true;
        }
        return improved;
    }

    // Which creature is this, asked of the game's own table instead of its
    // model name.
    //
    // Naming a creature from strings has hit its ceiling. Across twelve logged
    // sessions every word the mod ever matched was a generic one: fish arrive
    // as "cd_fish", the small insects as "cd_effectmonster_normal", and an
    // exact species was named three times in total. So c.species is null
    // almost always, the catch branch never reaches ItemDb::ByRow, and no item
    // rule can refuse a live catch. Issue #39.
    //
    // creatures.tsv is keyed by CharacterInfo string key, and the mod can
    // already turn a row index of that table into its key: EnumTables finds
    // characterinfo with 7250 rows and KeyInTable reads a row's name. So if the
    // actor carries its CharacterInfo row anywhere reachable, the whole problem
    // is one offset away.
    //
    // This looks for that offset and does nothing else. Every 2-byte field of
    // the four blocks FindSpecies already walks is tried as a row index, and a
    // hit is only reported when the key it resolves to is a creature our own
    // table knows. That cross-check is what makes the answer unambiguous: a
    // random 16-bit value will land on some row of some table constantly, and
    // almost never on one that names a creature.
    static void ProbeCreatureIdentity(uint32_t eid, uintptr_t ent, uintptr_t actor,
                                      uintptr_t status, uintptr_t ai, uint8_t cat2,
                                      const char* klass)
    {
        if (!CreatureDb::Loaded()) return;
        static std::unordered_set<uint32_t> s_seen;
        if (!s_seen.insert(eid).second) return;

        // A budget per class, not one pool.
        //
        // The category byte does not separate these: fish and the small insects
        // are both 05, so a single pool means walking to a lake spends the whole
        // allowance on insects and the fish, which are the case actually worth
        // probing, never get looked at.
        //
        // A class stops being probed once it has answered three times, which is
        // enough to see whether an offset is stable. The sweep is not cheap:
        // four blocks at two-byte steps, then a hop out, with a table lookup at
        // every step.
        static std::unordered_map<std::string, int> s_tried;
        static std::unordered_map<std::string, int> s_hit;
        const std::string cls = klass ? klass : "unknown";
        if (s_hit[cls] >= 3) return;
        if (s_tried[cls] >= 10) return;
        ++s_tried[cls];

        // Every table, not one picked by name.
        //
        // The first version of this looked up the table called "characterinfo"
        // and took the first match. There are 111 static tables in the image and
        // more than one carries that name: the one it found has four rows, while
        // the real one has 7250. With a count of four, the bounds test threw away
        // every candidate value, so the probe reported "nothing found" on every
        // creature while testing nothing at all.
        //
        // Trying them all removes the guess. It costs more, but the cross-check
        // below is what makes a hit meaningful, not which table it came from: a
        // value only gets reported when the key it resolves to names a creature
        // our own table knows, and that almost never happens by chance.
        const game::TableRef* tables = nullptr;
        const int nTables = game::EnumTables(&tables);
        if (nTables <= 0) { LOG("[cid] %08X: no static tables resolved, cannot probe", eid); return; }

        LOG("[cid] %08X (%s, byte %02X): looking for a row that names a creature, across %d tables",
            eid, cls.c_str(), cat2, nTables);

        // Given a value, does any table turn it into a creature we know?
        auto naming = [&](uint16_t v, char* key, size_t keyN, const char** tableName) -> const Creature*
        {
            if (v < 64) return nullptr;
            for (int i = 0; i < nTables; ++i)
            {
                if (v >= tables[i].count || tables[i].count < 256) continue;
                if (!game::KeyInTable(tables[i].global, v, key, keyN)) continue;
                if (const Creature* cr = CreatureDb::ByKey(key))
                {
                    *tableName = tables[i].name[0] ? tables[i].name : "?";
                    return cr;
                }
            }
            return nullptr;
        };

        const uintptr_t objs[4] = { ent, actor, status, ai };
        const char*     names[4] = { "ent", "actor", "status", "ai" };
        const unsigned  lens[4] = { 0x300, 0x300, 0x400, 0x300 };
        int hits = 0;
        for (int o = 0; o < 4 && hits < 12; ++o)
        {
            if (!objs[o] || !mem::Readable(objs[o], lens[o])) continue;
            for (unsigned off = 0; off + 2 <= lens[o] && hits < 12; off += 2)
            {
                uint16_t v = 0;
                if (!mem::Read16(objs[o] + off, &v)) continue;
                char key[96]; const char* tbl = "?";
                const Creature* cr = naming(v, key, sizeof key, &tbl);
                if (!cr) continue;
                ++hits;
                LOG("[cid] %08X  %s+0x%X = %u -> %s \"%s\" = %s (%s)",
                    eid, names[o], off, v, tbl, key, cr->name.c_str(), cr->klass.c_str());
            }
        }
        // The same blocks again as 32-bit values, tested against the numeric
        // CharacterInfo key rather than a row index.
        //
        // The first probe only read 16-bit fields, which can only ever hold a
        // row index. But creatures.tsv is keyed by character_key, 517 of the
        // 1004 of those do not fit in 16 bits, and the largest is 3653044009.
        // An actor is more likely to carry its own key than an index into a
        // table, so this was the more probable answer and the first version
        // could not have found it.
        for (int o = 0; o < 4 && hits < 12; ++o)
        {
            if (!objs[o] || !mem::Readable(objs[o], lens[o])) continue;
            for (unsigned off = 0; off + 4 <= lens[o] && hits < 12; off += 4)
            {
                uint32_t v = 0;
                if (!mem::Read32(objs[o] + off, &v)) continue;
                const Creature* cr = CreatureDb::ByCharacterKey(v);
                if (!cr) continue;
                ++hits;
                LOG("[cid] %08X  %s+0x%X = %u is the character key of %s (%s)",
                    eid, names[o], off, v, cr->name.c_str(), cr->klass.c_str());
            }
        }

        // One hop out, since FindSpecies finds most of what it finds there.
        for (int o = 0; o < 4 && hits < 12; ++o)
        {
            if (!objs[o] || !mem::Readable(objs[o], lens[o])) continue;
            for (unsigned off = 0; off < lens[o] && hits < 12; off += 8)
            {
                const uintptr_t mid = mem::Deref(objs[o], off);
                if (!mid || mem::InImage(mid) || !mem::Readable(mid, 0x200)) continue;
                for (unsigned off2 = 0; off2 + 2 <= 0x200 && hits < 12; off2 += 2)
                {
                    uint32_t w = 0;
                    if (mem::Read32(mid + off2, &w))
                        if (const Creature* ck = CreatureDb::ByCharacterKey(w))
                        {
                            ++hits;
                            LOG("[cid] %08X  [%s+0x%X]+0x%X = %u is the character key of %s (%s)",
                                eid, names[o], off, off2, w, ck->name.c_str(), ck->klass.c_str());
                            continue;
                        }
                    uint16_t v = 0;
                    if (!mem::Read16(mid + off2, &v)) continue;
                    char key[96]; const char* tbl = "?";
                    const Creature* cr = naming(v, key, sizeof key, &tbl);
                    if (!cr) continue;
                    ++hits;
                    LOG("[cid] %08X  [%s+0x%X]+0x%X = %u -> %s \"%s\" = %s (%s)",
                        eid, names[o], off, off2, v, tbl, key, cr->name.c_str(), cr->klass.c_str());
                }
            }
        }
        if (hits) ++s_hit[cls];
        else LOG("[cid] %08X (%s): nothing resolves to a creature this table knows, %d of 10 tried",
                 eid, cls.c_str(), s_tried[cls]);
    }

    static Species FindSpecies(uint32_t eid, uintptr_t ent, uintptr_t actor, uintptr_t status, uintptr_t ai, uint8_t cat2, uint8_t tag)
    {
        Species sp;
        if (!CreatureDb::Loaded()) return sp;
        auto hit = g_speciesByEid.find(eid);
        if (hit != g_speciesByEid.end()) return hit->second;
        if (g_speciesMiss.count(eid)) return sp;

        // The creature says what it is, at status+0x30.
        //
        // Everything below this is a guess built out of model names, and it had
        // run out: across twelve sessions every word it matched was a generic
        // one, and it named an exact species three times. Fish all arrive as
        // "cd_fish" and the small insects as "cd_effectmonster_normal", so
        // c.species stayed null, the catch branch never reached the item
        // verdict, and no item rule could refuse a live catch. Issue #39.
        //
        // status+0x30 is a 16-bit row of the characterinfo table and it names
        // the creature outright. Found by sweeping every field of the four
        // blocks this function walks and reporting only values that resolved to
        // a creature our own table knows: it was the most frequent hit by a
        // margin, and the class always agreed. Three fish in a shoal all read
        // 4000, Small Rasbora; two crickets read 3860, Camel Cricket; a firefly
        // cluster read 3932. The handful of other offsets that ever hit gave a
        // different animal each time, which is what coincidence looks like.
        //
        // Exact, because this is the game's own answer rather than a word that
        // might be shared. The string walk stays as the fallback for anything
        // this cannot name.
        if (status)
        {
            uint32_t rows = 0;
            if (const uintptr_t chr = game::CharacterInfoTable(&rows))
            {
                uint16_t row = 0;
                if (mem::Read16(status + 0x30, &row) && row && row < rows)
                {
                    char key[96];
                    if (game::KeyInTable(chr, row, key, sizeof key))
                        if (const Creature* cr = CreatureDb::ByKey(key))
                        {
                            sp.row = cr; sp.klass = cr->klass.c_str();
                            sp.exact = true; sp.trust = 5;
                            sp.word = cr->name; sp.from = key;
                            static int s_said = 0;
                            if (s_said < 12)
                            {
                                ++s_said;
                                LOG("[species] %08X is %s (%s), from characterinfo row %u at status+0x30",
                                    eid, cr->name.c_str(), cr->klass.c_str(), row);
                            }
                            g_speciesByEid[eid] = sp;
                            return sp;
                        }
                }
            }
        }
        // A budget per category byte rather than two buckets. The old pair sent
        // every byte that was not 05 into the 09 bucket, so with live creatures
        // now identified whatever their byte, a field of beasts would spend the
        // whole allowance before the player reached the animal the log was
        // being taken for.
        static std::unordered_map<uint8_t, int> s_dumped;
        int& dumped = s_dumped[cat2];
        const bool dump = dumped < 10;
        char probe[1400] = "";
        const uintptr_t objs[4] = { ent, actor, status, ai };
        const unsigned  lens[4] = { 0x300, 0x300, 0x400, 0x300 };
        // Every string within two hops is read and the best-trusted match
        // wins: the creature's own model key beats a leaked asset path, and
        // an exact creature key (trust 4) ends the search.
        for (int o = 0; o < 4 && sp.trust < 4; ++o)
        {
            if (!objs[o] || !mem::Readable(objs[o], lens[o])) continue;
            for (unsigned off = 0; off < lens[o] && sp.trust < 4; off += 8)
            {
                ReadStringsAt(objs[o], off, &sp, dump ? probe : nullptr, sizeof probe);
                const uintptr_t mid = mem::Deref(objs[o], off);
                if (!mid || mem::InImage(mid) || !mem::Readable(mid, 0x200)) continue;
                for (unsigned off2 = 0; off2 < 0x200 && sp.trust < 4; off2 += 8)
                    ReadStringsAt(mid, off2, &sp, dump ? probe : nullptr, sizeof probe);
            }
        }
        bool found = sp.klass != nullptr;
        if (found && !Plausible(cat2, sp))
        {
            static int s_rejected = 0;
            if (s_rejected < 20) { ++s_rejected; LOG("[species] %08X (byte %02X) cannot be %s (%s): word '%s' in \"%s\" (trust %d); treated as unidentified", eid, cat2, sp.klass, sp.row ? sp.row->name.c_str() : "-", sp.word.c_str(), sp.from.c_str(), sp.trust); }
            found = false;
            sp = Species();
        }
        if (found)
        {
            if (g_speciesByEid.size() > 4096) g_speciesByEid.clear();
            g_speciesByEid[eid] = sp;
            static int s_hits = 0;
            // The tag is here because it is the half of the catchable test that
            // usually does the excluding, and a line without it cannot be held
            // against one from a creature that was caught.
            if (s_hits < (g_debugLog ? 200 : 30)) { ++s_hits; LOG("[species] %08X (byte %02X tag %02X) is %s%s%s: word '%s' in \"%s\" (trust %d)", eid, cat2, tag, sp.klass, sp.row ? (sp.exact ? ", " : ", e.g. ") : "", sp.row ? sp.row->name.c_str() : "", sp.word.c_str(), sp.from.c_str(), sp.trust); }
            return sp;
        }
        if (dump)
        {
            ++dumped;
            uintptr_t vt = 0, ti = 0; mem::ReadPtr(ent, &vt); mem::ReadPtr(ent + kOff_Ent_TypeInfo, &ti);
            LOG("[species] no match for %08X (byte %02X tag %02X, vtable +0x%llX typeinfo %llX); strings seen: %s", eid, cat2, tag,
                static_cast<unsigned long long>(mem::Rva(vt)), static_cast<unsigned long long>(ti), probe[0] ? probe : "(none)");
        }
        if (g_speciesMiss.size() > 4096) g_speciesMiss.clear();
        g_speciesMiss.insert(eid);
        return sp;
    }

    // `own` is the player's own kit: worn, carried, or pointing back at them.
    // It is never lootable and there can be two dozen pieces of it inside two
    // metres, which is enough to fill the Nearby list before anything in the
    // world is reached, so the list leaves it out.
    // nodeReach: a pick-up the mod only knows about because the node table
    // vouched for the prefab. It is sent as a pick-up but it is reached like a
    // node, so it answers to the gather range and not the longer loot one.
    struct Verdict { bool loot = false; bool own = false; bool nodeReach = false; bool pick = false; Action act = Action::Take; const char* why = ""; char detail[40] = ""; };

    // The event the game itself sends at a plant that is picked by hand, read
    // out of LuxDragon's log of 17 September 2026: three Palmar Leaves picked
    // by hand, each one this event on the plant's own gimmick component. It is
    // also the event the well bucket takes water on, which is the same shape of
    // thing. The plants it is sent to are the ones the node table marks, never
    // a name written here.
    static constexpr uint32_t kPickEvent = 0x003ECC59;
    static bool StatePick(const NodeType* t) { return t && t->statePick; }

    // Memories. Keys: instance id when the node has one (survives respawns),
    // otherwise the entity id.
    // Instance ids seen on more than one world object at once. A stack dropped
    // from the bag comes down as several objects that all carry the stack's
    // id: on 18 September 2026 a pile of Palmar Pills dropped by hand came back
    // one per retry delay, because one send put the id on the retry timer and
    // held the rest of that stack, and their tries added up until the mod gave
    // one up as not responding while it lay there. Those objects are keyed by
    // entity.
    static std::unordered_set<uint32_t> g_sharedIid;
    static uint64_t Key(const Cand& c) { return c.iid && !g_sharedIid.count(c.iid) ? (0x100000000ull | c.iid) : c.eid; }
    struct Done { DWORD when; int tries; };
    static std::unordered_map<uint64_t, Done>  g_done;      // recently sent
    static std::unordered_set<uint64_t>        g_searched;  // never again this session
    // The same retirements, by entity. Key() prefers an item's instance id,
    // and that is not read until Fill has run, so the scan cannot ask
    // g_searched about an object it has not filled yet. This is what it asks
    // instead, and it is filled in as each retired object is recognised once.
    static std::unordered_set<uint32_t>        g_retiredEid;

    // The ownership oracle is a call into the game, and it is the last gate in
    // Decide, which runs thirty times a second over every candidate in range.
    // The answer does not change at that rate. One thrown sword was asked
    // thirty-four times in 1.1 seconds while its send was being throttled: the
    // same question, the same answer, thirty-four calls into the game.
    //
    // Two seconds is short enough that a door opening or a quest handing
    // something over is noticed within a tick or two of a scan, and long
    // enough that the repeat collapses to one call.
    struct OwnAns { DWORD when; int steal; };
    static std::unordered_map<uint32_t, OwnAns> g_ownAns;
    static constexpr DWORD kOwnAnsMs = 2000;

    // Everything the scan has seen hanging off the player, and the last moment
    // it did. Being attached is the one thing that keeps the player's own kit
    // out of the verdict, and a thrown weapon stops being attached for as long
    // as it is in the air. Nothing else marks it out: a thrown sword is the
    // same weapon class as one off a corpse.
    //
    // The item row is kept beside the time because entity ids get recycled. An
    // id alone would hand a stale refusal to whatever object inherits it.
    struct OnMe { DWORD when; uint16_t tid; };
    static std::unordered_map<uint32_t, OnMe> g_onMe;
    // Long enough for a weapon thrown across a field and collected on the way
    // past, short enough that a recycled id has usually aged out.
    static constexpr DWORD kOnMeWindowMs = 45000;

    // What the player drops out of the bag by hand stays where it lands.
    // Until this, a dropped item was an ordinary world object to the scan and
    // went straight back into the bag; 1.6.28 even made a dropped stack come
    // back in one pass instead of one piece per retry delay.
    //
    // The drop is seen on the server thread, in its parse of the discard
    // request (HandDropParse). The object it leaves is recognised on the scan
    // thread (ClaimHandDrops) two ways. The first is the instance id: the
    // world object keeps the bag slot's, which the session of 24 September
    // 2026 showed on all three drops in it, a hide and two shovels, so the
    // slot's id and row are refused the moment the scan hears of the drop,
    // wherever the object lands and however late it turns up. The second is
    // the older match by item row, by lying where the game was asked to put
    // it, and by first appearing after the drop, kept for any item that turns
    // out to get a new id when it lands. A pick-up does hand the bag a new id
    // (the duplicate probe of 15 September 2026, 133 tries), so the agreement
    // runs one way only and nothing else here leans on it.
    struct HandDrop
    {
        uint16_t tid = 0;
        uint32_t bagIid = 0;
        int      left = 0;          // objects still to recognise; a stack can land as one per unit
        int      claimed = 0;
        DWORD    claimedAt = 0;     // the first recognition; the rest of a stack lands with it
        float    world[3] = {};     // where the game was asked to put it, in its own world frame
        DWORD    at = 0;            // when the server parsed the request
        uint32_t pass = 0;          // the last scan pass fully recorded by then; anything first seen later is new
        bool     scanLive = false;  // the scan was running then, so it knows what already lay there
        Vec3     stamp{};           // the scan centre when the scan took the note
        bool     useStamp = false;  // measure from the stamp, because the frames did not convert
        DWORD    seenAt = 0;        // when the spot first came within reach of the scan; the window runs from here
        float    nearest = -1.0f;   // the closest thing of the same row the scan saw, for the log
    };
    static SRWLOCK g_handDropLock = SRWLOCK_INIT;
    static std::vector<HandDrop> g_handDropIn;        // parsed, not yet taken by the scan
    static std::vector<HandDrop> g_handDrops;         // scan thread from here down
    static std::unordered_set<uint32_t> g_droppedEid;
    static std::unordered_map<uint32_t, uint16_t> g_droppedIid;   // instance id -> item row
    static std::unordered_map<uint32_t, uint32_t> g_itemFirstPass; // eid -> the scan pass that first filled it
    static volatile LONG g_handPass = 0;               // the last pass whose sightings are all recorded
    static volatile LONG g_handScanAt = 0;             // and when it ran; both read on the server thread

    struct ArmRec { DWORD at; int fails; bool judged; int mode; int ctxKind; int rounds; DWORD restUntil; };
    // How to arm, in the order worth trying. Combo 0 names one key, the one
    // the game was last seen using; combo 1 passes a zeroed name and takes
    // the routine's broad path, where it walks the node's own list of keys
    // instead. Combo 2 used to differ only in a fourth argument that the
    // disassembly shows is never read, so it was combo 0 under another name
    // and is gone. Broad first: one captured key fits a tree and has never
    // once been seen to fit an ore vein.
    static constexpr int kArmCombo[] = { 1, 0, 1, 0 };
    static constexpr int kArmPerRound = static_cast<int>(sizeof kArmCombo / sizeof kArmCombo[0]);
    // A node that answers nothing is usually furniture. It can also be one
    // that was simply slow, or that changed after the mod stopped asking, so
    // a node rests and gets another round rather than being decided for the
    // whole session on the strength of the first few seconds.
    static constexpr int   kArmRounds = 2;
    static constexpr DWORD kArmRestMs = 30000;
    // How long an arm gets before it counts as unanswered. This cannot be one
    // number: a bush answers in a fifth of a second and an ore vein takes
    // seconds. Measured across two machines on 2026-09-06, with nothing broken
    // by hand first:
    //
    //   bushes, trees   203  203  204  328  391 ms
    //   ore veins      2438 2906 3625 4312 4454 ms, one at 14953
    //
    // At the old two seconds every vein was written down as unanswered on every
    // arm, including the ones that were answered, so it spent its whole budget
    // on attempts that had not been given time. The prefab table names the kind
    // before the first arm, so ore is judged on its own clock.
    static constexpr DWORD kArmJudgeMs    = 4000;
    static constexpr DWORD kArmJudgeOreMs = 9000;
    static constexpr DWORD kArmRetryPadMs = 1000;   // retry after judging, not on top of it
    // How far out ore is armed, against the eight metres everything else
    // gets. The game's answer takes seconds for a vein, so asking at eight
    // metres means it opens just as the player arrives, and not at all if
    // they are running. Asking at twenty-five spends that wait on the walk
    // in. The scan range still caps it: an unseen node cannot be armed.
    // As far as the scan can see. Now that arming actually lands, the only
    // thing that matters for ore is how much head start it gets: a vein takes
    // 3.5 to 9.4 seconds to answer, against a fifth of a second for a tree,
    // so it has to be asked long before the player arrives or it opens under
    // their feet and the walk was wasted.
    static constexpr float kArmRangeOre = 40.0f;
    // What a prefab has actually done when armed, so effort follows evidence
    // rather than the kind written in the table. Keyed on the NodeDb row,
    // which is one object per prefab for the life of the session.
    struct PrefabArm { int exhausted; int filled; bool announced; };
    static std::unordered_map<const NodeType*, PrefabArm> g_prefabArm;
    // How many separate nodes of one prefab must use up every round, with no
    // node of that prefab ever filling, before it stops earning the long ore
    // reach. Four rather than three: copper had three nodes fail before its
    // first success, and demoting copper would be wrong.
    static constexpr int kUnresponsiveAfter = 4;
    static bool Unresponsive(const NodeType* t)
    {
        if (!t) return false;
        auto it = g_prefabArm.find(t);
        return it != g_prefabArm.end() && it->second.filled == 0 &&
               it->second.exhausted >= kUnresponsiveAfter;
    }
    static std::unordered_map<uint64_t, ArmRec> g_armed;    // nodes we asked the game to fill
    static std::unordered_map<uint32_t, const char*> g_why; // last logged verdict per entity
    static int g_whyLines = 0;
    // See the budget at the [why] emitter. The ceiling is a runaway guard for a
    // session that never ends, not a working limit: at 120 a minute it is out of
    // reach of any ordinary evening.
    static constexpr int kWhyPerMinute = 120;
    static constexpr int kWhyCeiling   = 60000;
    static std::unordered_map<uint32_t, DWORD> g_firstSeen; // eid -> when first listed
    static std::unordered_set<uint32_t>        g_containers;
    struct Spot { Vec3 p; uint16_t tid; DWORD when; uint32_t eid; };
    static std::vector<Spot> g_spots;                       // recently sent, by place and type
    static std::unordered_set<uint32_t> g_present;          // eids seen in the current scan
    struct Seen { uintptr_t ent; Vec3 pos; DWORD when; };
    static std::unordered_map<uint32_t, Seen>  g_seen;      // merges the game's partial lists

    static bool WasSeen(uint32_t eid) { return g_seen.count(eid) != 0; }

    // ------------------------------------------------- duplicate probe ----
    // Issue #73. Looting a piece of worn gear used to be refused outright on
    // the grounds that the game hands over a copy and leaves the original
    // equipped, which is what mrbryan23 saw on 1.6.12. The evidence for that
    // was a player counting items in a 240-slot bag, and so was the evidence
    // against it on 15 September. This asks the bag directly.
    //
    // Three seconds is long enough for the game to have run the pick-up and
    // short enough that the player is usually still standing there, which
    // matters because the "is the world object still there" half is answered
    // out of g_seen, and g_seen only knows about things inside scan range.
    // Walking away is reported as not knowing rather than as a clean result.
    struct DupeWatch
    {
        uint32_t  eid = 0, iid = 0;
        uint16_t  tid = 0;
        Vec3      pos{};
        DWORD     at = 0;
        int       entriesBefore = 0;
        int       walkedBefore = -1;   // slots the walk could read, -1 when it could not run
        long long stackBefore = 0;
        char      label[64] = "";
        char      node[192] = "";
    };
    static std::vector<DupeWatch> g_dupeWatch;

    // What the bag holds of one type, and whether one instance id is among
    // everything it holds. A fresh walk of every bucket, so it runs twice per
    // loot and never per scan.
    // The holder is passed in rather than fetched. The first version called
    // game::LocalPlayer(), which is not what the rest of the engine reads an
    // inventory through: all six other calls pass g_me, the actor the scan is
    // centred on, and as Damiane that is the played body while LocalPlayer is
    // the identity. The walk came back empty and every line of the run reported
    // it as "nothing reached the bag".
    //
    // Hence the slot count coming back out. A walk that saw nothing has to be
    // able to say so, or the next broken reading becomes a finding again.
    static int BagHolding(uintptr_t me, uint16_t tid, uint32_t iid, int* entries, long long* stack, bool* hasIid)
    {
        *entries = 0; *stack = 0; *hasIid = false;
        if (!me) return -1;
        static game::InvEntry buf[4096];
        const int n = game::InventoryEntries(me, buf, 4096);
        for (int i = 0; i < n; ++i)
        {
            if (iid && buf[i].iid == iid) *hasIid = true;
            if (buf[i].tid != tid) continue;
            ++*entries;
            *stack += buf[i].count;
        }
        return n;
    }

    static void DupeOpen(uintptr_t me, uint32_t eid, uint32_t iid, uint16_t tid, const Vec3& pos,
                         const char* label, const char* node, DWORD now)
    {
        if (!tid || g_dupeWatch.size() >= 32) return;
        DupeWatch w;
        w.eid = eid; w.iid = iid; w.tid = tid; w.pos = pos; w.at = now;
        bool has = false;
        w.walkedBefore = BagHolding(me, tid, 0, &w.entriesBefore, &w.stackBefore, &has);
        snprintf(w.label, sizeof w.label, "%s", label ? label : "");
        snprintf(w.node, sizeof w.node, "%s", node ? node : "");
        g_dupeWatch.push_back(w);
    }

    static void DupeMature(uintptr_t me, DWORD now, const Vec3& centre, float scanRange)
    {
        for (size_t i = 0; i < g_dupeWatch.size();)
        {
            DupeWatch& w = g_dupeWatch[i];
            if (now - w.at < 3000) { ++i; continue; }

            int entries = 0; long long stack = 0; bool hasIid = false;
            const int walked = BagHolding(me, w.tid, w.iid, &entries, &stack, &hasIid);

            const float dx = w.pos.x - centre.x, dy = w.pos.y - centre.y, dz = w.pos.z - centre.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool watching = d <= scanRange;
            auto it = g_seen.find(w.eid);
            const bool stillThere = it != g_seen.end() && it->second.when > w.at + 500;
            const bool gained = stack > w.stackBefore;

            const char* verdict;
            if (walked <= 0 || w.walkedBefore <= 0)
                                  verdict = "the bag could not be read at either end, so this says nothing about the game";
            else if (!watching)   verdict = "moved out of range before the answer was due, so this one says nothing";
            else if (gained && stillThere) verdict = "A COPY: the bag gained one and the world kept the original";
            else if (gained)      verdict = "moved, not copied";
            else if (stillThere)  verdict = "nothing reached the bag and the object is still there";
            else                  verdict = "the object is gone and nothing reached the bag, so it went somewhere else";

            LOG("[dupe] %s type %u eid %08X iid %08X: bag %d -> %d entries, %lld -> %lld held "
                "(walked %d -> %d slots); world object %s; its instance id %s in the bag; "
                "%.1f m from the scan centre. %s%s%s",
                w.label[0] ? w.label : "something unnamed", w.tid, w.eid, w.iid,
                w.entriesBefore, entries, w.stackBefore, stack, w.walkedBefore, walked,
                stillThere ? "still there" : (watching ? "gone" : "out of range"),
                hasIid ? "is" : "is not", d, verdict,
                w.node[0] ? " | " : "", w.node[0] ? w.node : "");

            g_dupeWatch.erase(g_dupeWatch.begin() + static_cast<long>(i));
        }
    }
    static const char* LastVerdict(uint32_t eid)
    {
        auto it = g_why.find(eid);
        return it == g_why.end() ? nullptr : it->second;
    }
    static std::unordered_map<uint32_t, uint16_t> g_nodeType; // eid -> gather node type, from the last scans
    static std::unordered_map<uintptr_t, uint32_t> g_actorEid; // node actor -> eid, from the last scans
    static std::unordered_map<uint32_t, DWORD> g_slotProbeAt;  // eid -> last gimmick slot dump

    // Atomic because the game thread reads it too, to ask the game which bag
    // this actor uses.
    static std::atomic<uintptr_t> g_me{ 0 };
    // Declared here, above the body helpers that read it.
    static uint32_t  g_meEid = 0, g_meRoute = 0;
    // When the scan last found nothing at all around the chosen actor.
    static DWORD g_barrenSince = 0;
    // The last world change, kept for the crash handler. Issue #59: lsimo's
    // game dies on teleporting and on going to bed, and the question the
    // faulting address cannot answer is whether this mod had noticed the world
    // change at all. A teleport driven by another mod need not raise anything
    // the block below watches for, and then the purge that drops every stale
    // component pointer never runs. Plain scalars on purpose, read without a
    // lock from an exception handler.
    static volatile DWORD g_changeAt = 0;
    static char           g_changeWhy[64] = "";
    static volatile float g_changeJumpM = 0.0f;

    // How fast the scan centre moved between the last two scans, metres a
    // second. A teleport reads as a huge number for one scan, which only means
    // one scan's worth of trees wait.
    static volatile float g_moveSpeed = 0.0f;
    // When the player actor itself last took a walking step (see Walked).
    // As Kliff the actor is the body and walks with the player. As Damiane
    // or Oongka it sits wherever the game put it, which is not one fixed
    // spot: 0,1000,0 in one session, -485,970,100 and 945,686,87 in the
    // next, sometimes in a crowd, and it jumps between them on a load or a
    // swap. It never walks.
    static DWORD g_actorMovedAt = 0;

    // --- the body -------------------------------------------------------------
    //
    // Playing as Damiane the player identity and the player's body are two
    // different entities. The identity is the player-tagged actor A0100001:
    // the game raises its own events under that id whatever character is on
    // screen, so it is the right thing to send loot events as. The body is
    // whatever the identity is currently wearing, and that is a world-tagged
    // actor, B0100005 in the capture, standing 1750 m from the identity with
    // twenty items hanging off it. The other autoloot mod logs this pair as
    // "puppet body" and "playerEid" and then gives up on it.
    //
    // A body is found by its gear, because a dressed character is the one
    // thing in the world with a handful of items parented to it. It cannot be
    // found from one enumeration: the manager hands the world over a few
    // entities at a time, so a single pass rarely holds a body and its gear
    // together, and every rule that decided from one pass picked wrong. This
    // table is fed every scan and remembers holders for fifteen seconds, which
    // is the same "partial lists, keep what was seen" treatment the scan has
    // always given world objects.
    //
    // Counting children raw is not enough on its own. A wagon carries about ten
    // attached parts and a horse carries tack, so both clear the bar and either
    // can out-count a body on the pass that happens to decide. What separates
    // them is what the children are: a dressed character carries items, a cart
    // carries cart. Children the scan has already classified as items are
    // counted separately and rank ahead of raw children. That count only fills
    // for a holder close enough for its children to be classified, which is
    // exactly the case where the confusion arises. A body 1750 m away has no
    // classified children and is still chosen on raw count, as before.
    struct Holder
    {
        uint32_t eid;
        int   kids, tickKids;   // children of any sort, best single tick
        int   gear, tickGear;   // children that classified as items, or carry the worn byte
        int   tickWorn;         // worn children seen in this enumeration tick
        DWORD seen, tick, gearTick, wornTick;
        // The entity itself has been enumerated wearing the pair of bytes only
        // the played body has shown: type tag 04 with status category 0E. Two
        // Damiane logs from two machines, against 03/0A for people and 03/0C
        // for beasts, so it is a preference and not a rule until a third says.
        bool  played;
        Vec3  pos;
        uint32_t route;
        // Where it last stood still and when it last left that spot. A body
        // being played moves; a body left behind by a character swap does not.
        Vec3  lastPos;
        DWORD movedAt;
        // The pair its own entity showed on the latest pass, where `played`
        // above only ever turns on. 0 until it is seen.
        uint8_t tagNow, catNow;
        int Score() const { return gear * 8 + kids; }
    };
    // Walking is half a metre to fifteen metres between two sightings, which
    // covers a run and a horse. A jump beyond that is a load, a swap or a
    // fast travel and says nothing about who is being played: it moves the
    // anchor and nothing else. movedAt is the last walking step.
    static bool Walked(Vec3& anchor, bool& anchored, const Vec3& at)
    {
        if (!anchored) { anchor = at; anchored = true; return false; }
        const float dx = at.x - anchor.x, dy = at.y - anchor.y, dz = at.z - anchor.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= 0.25f) return false;
        anchor = at;
        return d2 <= 225.0f;
    }
    static void NoteHolderPos(Holder& h, const Vec3& at, DWORD now)
    {
        bool anchored = h.movedAt != 0 || h.lastPos.x != 0 || h.lastPos.y != 0 || h.lastPos.z != 0;
        if (Walked(h.lastPos, anchored, at)) h.movedAt = now;
    }
    static Holder g_holders[32];
    static int    g_holderN = 0;
    static uint32_t g_bodyEid = 0;       // the holder the scan is centred on; 0 means the player actor itself
    static constexpr DWORD kHolderFreshMs = 15000;
    // A challenger has to hold its lead this long before the centre moves. One
    // pass that enumerated the wagon and not the player used to be enough, and
    // the pass after it moved the centre back.
    static constexpr DWORD kBodyHoldMs = 2500;
    static uint32_t s_challenger = 0;
    static DWORD    s_challengeSince = 0;
    static bool     s_challengeSaid = false;   // the takeover line, once per challenger
    static uint32_t s_missingSaid = 0;         // the body the absence line was last written for

    // Gear first, score second. A child that classified as an item is the only
    // evidence that a holder is a person, and no count of parts equals one of
    // them. Score alone put a war ship at 40 and the player identity at 63 in
    // one reporter's log, and the ship still held the centre for four minutes
    // and forty seconds, so whatever let it through was not the arithmetic.
    // This is asked wherever a winner is picked, not only when an incumbent is
    // being defended.
    static bool Beats(const Holder& a, const Holder& b)
    {
        if (a.played != b.played) return a.played;
        if ((a.gear > 0) != (b.gear > 0)) return a.gear > 0;
        return a.Score() > b.Score();
    }
    // The current scan's timestamp, so the classify pass stamps its gear counts
    // with the same tick the enumeration used. Two calls to GetTickCount a few
    // milliseconds apart would read as two ticks and reset the per-tick count.
    static DWORD g_scanNow = 0;

    // A child's position is its parent's: gear is attached. So one item is
    // enough to place the body, and the body itself never has to be
    // enumerated for the scan to stand where it stands.
    static void NoteHolder(uint32_t parent, const Vec3& at, uint32_t route, DWORD now)
    {
        Holder* h = nullptr;
        for (int i = 0; i < g_holderN; ++i) if (g_holders[i].eid == parent) { h = &g_holders[i]; break; }
        if (!h)
        {
            int slot;
            if (g_holderN < 32) slot = g_holderN++;
            else
            {
                // Full, so something has to go, and it must not be whatever the
                // scan is currently standing on.
                //
                // An evicted incumbent is not an incumbent. BestHolder looks the
                // body up by eid, finds nothing, and takes the "nothing held
                // yet" branch, which returns the highest score outright with no
                // margin and no 2.5 s hold. Every protection against a cart
                // stealing the centre is bypassed by the cart quietly pushing
                // the player out of a 32 slot table first, and a steam engine
                // has more than enough distinct parents nearby to do it. That
                // is Cmz4455's log: the scan left A0100001, which was carrying
                // seven items, for a train carrying none. Issue #36.
                //
                // The old line had a second problem. It claimed a fresh slot
                // with g_holderN++ and then, because that made the count 32,
                // immediately searched for the stalest and overwrote a
                // different one, abandoning the slot it had just taken.
                slot = -1;
                for (int i = 0; i < 32; ++i)
                {
                    const uint32_t e = g_holders[i].eid;
                    if (e && (e == g_bodyEid || e == g_meEid || g_holders[i].played)) continue;
                    if (slot < 0 || g_holders[i].seen < g_holders[slot].seen) slot = i;
                }
                if (slot < 0) return;   // nothing evictable: keep what we have
            }
            Holder nh{};
            nh.eid = parent; nh.seen = now; nh.tick = now; nh.pos = at; nh.route = route;
            g_holders[slot] = nh;
            h = &g_holders[slot];
        }
        if (h->tick != now) { h->tick = now; h->tickKids = 0; }
        ++h->tickKids;
        if (h->tickKids > h->kids) h->kids = h->tickKids;
        h->seen = now; h->pos = at; if (route) h->route = route;
        NoteHolderPos(*h, at, now);
    }

    // A child that turned out to be an item. Called from the classify pass, so
    // it costs nothing beyond work already done, and it only ever speaks for a
    // holder near enough to have had its children classified. Wagon parts and
    // horse tack are attached but they are not items, so a cart earns nothing
    // here however many pieces it is built from.
    // Swapping character invalidates every holder: the old body's gear is
    // still on record and would win the next pick outright.
    static void ForgetBody(const char* why)
    {
        if (g_bodyEid) LOG("[player] forgetting the body %08X: %s", g_bodyEid, why);
        g_bodyEid = 0; g_holderN = 0; s_challenger = 0;
        for (Holder& h : g_holders) h = {};
    }

    static void NoteHolderGear(uint32_t parent, DWORD now)
    {
        for (int i = 0; i < g_holderN; ++i)
        {
            Holder& h = g_holders[i];
            if (h.eid != parent) continue;
            if (h.gearTick != now) { h.gearTick = now; h.tickGear = 0; }
            ++h.tickGear;
            if (h.tickGear > h.gear) h.gear = h.tickGear;
            return;
        }
    }

    // A worn item says so in its own status byte, and it says so from any
    // distance. NoteHolderGear runs in the classify pass and so only ever
    // speaks for a holder within scan range, which was a catch-22 as Damiane:
    // her gear is on a body far from the identity actor the scan starts on,
    // so it never classified, so the body never scored above its raw child
    // count, so a ship with forty-five parts won every pick and a whole
    // session looted nothing. This counts the same gear from the enumeration
    // instead. Same high-water mark, fed by whichever per-tick count is
    // larger, so a child seen by both paths is not counted twice.
    // The body itself is handed over every tick. Its gear is handed over only
    // when the game thinks something about it changed, and standing still
    // beside a ship that was once in four minutes: the enumeration ran at one
    // object a scan, the body a metre from the centre, and after fifteen
    // seconds without a child the body aged out of its own table, twice in one
    // session. Seeing the holder is as good as seeing one of its children.
    static void TouchHolder(uint32_t eid, uintptr_t e, uint8_t tag, const Vec3& at, DWORD now, bool played)
    {
        for (int i = 0; i < g_holderN; ++i)
            if (g_holders[i].eid == eid)
            {
                g_holders[i].seen = now; g_holders[i].pos = at;
                if (played) g_holders[i].played = true;
                g_holders[i].tagNow = tag;
                g_holders[i].catNow = game::Cat2(e);
                NoteHolderPos(g_holders[i], at, now);
                return;
            }
    }

    // Is the thing the scan is centred on a body the game has shown us being
    // played? Type tag 04 with status category 0E, the pair issue #36 settled.
    static bool PlayedHolder(uint32_t eid)
    {
        if (!eid) return false;
        for (int i = 0; i < g_holderN; ++i) if (g_holders[i].eid == eid) return g_holders[i].played;
        return false;
    }

    // Player-tagged actors seen lately, merged across scans. The actor manager
    // hands the world over a few entities at a time, so "this pass saw one
    // player-tagged actor" says nothing about how many there are; five separate
    // rules for picking the player have failed on exactly that. Twelve seconds,
    // to match g_seen, and cleared on a world change with it.
    static std::unordered_map<uint32_t, DWORD> g_actorsSeen;
    static void NoteActor(uint32_t eid, DWORD now) { g_actorsSeen[eid] = now; }
    static int ActorsSeen(DWORD now)
    {
        for (auto it = g_actorsSeen.begin(); it != g_actorsSeen.end();)
            it = (now - it->second > 12000) ? g_actorsSeen.erase(it) : std::next(it);
        return static_cast<int>(g_actorsSeen.size());
    }

    static void NoteHolderWorn(uint32_t parent, DWORD now)
    {
        for (int i = 0; i < g_holderN; ++i)
        {
            Holder& h = g_holders[i];
            if (h.eid != parent) continue;
            if (h.wornTick != now) { h.wornTick = now; h.tickWorn = 0; }
            ++h.tickWorn;
            if (h.tickWorn > h.gear) h.gear = h.tickWorn;
            return;
        }
    }

    // The best body on record: fresh, carrying at least three things, on the
    // player's own route when that is known, and carrying the most, with one
    // classified item worth eight of anything else.
    //
    // Whatever is already chosen keeps the centre while it stays fresh and
    // still qualifies. A challenger has to beat it by a clear margin and hold
    // that lead for kBodyHoldMs before the scan moves. The manager hands the
    // world over a few entities at a time, so any single pass is a poor
    // witness: the pass that sees the wagon and not the player reports that
    // the wagon is the only thing carrying anything. Sustained and momentary
    // are different claims and only the first is worth moving for.
    static const Holder* BestHolder(DWORD now, uint32_t playerRoute)
    {
        const Holder* best = nullptr;
        const Holder* incumbent = nullptr;
        for (int i = 0; i < g_holderN; ++i)
        {
            const Holder& h = g_holders[i];
            if (now - h.seen > kHolderFreshMs || h.kids < 3) continue;
            if (playerRoute && h.route && h.route != playerRoute) continue;
            if (g_bodyEid && h.eid == g_bodyEid) incumbent = &h;
            if (!best || Beats(h, *best)) best = &h;
        }
        // Nothing held yet, or what was held has gone stale: take the best.
        if (!incumbent)
        {
            // Issue #36. This branch has no margin, no hold and no gear test,
            // so if a body was held a moment ago and is not here now, this is
            // where a cargo ship takes the centre outright. Nothing in any
            // log has ever said which filter dropped the body, so say it,
            // once per body, in the terms the filters above use.
            if (g_bodyEid && s_missingSaid != g_bodyEid)
            {
                s_missingSaid = g_bodyEid;
                const Holder* h = nullptr;
                for (int i = 0; i < g_holderN; ++i) if (g_holders[i].eid == g_bodyEid) { h = &g_holders[i]; break; }
                if (!h)
                    LOG("[player] the body %08X is not in the holder table at all (%d of 32 slots used); choosing afresh", g_bodyEid, g_holderN);
                else
                    LOG("[player] the body %08X is in the table but filtered out: seen %lu ms ago (limit %lu), %d children, %d of them equipment, route %08X against the player's %08X; choosing afresh",
                        g_bodyEid, static_cast<unsigned long>(now - h->seen), static_cast<unsigned long>(kHolderFreshMs),
                        h->kids, h->gear, h->route, playerRoute);
            }
            s_challenger = 0; return best;
        }
        s_missingSaid = 0;
        // Two played bodies on record is a character swap between Damiane and
        // Oongka: both wear the pair of bytes, so neither rule below can tell
        // them apart, and the parked one keeps the centre on score for the
        // rest of the session. The one being played moves. A played body that
        // left its spot in the last three seconds takes the centre from one
        // that has stood still for ten, after the usual hold.
        const Holder* mover = nullptr;
        if (incumbent->played && incumbent->movedAt && now - incumbent->movedAt > 10000)
            for (int i = 0; i < g_holderN; ++i)
            {
                const Holder& h = g_holders[i];
                if (&h == incumbent || !h.played || !h.movedAt || now - h.movedAt > 3000) continue;
                if (now - h.seen > kHolderFreshMs || h.kids < 3) continue;
                if (playerRoute && h.route && h.route != playerRoute) continue;
                if (!mover || h.movedAt > mover->movedAt) mover = &h;
            }
        if (mover) best = mover;
        const bool movingOverParked = mover != nullptr;
        if (!best || best->eid == incumbent->eid) { s_challenger = 0; return incumbent; }

        // Parts are not gear. Something that has never had a single child
        // classify as an item does not take the centre from something that
        // has, however many pieces it is built from. A steam engine enumerates
        // thirty-two parts and a dressed character seven items, and the score
        // already says the character wins; this is the backstop for when it
        // does not, because a big enough object can out-count seven items on
        // raw children alone.
        if (best->gear == 0 && incumbent->gear > 0 && !movingOverParked) { s_challenger = 0; return incumbent; }
        // The played body is not displaced by anything that is not one, and a
        // pack cow with eleven items of cargo classified is the case in hand:
        // score 101 against the body's 36, and it took the centre.
        if (incumbent->played && !best->played && !movingOverParked) { s_challenger = 0; return incumbent; }
        const bool playedOverNot = best->played && !incumbent->played;
        // And its mirror. Something with gear takes the centre from something
        // without, whatever the counts say, after the same hold. In the same
        // log a prison wagon kept the centre for eighty seconds against the
        // real body because the body's score did not clear the margin.
        const bool gearOverParts = best->gear > 0 && incumbent->gear == 0;

        // Half again as much, and two more outright, or it is noise.
        const int mine = incumbent->Score(), theirs = best->Score();
        if (!gearOverParts && !playedOverNot && !movingOverParked && (theirs < mine + 2 || theirs * 2 < mine * 3)) { s_challenger = 0; return incumbent; }

        if (s_challenger != best->eid) { s_challenger = best->eid; s_challengeSince = now; s_challengeSaid = false; }
        if (now - s_challengeSince < kBodyHoldMs) return incumbent;
        // Both sides of the decision, once. The caller's line names the winner
        // and where it stands; this one says what it beat and by how much.
        if (!s_challengeSaid)
        {
            s_challengeSaid = true;
            LOG("[player] %08X (%d children, %d equipment, score %d) takes the centre from %08X (%d children, %d equipment, score %d)%s",
                best->eid, best->kids, best->gear, theirs, incumbent->eid, incumbent->kids, incumbent->gear, mine,
                movingOverParked ? ", as the played body that moves while the other stands" : playedOverNot ? ", as the played body" : gearOverParts ? ", on gear alone" : "");
        }
        return best;
    }

    // Worn or carried by the player, whichever entity that means right now.
    static bool IsMine(uint32_t parent)
    {
        return parent && (parent == g_meEid || (g_bodyEid && parent == g_bodyEid));
    }

    // --- what a gather node yields --------------------------------------------
    // A node is named by the prefab it was placed from (see NodeDb). For the
    // few that table does not cover, the bag says what it gave: after a gather,
    // whichever item count rose is what that node type yielded.
    //
    // That part is per session only. The type number a node reports is not
    // stable between runs: 50869 was Shrubby Sophora on one and a sweet potato
    // socket on the next, so a remembered pair goes quietly wrong and files a
    // node under the wrong switch. Nothing is written to disk, and a file left
    // by an older build is deleted on load.
    // Whether the scan ever had an entity in hand, and the last verdict it gave
    // it. Defined below; used by the diagnostic for what the player took by hand.
    static bool WasSeen(uint32_t eid);
    static const char* LastVerdict(uint32_t eid);

    // A node's gather type id is not a kind of node. symplexity's log of 14
    // September 2026 has type 50875 gathered as Timber fourteen times, as
    // Firewood six times and as a Mine rock once, and type 50754 covers
    // hickory, oak, cypress and an apple crop. So a bag diff that teaches this
    // map anything teaches it about every other kind sharing the id: gathering
    // firewood at 15:44 taught it that 50875 pays Timber, and from that minute
    // every mine rock in the world read as wood, which symplexity allows, and
    // the rocks he had refused by class came in for the next hour. Issue #72.
    //
    // So the prefab that taught it is kept beside the answer and has to match
    // before the answer is used. The prefab is the identity; the type id is not,
    // and on its own it is worse than knowing nothing.
    struct Learned { uint16_t row = 0; std::string node; };
    static std::unordered_map<uint16_t, Learned> g_learn;   // node type -> what one prefab of that type paid
    static std::unordered_map<uint32_t, std::string> g_nodePrefab;  // eid -> prefab, beside g_nodeType
    // mine: sent by this mod. The rest came off the player's own hands, and
    // only what this mod picked up is ever offered to storage. yieldRow: what a
    // gather of this mod's is known to pay, -1 when nothing names it.
    struct PendSend { DWORD at; Action act; uint16_t nodeType; int itemRow; bool counted = false; std::string node; bool mine = false; int yieldRow = -1; };
    static std::vector<PendSend> g_pend;
    // The last time anything was done by hand. Kept apart from g_pend because a
    // hand gather of a node whose yield is already learned never goes into
    // g_pend at all, and auto-store has to know about it all the same.
    static DWORD g_lastHandAt = 0;
    // Auto-store's evidence. An item counts as this mod's pick-up when the
    // object the mod sent to has gone from the world and a rise of that item
    // follows. A matching entry on its own is not enough, and /vet showed it
    // twice: an object the mod reached for and never got stays in the world,
    // and its entry claimed the player's own hand pick-up, purchase or craft of
    // the same item. So a rise waits in g_heldRises until the scan has seen
    // whether the target is still lying there.
    //
    // The game can hold a run of pick-ups and hand them over together: on 18
    // September 2026 a camp clear reached nothing for eight seconds and then
    // landed all at once. The objects stay in the world for all of that, so
    // an entry keeps no clock while its object is there. Once the object is
    // gone the entry has five seconds to be matched. One entry per world
    // object, and the oldest go first past 512.
    //
    // Whether it went because it was delivered or because the player walked
    // off is judged where it vanished: the object's own position, kept here
    // from the send because the scan forgets an entity that stops reading back,
    // against the scan centre in the first scan that missed it. missAt and
    // missDist hold that first miss until a second and a half says it is real.
    struct OwnSend { uint16_t row; uint32_t eid; DWORD sentAt; DWORD goneAt; Vec3 pos; DWORD missAt; float missDist; };
    static std::vector<OwnSend> g_ownSends;
    static constexpr DWORD kOwnGoneMs = 5000;
    // A rise of one item, seen by LearnFromInventory and judged by
    // AutoStorePass. fallback: one of this mod's nameless sends could have paid
    // it and nothing was done by hand, judged while those sends were pending.
    // handUnnamed: something taken by hand in the last twelve seconds is an
    // item nothing here can name, so it could have paid this rise, and no
    // credit of any kind is given to it.
    // inPlay: the player was in free play when the rise was first seen. A rise
    // seen in a shop, a crafting screen, a menu or a storage is a purchase, a
    // craft or something taken out on purpose, and is never offered. credited:
    // units already judged to be this mod's, waiting for Private Storage
    // Master to take them, which it refuses outside free play; they are
    // offered again until it does, for twenty seconds.
    struct HeldRise { uint16_t row; long long units; DWORD at; bool fallback; bool inPlay; bool handUnnamed = false; long long credited = 0; };
    static std::vector<HeldRise> g_heldRises;
    static constexpr DWORD kRiseHoldMs = 4000;
    static constexpr DWORD kRiseRetryMs = 20000;
    // The last time Private Storage Master said the player was out of free play.
    // It is asked every tenth of a second, between scans as well, so a storage
    // opened and closed inside one scan interval is still seen. A rise read
    // within FreeGraceMs of it is treated as seen outside free play: the rise
    // can have happened just before the close, and the bag is read up to half
    // a second late and then only at the next scan.
    static DWORD g_notFreeAt = 0;
    static void SampleFreePlay(DWORD now)
    {
        if (psm::FreePlay() == 0) g_notFreeAt = now ? now : 1;
    }
    static DWORD FreeGraceMs()
    {
        return 1600 + static_cast<DWORD>(1000 / std::clamp(Settings::Get().scansPerSec, 1, 30));
    }
    // A gather of this mod's whose yield is known, kept for twelve seconds
    // because the game can hold a delivery for eight. It vouches for a rise of
    // that item alone. The pending sends expire at four, which is too soon.
    struct MineYield { DWORD at; uint16_t row; };
    static std::vector<MineYield> g_mineYields;
    // What the player touched by hand, by entity. Kept for twelve seconds,
    // because the game can hold a delivery for eight and a hand pick-up
    // delivered late must still read as the player's.
    struct HandTouch { uint32_t eid; DWORD at; };
    static constexpr DWORD kHandMs = 12000;
    // How long a companion's body search vouches for what arrives after it.
    static constexpr DWORD kPetStoreMs = 8000;
    static std::vector<HandTouch> g_handTouches;
    static std::vector<std::pair<uint16_t, long long>> g_invPrev;
    static bool g_invPrevValid = false;
    // The carried bag alone, for auto-store. The sample above spans every
    // storage, and Private Storage Master moving a stack out of the bag can be
    // read half done, in both places at once, which is a rise nobody picked up.
    static std::vector<std::pair<uint16_t, long long>> g_bagPrev;
    static bool g_bagPrevValid = false;
    // What Private Storage Master takes out of the bag, counted back in.
    //
    // A rise used to be the bag now minus the bag one sample ago, so whatever
    // was stored in between cancelled as many fresh arrivals. Fyreon87's
    // Peony gathers of 24 September 2026 landed about 27 plants while this
    // mod reported 16, and the other 11 stayed in the bag with no error
    // anywhere, since Private Storage Master moves exactly what it is told.
    // Its log shows the same for twelve items that session.
    //
    // So while a deposit of an item is out, the item keeps a tally: every
    // change the bag shows, signed, plus every unit a result says was stored,
    // and whatever that comes to beyond what was already reported is the rise.
    // That is right whichever shows first, the bag falling or the result. The
    // tally is per item because deposits of one item merge into one job and
    // one result. It clears once the item has had a result and nothing for a
    // few seconds, after two minutes regardless, and with the baseline. A
    // result with no tally open is ignored, and no tally opens for a while
    // after a world change, because a result that lands in a tally opened
    // after its move would report stock the player already carried, and
    // Private Storage Master would store it. Every way this can be off leaves
    // things in the bag rather than taking more than arrived.
    struct StoreTally { long long delta = 0, moved = 0, credited = 0; DWORD opened = 0, lastAt = 0; bool resulted = false; };
    static std::unordered_map<uint16_t, StoreTally> g_storeTally;
    static constexpr DWORD kTallyQuietMs = 8000;
    static constexpr DWORD kTallyMaxMs = 120000;
    static constexpr DWORD kTallyAfterChangeMs = 10000;
    static long long BagCount(const std::vector<std::pair<uint16_t, long long>>& v, uint16_t row)
    {
        const auto it = std::lower_bound(v.begin(), v.end(), row,
                                         [](const std::pair<uint16_t, long long>& e, uint16_t r) { return e.first < r; });
        return it != v.end() && it->first == row ? it->second : 0;
    }
    // Gather nodes whose yield is known, by entity, so a node gathered by hand
    // names what it paid the way an item's own row does.
    static std::unordered_map<uint32_t, uint16_t> g_yieldByEid;
    static std::unordered_map<uint32_t, uint16_t> g_tidByEid;   // items the scan has read, by entity

    // How full the bag is is read straight from it: the carried bag keeps its
    // used count and its limit side by side. When those do not read sanely the
    // behaviour check below stands in, which infers a full bag from pick-ups
    // that reach nothing. One failure means little (an item can be snatched, an
    // event refused), three in a row mean the bag has no room, and one landing
    // clears it. An item that is going to arrive does so in well under a
    // second, so a send is judged at 1.2 s rather than waiting out the four
    // seconds the yield learning wants.
    static constexpr int   kBagFullStreak = 3;
    static constexpr DWORD kBagVerdictMs  = 1200;

    // The bag's limit was read off one bag that never grew, so expanding it has
    // never been watched. If the field turns out to be a base figure that does
    // not move, the mod would call a bigger bag full early. One pick-up landing
    // while it reads full proves the limit is higher than it says, and that is
    // enough to correct it and carry on. Cleared whenever the field itself
    // moves, since then it is telling the truth and should be believed.
    static int g_capSeen = 0, g_capBias = 0;
    static int   g_noRise = 0;
    static bool  g_bagFull = false;
    static DWORD g_bagFullSaid = 0;

    static void DropLearnedFile()
    {
        const std::wstring path = Paths::File(L"MasterLooter.learned.tsv");
        if (DeleteFileW(path.c_str()))
            LOG("[learn] removed MasterLooter.learned.tsv: a node's type number changes between sessions, so remembered yields cannot be trusted");
    }
    static const Item* LearnedYield(uint16_t nodeType, const char* node)
    {
        auto it = g_learn.find(nodeType);
        if (it == g_learn.end()) return nullptr;
        // An entry with no prefab recorded answers for nothing but itself.
        if (it->second.node.empty()) return nullptr;
        if (!node || !node[0] || it->second.node != node) return nullptr;
        return ItemDb::ByRow(it->second.row);
    }

    // What the player has just taken by hand, so neither delete path touches
    // it. Seth decided on 21 September 2026 that a pick-up of your own is
    // never deleted, after LuxDragon lost a recipe he had picked up as Damiane
    // with a mercenary out: the filter used to hand any refused pick-up of the
    // player's to the pet window while a companion was about, since the game
    // can raise a pet's pick-up as the player's own. Prevention tells a pet no
    // before it reaches for a refused loose item now, so that caution had
    // stopped paying for itself and was costing the player things he chose.
    // A row is kept for a few seconds, long enough for the sweep's two-second
    // diff to see it; a gather or catch names no row, so it marks the whole
    // bag as the player's for that long instead.
    struct HandRow { uint16_t tid; DWORD at; };
    static std::vector<HandRow> g_handRows;
    static DWORD g_handUnknownAt = 0;
    static constexpr DWORD kHandKeepMs = 5000;

    static void NoteHandTake(uint32_t eid, bool namesRow, DWORD now)
    {
        const auto it = namesRow ? g_tidByEid.find(eid) : g_tidByEid.end();
        if (it != g_tidByEid.end() && it->second)
        {
            if (g_handRows.size() >= 64) g_handRows.erase(g_handRows.begin());
            g_handRows.push_back({ it->second, now ? now : 1 });
        }
        else g_handUnknownAt = now ? now : 1;
    }

    // Whether the player took this row, or took something unnamed, at or
    // after `since`.
    static bool HandTookRow(uint16_t tid, DWORD since)
    {
        for (const HandRow& h : g_handRows)
            if (h.tid == tid && static_cast<long>(h.at - since) >= 0) return true;
        return false;
    }
    static void AgeHandRows(DWORD now)
    {
        while (!g_handRows.empty() && now - g_handRows.front().at > kHandKeepMs) g_handRows.erase(g_handRows.begin());
    }
    static bool HandTookUnknown(DWORD since)
    {
        return g_handUnknownAt && static_cast<long>(g_handUnknownAt - since) >= 0;
    }

    // Only while a pet is demonstrably out and looting. A pet's body search is
    // the one event stamped with the pet's own id, so it is proof; its pick-up
    // of a loose item is not, which is the whole problem. Without that proof a
    // hand pick-up is just a hand pick-up, and deleting it takes something the
    // player chose to carry.
    //
    // Reported on the Nexus bugs tab against 1.6.12: "Mod deletes every
    // filtered out item picked up by the main character when Pets and
    // companions follow the filters is on." Correct, and it was by design.
    // A player who picks up something their own rules refuse now keeps it
    // unless a pet has been seen looting in the last half minute.
    static DWORD g_petSeenAt = 0;
    static constexpr DWORD kPetOutMs = 30000;
    // Set when the world changes under the mod: a load, a teleport, a mount, a
    // cutscene, a character swap. The inventory that comes back is a different
    // inventory, and against a snapshot taken before it every row reads as an
    // arrival. Seth loaded a save on 11 September 2026 and the sweep deleted
    // what was already in his bag. The filter takes a fresh baseline and skips
    // one pass rather than judging anything across that line.
    static volatile LONG g_bagBaselineStale = 0;
    static void InvalidateBagBaseline() { InterlockedExchange(&g_bagBaselineStale, 1); }
    static bool TakeBagBaselineStale() { return InterlockedExchange(&g_bagBaselineStale, 0) != 0; }
    static void NotePetActivity(DWORD now) { g_petSeenAt = now ? now : 1; }

    // Is this raiser the player rather than something following them?
    //
    // g_meEid is the A0 identity, and as Kliff that is also the thing that
    // walks, so one test was enough for a year. As Damiane or Oongka the
    // identity is a fixture at 0,1000,0 and the body that picks things up is a
    // separate B0 actor, which is neither g_meEid nor player-tagged, so the
    // filter read every one of her own pick-ups as a pet's. LuxDragon lost a
    // Sealed Abyss Artifact to that on 1.6.34 with nothing out at all, issue
    // #85: his log reads "[pet] B0100002 (a pet) picked up ... Sealed Abyss
    // Artifact" two lines after naming B0100002 tag 04 byte 0E, the pair that
    // means a played body, 0.6 m from an identity standing at the placeholder.
    static bool RaisedByPlayer(uint32_t eid)
    {
        return eid && (eid == g_meEid || eid == g_bodyEid || PlayedHolder(eid));
    }
    static bool PetOutRecently(DWORD now)
    {
        if (g_petSeenAt && (now - g_petSeenAt) < kPetOutMs) return true;
        // Anything a companion raised counts, not only the pick-ups and body
        // searches the filter can hang a window on. A mercenary that breaks a
        // rock raises a drop event and nothing else, and it is still out.
        const DWORD c = events::CompanionActiveAt();
        return c && (now - c) < kPetOutMs;
    }

    static bool MercenaryOutRecently(DWORD now)
    {
        const DWORD m = events::MercenaryActiveAt();
        return m && (now - m) < kPetOutMs;
    }

    // Whether a pet is told no before it reaches for a loose item the rules
    // refuse: the condition hook is in and one of the two switches that
    // answer through it is on. Then a refused loose item that lands in the
    // bag was not the pet's, whatever event it came in on, and it is the
    // player's to keep. A mercenary is the exception until a session shows it
    // asks the same question, so while one is out the old caution holds.
    static bool PetPrevented(const Config& cfg, DWORD now)
    {
        return hooks::PetLootingHooked() && (cfg.petFilter || cfg.stopPetLooting) && !MercenaryOutRecently(now);
    }

    static bool IsFilterRule(const char* why)
    {
        return why && (strcmp(why, "item override") == 0 || strcmp(why, "tag never") == 0 || strcmp(why, "class skipped") == 0 ||
                       strcmp(why, "below value floor") == 0 || strcmp(why, "unsellable") == 0);
    }

    // Auto-store. Private Storage Master moves what this mod picked up into the
    // storage it belongs in; this side only says what arrived and reports what
    // went where. Its results are read once a second and shown as one line, so a
    // camp clear reads "Stored 40 items: ..." once and not forty times.
    static const char* DepositReason(int r)
    {
        switch (r)
        {
        case PSM_DEPOSIT_STORED:              return "stored";
        case PSM_DEPOSIT_NO_STORAGE_TAKES_IT: return "no storage takes it";
        case PSM_DEPOSIT_STORAGE_FULL:        return "every storage that takes it is full";
        case PSM_DEPOSIT_NEVER_MOVED:         return "on the never-move list";
        case PSM_DEPOSIT_LOCKED:              return "the stack is locked";
        case PSM_DEPOSIT_NOT_IN_BAG:          return "it never showed up in the bag";
        case PSM_DEPOSIT_OFF:                 return "auto-store is off";
        case PSM_DEPOSIT_BUSY:                return "too many at once";
        default:                              return "an unknown reason";
        }
    }

    static void DrainDeposits(DWORD now)
    {
        static DWORD s_last = 0;
        if (now - s_last < 1000) return;
        s_last = now;
        PsmDepositResult res[64];
        const int n = psm::DepositResults(res, 64);
        if (n <= 0) return;
        const psm::Api* api = psm::Get();
        long long perStorage[PSM_STORAGES] = {};
        long long total = 0;
        int kept = 0;
        for (int i = 0; i < n; ++i)
        {
            const PsmDepositResult& r = res[i];
            const Item* it = ItemDb::ByRow(r.item);
            {
                const auto t = g_storeTally.find(r.item);
                if (t != g_storeTally.end())
                {
                    if (r.reason == PSM_DEPOSIT_STORED && r.moved > 0) t->second.moved += r.moved;
                    t->second.resulted = true;
                    t->second.lastAt = now;
                }
            }
            if (r.reason == PSM_DEPOSIT_STORED && r.storage >= 0 && r.storage < PSM_STORAGES && r.moved > 0)
            {
                perStorage[r.storage] += r.moved;
                total += r.moved;
                if (g_debugLog)
                    LOG("[store] %lld %s went to %s", static_cast<long long>(r.moved), it && !it->name.empty() ? it->name.c_str() : "unnamed item",
                        api ? api->storageName(r.storage) : "storage");
                continue;
            }
            ++kept;
            // Stays in the bag. Worth a line when the verbose log is on, and a
            // full storage always, since that is the one a player can fix.
            static int s_fullSaid = 0;
            if (g_debugLog || (r.reason == PSM_DEPOSIT_STORAGE_FULL && s_fullSaid < 20))
            {
                if (r.reason == PSM_DEPOSIT_STORAGE_FULL) ++s_fullSaid;
                LOG("[store] %s stayed in the bag: %s", it && !it->name.empty() ? it->name.c_str() : "an unnamed item", DepositReason(r.reason));
            }
        }
        if (!total) return;
        char msg[256];
        int len = snprintf(msg, sizeof msg, "Stored %lld item%s:", total, total == 1 ? "" : "s");
        bool first = true;
        for (int s = 0; s < PSM_STORAGES && len > 0 && len < static_cast<int>(sizeof msg) - 1; ++s)
        {
            if (!perStorage[s]) continue;
            len += snprintf(msg + len, sizeof msg - len, "%s %s %lld", first ? "" : ",",
                            api ? api->storageName(s) : "storage", perStorage[s]);
            first = false;
        }
        LOG("[store] %s%s", msg, kept ? " (some stayed in the bag)" : "");
        if (Settings::Get().notifyAutoStore)
        {
            char shown[272];
            snprintf(shown, sizeof shown, "Master Looter: %s", msg);
            State::Get().Notify(shown, 3500);
        }
    }

    // Diff the bag against the last scan and attribute every rise to a pending send.
    static void LearnFromInventory(DWORD now)
    {
        DrainDeposits(now);
        // What the player gathered or caught by hand counts as a pending send
        // too, so nodes get identified without the mod ever gathering them.
        static events::Seen seen[32];
        const int sn = events::DrainSeen(seen, 32);
        if (sn > 0) g_lastHandAt = now ? now : 1;
        for (int i = 0; i < sn; ++i)
        {
            if (g_handTouches.size() < 128) g_handTouches.push_back({ seen[i].eid, now });
            NoteHandTake(seen[i].eid, seen[i].act == Action::Take, now);
            // A carcass or body searched by hand is empty now, so it joins the
            // same never-again set the mod's own searches go into. Before this
            // only the mod's searches were remembered: on 16 September 2026 Seth
            // skinned a Pigeon by hand with Carcasses off, turned the switch on,
            // and 55 seconds later the mod searched the same carcass again.
            // Searching an emptied carcass is the known way to duplicate items.
            // Only player-tagged raisers reach this queue, so a mercenary's
            // search counts too, and the mod's own sends never do.
            if (seen[i].act == Action::Search)
            {
                g_searched.insert(seen[i].eid);
                g_retiredEid.insert(seen[i].eid);
            }
            uint16_t nodeType = 0;
            std::string nodePrefab;
            if (seen[i].act == Action::Gather)
            {
                auto it = g_nodeType.find(seen[i].eid); if (it != g_nodeType.end()) nodeType = it->second;
                auto ip = g_nodePrefab.find(seen[i].eid); if (ip != g_nodePrefab.end()) nodePrefab = ip->second;
            }
            const Item* known = nullptr;
            if (seen[i].act == Action::Gather && nodeType) known = LearnedYield(nodeType, nodePrefab.c_str());
            if (known) continue; // nothing new to learn from it
            g_pend.push_back({ static_cast<DWORD>(seen[i].at), seen[i].act, nodeType, -1, false, nodePrefab });
            // Anything the player takes by hand that the mod passed over is worth
            // a line: it is the only way a missed object leaves a trace at all,
            // and it separates "never scanned" from "scanned and skipped".
            //
            // This used to stay quiet for a gather whose node type is unknown,
            // on the grounds that there was nothing to learn from it. That is
            // the one case worth hearing about. Drawing water from a well is a
            // player gather on an object with no gather block, so it left no
            // trace at all and the log could not say whether the mod had even
            // seen it. An interaction the mod does not understand is exactly
            // what a log is for.
            {
                const char* why = LastVerdict(seen[i].eid);
                static int s_missLogs = 0;
                if (why && seen[i].act == Action::Take && Settings::Get().petFilter && IsFilterRule(why))
                {
                    static int s_said = 0;
                    if (s_said < 8)
                    {
                        ++s_said;
                        LOG("[pet] eid %08X was picked up by hand and the rules refuse it; a pick-up of your own "
                            "is never deleted, so it stays in the bag.", seen[i].eid);
                    }
                }
                if (why) { if (Settings::Get().debugLog) LOG("[learn] player %s eid %08X (node type %u), we had skipped it: %s", events::ActionName(seen[i].act), seen[i].eid, nodeType, why); }
                else if (WasSeen(seen[i].eid)) { if (Settings::Get().debugLog) LOG("[learn] player %s eid %08X (node type %u), the scan had it and did not act", events::ActionName(seen[i].act), seen[i].eid, nodeType); }
                else if (s_missLogs < 40) { ++s_missLogs; LOG("[learn] player %s eid %08X (node type %u), the scan never saw it", events::ActionName(seen[i].act), seen[i].eid, nodeType); }
            }
        }
        // A different holder from the last sample is a different bag: the scan
        // moved to another actor, or the game has just said which bag this one
        // uses. Diffing across that would read everything in the new bag as
        // arriving at once, which auto-store would offer and the pet filter
        // would judge, so every baseline starts again from this sample.
        {
            static uintptr_t s_holder = 0;
            const uintptr_t hr = game::LastInventoryHolder();
            if (hr != s_holder)
            {
                s_holder = hr;
                g_invPrevValid = false;
                g_bagPrevValid = false;
                g_storeTally.clear();
                InvalidateBagBaseline();
            }
        }
        static uint16_t types[2048]; static long long qty[2048];
        const int n = game::InventoryTypes(types, qty, 2048);
        std::vector<std::pair<uint16_t, long long>> cur(n);
        for (int i = 0; i < n; ++i) cur[i] = { types[i], qty[i] };
        std::vector<uint16_t> rose;
        // Which types rose, and by how much. Four Stones knocked out of one
        // rock arrive as a single rise of four, not as four rises, so the
        // count is the only way to tell four pick-ups landing from one.
        std::unordered_map<uint16_t, long long> gained;
        if (g_invPrevValid)
        {
            size_t j = 0;
            for (const auto& e : cur)
            {
                while (j < g_invPrev.size() && g_invPrev[j].first < e.first) ++j;
                const long long before = (j < g_invPrev.size() && g_invPrev[j].first == e.first) ? g_invPrev[j].second : 0;
                if (e.second > before) { rose.push_back(e.first); gained[e.first] = e.second - before; }
            }
        }
        g_invPrev.swap(cur);
        g_invPrevValid = true;
        std::vector<std::pair<uint16_t, long long>> bagRose;   // row, units
        {
            static uint16_t btypes[1024]; static long long bqty[1024];
            const int bn = game::BagTypes(btypes, bqty, 1024);
            if (bn < 0) { g_bagPrevValid = false; g_storeTally.clear(); }
            else
            {
                std::vector<std::pair<uint16_t, long long>> bcur(bn);
                for (int i = 0; i < bn; ++i) bcur[i] = { btypes[i], bqty[i] };
                if (g_bagPrevValid)
                {
                    size_t j = 0;
                    for (const auto& e : bcur)
                    {
                        if (g_storeTally.count(e.first)) continue;
                        while (j < g_bagPrev.size() && g_bagPrev[j].first < e.first) ++j;
                        const long long before = (j < g_bagPrev.size() && g_bagPrev[j].first == e.first) ? g_bagPrev[j].second : 0;
                        if (e.second > before) bagRose.push_back({ e.first, e.second - before });
                    }
                    // A tallied row can also have left the bag entirely, so it
                    // is read from both samples rather than from this one alone.
                    for (auto it = g_storeTally.begin(); it != g_storeTally.end();)
                    {
                        StoreTally& t = it->second;
                        t.delta += BagCount(bcur, it->first) - BagCount(g_bagPrev, it->first);
                        const long long rise = t.delta + t.moved - t.credited;
                        if (rise > 0)
                        {
                            bagRose.push_back({ it->first, rise });
                            t.credited += rise;
                            if (g_debugLog)
                            {
                                const Item* named = ItemDb::ByRow(it->first);
                                LOG("[store] +%lld %s counted past what was just stored (bag %+lld, stored %lld)",
                                    rise, named && !named->name.empty() ? named->name.c_str() : "unnamed item", t.delta, t.moved);
                            }
                        }
                        const bool done = (t.resulted && now - t.lastAt > kTallyQuietMs) || now - t.opened > kTallyMaxMs;
                        it = done ? g_storeTally.erase(it) : std::next(it);
                    }
                }
                else g_storeTally.clear();
                g_bagPrev.swap(bcur);
                g_bagPrevValid = true;
            }
        }
        // Expire stale sends. A pick-up of a known item that expires with
        // nothing to show for it is what a full bag looks like from here. Only
        // those count: an empty carcass, a node that gives nothing and an item
        // the database cannot name would all otherwise read as a full bag.
        //
        // The streak is cleared when one of our own pick-ups lands, not when
        // anything at all rises. The reader watches every store the player
        // owns, so unrelated changes are common and used to clear it wrongly.
        // A pick-up old enough to have landed and still unaccounted for is one
        // the bag did not take. Judged here and left in place, since the yield
        // learning below has its own use for it.
        int missed = 0;
        for (PendSend& p : g_pend)
        {
            if (p.counted || p.act != Action::Take || p.itemRow < 0) continue;
            if (now - p.at < kBagVerdictMs) continue;
            p.counted = true;
            ++missed;
        }
        g_pend.erase(std::remove_if(g_pend.begin(), g_pend.end(), [now](const PendSend& p) { return now - p.at > 4000; }), g_pend.end());
        if (missed)
        {
            g_noRise += missed;
            if (g_noRise >= kBagFullStreak && !g_bagFull && !game::BagSlots(nullptr, nullptr))
            {
                g_bagFull = true;
                LOG("[bag] %d pick-ups in a row reached nothing: the bag is full, or those items cannot be carried", g_noRise);
            }
        }
        // The bag's own numbers win when they are there. They also say so while
        // the player stands still, which the behaviour check never can.
        int used = 0, cap = 0;
        if (game::BagSlots(&used, &cap))
        {
            if (cap != g_capSeen) { g_capSeen = cap; g_capBias = 0; }
            const bool full = used >= cap + g_capBias;
            if (full != g_bagFull)
            {
                if (full) LOG("[bag] full: %d of %d slots", used, cap);
                else { LOG("[bag] room again: %d of %d slots", used, cap); g_noRise = 0; }
                g_bagFull = full;
            }
            // Worth knowing if the two ever disagree: the fields were found by
            // watching one bag fill, and a wrong guess should not go unnoticed.
            if (!full && g_noRise >= kBagFullStreak)
            {
                static int s_odd = 0;
                if (s_odd < 5) { ++s_odd; LOG("[bag] %d pick-ups reached nothing while the bag reads %d of %d: something else is refusing them", g_noRise, used, cap); }
                g_noRise = 0;
            }
        }
        if (g_bagFull && Settings::Get().notifyBagFull && now - g_bagFullSaid > 30000)
        {
            g_bagFullSaid = now;
            char msg[96];
            if (cap) snprintf(msg, sizeof msg, "Master Looter: bag full, %d of %d slots", used, cap);
            else     snprintf(msg, sizeof msg, "Master Looter: bag full, nothing is being picked up");
            State::Get().Notify(msg, 5000, true);
        }
        // Auto-store. Every rise is held for AutoStorePass, which runs later in
        // the scan, once this scan has seen which objects are still in the world
        // and whether the world changed under it. The one thing judged here is
        // the fallback: a rise no send names is this mod's when one of its own
        // sends made since the last world change could have paid it. A gather
        // whose yield is known pays that item alone and vouches for twelve
        // seconds. A body search or an unnamed gather could pay anything,
        // vouches only while it is pending, and only when nothing was done by
        // hand in the last twelve seconds. None vouches while something taken
        // by hand is an item nothing here can name. A rise read in or just
        // after a shop, a menu or a storage is never offered at all.
        {
            SampleFreePlay(now);
            const bool inPlay = psm::FreePlay() != 0 && !(g_notFreeAt && now - g_notFreeAt < FreeGraceMs());
            g_mineYields.erase(std::remove_if(g_mineYields.begin(), g_mineYields.end(),
                                              [now](const MineYield& m) { return now - m.at > kHandMs; }), g_mineYields.end());
            bool handOpen = g_lastHandAt && now - g_lastHandAt < kHandMs;
            for (const PendSend& p : g_pend) if (!p.mine) handOpen = true;
            // Something taken by hand whose item nothing here can name could
            // have paid any rise, so while one is recent no send vouches, not
            // even one whose yield is known. A named one is caught by row in
            // AutoStorePass.
            bool handUnnamed = false;
            for (const HandTouch& t : g_handTouches)
                if (now - t.at < kHandMs && !g_tidByEid.count(t.eid) && !g_yieldByEid.count(t.eid)) handUnnamed = true;
            // A pet or a companion that just searched a body. The search is
            // stamped with its own id, so this is the one kind of companion
            // loot that can be told from the player's. It vouches the way this
            // mod's own body search does, for eight seconds because the game
            // can hold a delivery that long, and only when nothing was done by
            // hand, since a hand pick-up could have paid the same rise.
            const Config& petCfg = Settings::Get();
            const DWORD petSearch = events::PetSearchAt();
            const bool petVouch = petCfg.petLootToStorage && petSearch && now - petSearch < kPetStoreMs &&
                                  !(g_changeAt && static_cast<LONG>(petSearch - g_changeAt) < 0) && !handOpen;
            for (const auto& [type, units] : bagRose)
            {
                // Never a document or a quest item, by any route. Those are
                // handed out as often as picked up: on 18 September 2026 two
                // supply contracts came in during a camp clear with the mod's
                // gathers pending and were offered. And one the mod did pick up,
                // with the Quest items switch on, still belongs in the bag.
                // Nor a key. The generic Key comes off 931 kinds of character,
                // so a camp clear sent one to storage from nearly every body,
                // and a key is only any use in the bag. The named keys belong
                // to a place or a quest.
                const Item* it = ItemDb::ByRow(type);
                if (it && (it->klass == "document" || it->klass == "key" || it->tags.find(" quest ") != std::string::npos)) continue;
                HeldRise h{ type, units > 0 ? units : 1, now, false, inPlay };
                h.handUnnamed = handUnnamed;
                // A send whose yield is known vouches for that item whatever
                // else the player is doing; a hand pick-up of the same item is
                // caught later by AutoStorePass. A send that could have paid
                // anything vouches only when nothing was done by hand.
                // A body search or an unnamed gather hands over whatever it
                // holds, and the rules never saw any of it. What they refuse
                // is not offered: it stays in the bag, where a pet's haul of
                // the same thing stays too, rather than being put away as if
                // the player had asked for it. An item the database cannot
                // name has no rule to ask and is offered as before.
                const bool rulesAllow = !it || Rules::Decide(*it, Settings::Get()).loot;
                if (!handUnnamed)
                {
                    for (const PendSend& p : g_pend)
                    {
                        if (!p.mine || p.itemRow >= 0 || p.yieldRow >= 0) continue;
                        if (g_changeAt && static_cast<LONG>(p.at - g_changeAt) < 0) continue;
                        if (!handOpen && rulesAllow) h.fallback = true;
                    }
                    for (const MineYield& m : g_mineYields)
                    {
                        if (g_changeAt && static_cast<LONG>(m.at - g_changeAt) < 0) continue;
                        if (m.row == type) h.fallback = true;
                    }
                    // Only what the rules allow. With the pet filter on, a
                    // refused item is on its way out of the bag already, and
                    // with it off the player said what they want carried.
                    if (petVouch && !h.fallback && it && rulesAllow)
                    {
                        h.fallback = true;
                        if (g_debugLog) LOG("[store] +%lld %s came from a companion's body search; offered with the mod's own",
                                            h.units, it->name.c_str());
                    }
                }
                if (g_heldRises.size() < 256) g_heldRises.push_back(h);
            }
        }
        if (rose.empty() || g_pend.empty()) return;
        for (uint16_t type : rose)
        {
            // Sends whose item we already knew explain the rise. As many are
            // credited as units arrived, because a stack does not announce
            // itself once per item: mining a rock spills four Stones, four
            // pick-ups go out, and the bag reports one type going up by four.
            // Crediting one of those and calling the other three missed is
            // what made a full bag out of a productive minute.
            long long units = 1;
            { const auto g = gained.find(type); if (g != gained.end() && g->second > 1) units = g->second; }
            bool credited = false, landed = false;
            for (; units > 0; --units)
            {
                auto known = std::find_if(g_pend.begin(), g_pend.end(), [type](const PendSend& p) { return p.itemRow == type; });
                if (known == g_pend.end()) break;
                if (known->act == Action::Take) landed = true;
                g_pend.erase(known);
                credited = true;
            }
            if (landed)
            {
                g_noRise = 0;
                // Something arrived. If the bag still reads as having no
                // room, the limit we read is too low: believe what just
                // happened over the field.
                int u = 0, c = 0;
                if (game::BagSlots(&u, &c) && u >= c + g_capBias)
                {
                    g_capBias = u + 1 - c;
                    LOG("[bag] an item arrived at %d of %d slots, so the bag holds at least %d: reading the limit as %d from here", u, c, u + 1, c + g_capBias);
                }
                if (g_bagFull) { g_bagFull = false; LOG("[bag] pick-ups are landing again"); }
            }
            if (credited) continue;
            // Otherwise every pending gather of one node type is the source,
            // but only when nothing else (a catch, a pick-up of an unnamed
            // item, a carcass) could explain the rise.
            std::vector<size_t> unknown;
            bool other = false;
            for (size_t i = 0; i < g_pend.size(); ++i)
            {
                if (g_pend[i].act == Action::Gather && g_pend[i].itemRow < 0 && g_pend[i].nodeType) unknown.push_back(i);
                else if (g_pend[i].itemRow < 0) other = true;
            }
            if (unknown.empty() || other) continue;
            {
                const Item* it = ItemDb::ByRow(type);
                if (IsCreatureItem(it)) continue; // a creature came from a catch
            }
            const uint16_t nodeType = g_pend[unknown[0]].nodeType;
            const std::string nodePrefab = g_pend[unknown[0]].node;
            bool same = true;
            // The prefab as well as the id. Several kinds of node share an id,
            // so "every pending gather was this type" was never the same claim
            // as "every pending gather was this kind of thing".
            for (size_t i : unknown)
                if (g_pend[i].nodeType != nodeType || g_pend[i].node != nodePrefab) same = false;
            if (!same || rose.size() > 1) continue; // ambiguous: wait for a cleaner sample
            // Nothing is learned from a gather whose prefab went unrecorded,
            // because nothing later can say which kind of node it was about.
            if (!nodePrefab.empty() && !g_learn.count(nodeType))
            {
                g_learn[nodeType] = { type, nodePrefab };
                const Item* it = ItemDb::ByRow(type);
                LOG("[learn] %s (node type %u) yields %s (%s)", nodePrefab.c_str(), nodeType,
                    it ? it->Label() : "?", it ? it->klass.c_str() : "");
            }
            for (size_t k = unknown.size(); k-- > 0;) g_pend.erase(g_pend.begin() + static_cast<long>(unknown[k]));
        }
    }

    // The arming context the game passes (4th argument) is a pointer. If it is
    // the player entity, its component array or one of its component slots, the
    // same thing is looked up again at call time so it can never be stale.
    static int s_armSlot = -2;          // -2 unknown, -1 raw pointer, 0.. component slot, 100 entity, 101 comps
    static uintptr_t ArmContextNow()
    {
        const uintptr_t raw = hooks::ArmContext();
        if (!raw || !g_me) return static_cast<uintptr_t>(g_meEid);
        const uintptr_t comps = game::Comps(g_me);
        static uintptr_t s_lastRaw = 0;
        if (raw != s_lastRaw)
        {
            s_lastRaw = raw; s_armSlot = -1;
            if (raw == g_me) s_armSlot = 100;
            else if (comps && raw == comps) s_armSlot = 101;
            else if (comps)
                for (unsigned off = 0; off < kComps_SlotsEnd; off += 8)
                    if (mem::Deref(comps, off) == raw) { s_armSlot = static_cast<int>(off / 8); break; }
            const char* cls = mem::RttiShort(raw);
            LOG("[arm] context %llX is %s (%s)", static_cast<unsigned long long>(raw),
                s_armSlot == 100 ? "the player entity" : s_armSlot == 101 ? "the player's component array" : s_armSlot >= 0 ? "a player component" : "an unrelated object",
                cls ? cls : "no class");
        }
        if (s_armSlot == 100) return g_me;
        if (s_armSlot == 101) return comps ? comps : static_cast<uintptr_t>(g_meEid);
        if (s_armSlot >= 0) { const uintptr_t c = comps ? mem::Deref(comps, static_cast<unsigned>(s_armSlot) * 8) : 0; if (c) return c; }
        // Unrelated object: only while it still carries a class vtable.
        if (mem::RttiName(raw)) return raw;
        return static_cast<uintptr_t>(g_meEid);
    }

    // What kind of thing a gather node is, from what it yields.
    enum class GatherKind { Unknown, Plant, Crop, CampFarm, Ore, Stone, Wood, Item, Furniture, Container };
    // What an item counts as for the kind toggles. The classes come straight
    // from the item database (scripts/build_item_db.py): ore and jewel are
    // minerals from veins, stone from quarries, wood from trees and branches.
    // "Plant" means herbs, flowers and mushrooms; a crop (a vegetable, fruit or
    // grain) is food and has its own toggle, on the plant or lying loose.
    // `onGround`: an item lying in the world rather than a node's yield.
    static GatherKind KindFromName(const std::string& kind)
    {
        if (kind == "plant") return GatherKind::Plant;
        // The camp farm crops. Without this they read as Unknown and answer to
        // Unidentified nodes, which is the switch they were stuck behind in the
        // first place, so the table row would have bought nothing.
        //
        // Their own kind rather than Crop, because these are plants the player
        // put there. Crops is on by default and sweeping somebody's farm as they
        // walk past it is not a thing to start doing without being asked.
        if (kind == "farm")  return GatherKind::CampFarm;
        if (kind == "ore")   return GatherKind::Ore;
        if (kind == "stone") return GatherKind::Stone;
        if (kind == "wood")  return GatherKind::Wood;
        if (kind == "item")  return GatherKind::Item;
        if (kind == "pickup") return GatherKind::Item;
        if (kind == "container") return GatherKind::Container;
        return GatherKind::Unknown;
    }

    // What a node turned out to hold, learned from the spill of a break the mod
    // drove and kept in the player's own ini. The table names a yield for 131 of
    // its 966 prefabs and an item rule can only reach a node that has one, so
    // this is how the other 835 come to have one. Keyed by prefab path, the way
    // [NotVeins] is, because a node that has not filled has no type id to key on
    // and the path is known from the first sighting.
    static std::unordered_map<std::string, std::string> g_nodeYield;
    static void SeedNodeYields(const Config& cfg)
    {
        if (g_nodeYield.size() == cfg.nodeYields.size()) return;
        for (const auto& kv : cfg.nodeYields) g_nodeYield.emplace(kv.first, kv.second);
    }
    static const Item* LearnedNodeYield(const char* node)
    {
        if (!node || !node[0]) return nullptr;
        const auto it = g_nodeYield.find(node);
        return it == g_nodeYield.end() ? nullptr : ItemDb::ByStringKey(it->second.c_str());
    }

    // What a gather node hands over, when anything says so. The prefab table
    // names the item for the nodes whose socket and item share a name; then what
    // a node of this prefab was watched paying out, in this session or an earlier
    // one; then a bag diff against the node's type id.
    static const Item* NodeYield(const Cand& c)
    {
        if (c.nodeType && !c.nodeType->itemKey.empty())
            if (const Item* it = ItemDb::ByStringKey(c.nodeType->itemKey.c_str())) return it;
        if (const Item* it = LearnedNodeYield(c.node)) return it;
        return LearnedYield(c.gtid, c.node);
    }

    static GatherKind KindOf(const Item* y, bool onGround = false)
    {
        if (!y) return GatherKind::Unknown;
        const std::string& k = y->klass;
        if (k == "wood"  || y->HasTag("wood"))  return GatherKind::Wood;
        if (k == "stone" || y->HasTag("stone")) return GatherKind::Stone;
        if (k == "ore" || k == "jewel" || y->HasTag("ore") || y->HasTag("mineral")) return GatherKind::Ore;
        if (k == "herb") return GatherKind::Plant;
        // Furnishings, whatever else they also are. Decided before the onGround
        // line below, or a chair lying in a room is just another ground item.
        //
        // This used to read the class, and the class is a single label picked by
        // priority: "furniture" sits twelfth of the thirteen furnishing tags in
        // that order, so a candelabra files as "light", a glass lamp as "lamp",
        // a still life as "painting" and a pot as "flower-pot". None of them
        // ever reached this line, which is why unchecking Furniture still let a
        // room be emptied of everything except the tables. Reading the class
        // covered 123 pieces; reading the tag covers 452.
        //
        // Safe to ask this way round because the tag is on nothing outside the
        // furnishing block, and nothing carrying it is claimed by the wood,
        // stone, ore or herb lines above.
        //
        // Anything that holds things stays with the container rule instead:
        // goblets and bowls are tagged furniture too, and a chest is not a chair.
        if (y->HasTag("furniture") && k != "container" && k != "storage" && k != "chest")
            return GatherKind::Furniture;
        // And then the things that hold things actually answer to the Containers
        // switch, which the line above has always claimed they do. They did not:
        // there was no kind for them, so a bottle, a jar or a clay pot fell
        // through to Item and the only switch that could stop it was Ground
        // items. Seth had Containers off and watched a shelf of clay jars go in
        // the bag on 11 September 2026, twice, and he was right both times.
        // Chests stay out of it: the six items in that class are reward boxes,
        // which Treasure and keepsakes owns and nobody means to refuse by
        // turning off chests and crates.
        if (k == "container" || k == "storage") return GatherKind::Container;
        // Ahead of the onGround line, so a fallen apple and one still on the
        // tree answer to the same switch. Before 1.3.1 both were ground items.
        if (k == "vegetable" || k == "fruit" || k == "grain") return GatherKind::Crop;
        // A seed comes off a plant and is a seed wherever it is lying, so it
        // answers to Plants either way. The twenty of them are their own class,
        // so anyone who wants the herbs and not the pips says so on the Classes
        // tab.
        if (k == "seed") return GatherKind::Plant;
        // Two other things used to be swept in beside it, and only when a node
        // produced them: the alchemy-material class and anything tagged
        // rare-gather. That lumped a Razor Clam, a bar of Chocolate and a
        // Golden Goose Egg in with the herbs, and only sometimes, because the
        // same item lying on the floor was an ordinary pick-up. Neither is a
        // plant, so neither gets a plant's switch. They answer to Ground items
        // when they are loose and to the node's own kind when one yields them,
        // and the choice is on the Classes tab under alchemy-material and on
        // the Tags tab under rare-gather, where it can be made per item instead
        // of by a switch that was never about them.
        //
        // `onGround` is deliberately unused now. It existed to give a thing one
        // kind on the floor and a different one out of a node, which is the
        // inconsistency this removes; crops lost the same split in 1.3.1 so a
        // fallen apple and one on the tree answer to the same switch.
        (void)onGround;
        return GatherKind::Item;
    }

    // The one kind every item in the node's drop list belongs to, or Unknown
    // where the list is empty, names something the database cannot, or holds
    // two kinds at once. A list is a set of possibilities and not a promise, so
    // it is evidence only where the whole set agrees, which is the same bar
    // TableYieldRefused holds it to.
    static GatherKind YieldListKind(const Cand& c)
    {
        if (!c.nodeType || c.nodeType->yields.empty()) return GatherKind::Unknown;
        GatherKind agreed = GatherKind::Unknown;
        for (const std::string& key : c.nodeType->yields)
        {
            const Item* it = ItemDb::ByStringKey(key.c_str());
            if (!it) return GatherKind::Unknown;
            const GatherKind one = KindOf(it);
            if (one == GatherKind::Unknown) return GatherKind::Unknown;
            if (agreed == GatherKind::Unknown) agreed = one;
            else if (agreed != one) return GatherKind::Unknown;
        }
        return agreed;
    }

    // What the node is, in order of how much it can be trusted: the prefab it
    // was placed from, the item it is already known to hold, then what one of
    // its kind yielded earlier this session.
    static GatherKind NodeKind(const Cand& c)
    {
        GatherKind kind = c.nodeType ? KindFromName(c.nodeType->kind) : GatherKind::Unknown;
        // "item" is the game's own catch-all for a collection socket and it
        // covers 44 of the 72 in the table, nearly all of them crops, so it is
        // not an answer on its own. Ask what the node yields before settling
        // for it; a real item still comes back as one.
        if (kind == GatherKind::Unknown || kind == GatherKind::Item || kind == GatherKind::Container)
        {
            const GatherKind byYield = KindOf(c.db ? c.db : NodeYield(c));
            if (byYield != GatherKind::Unknown) kind = byYield;
        }
        // And then the gimmick row's own drop list. item_key names the yield
        // for 131 rows; the drop candidates cover 542, and for a pick-up socket
        // that carries neither an item of its own nor a learned yield it is the
        // only thing that ever says what the thing is. Thirteen cloth rows pay
        // furniture and nine sockets pay a crop, and every one of the
        // twenty-two answered to Ground items alone.
        //
        // Never for a row the table calls a container: what is inside a box
        // does not change what the box is, and letting it would hand the 540
        // container rows to their own contents. A container can still be
        // refined by the line above, which reads the item the entity is rather
        // than the things it holds, and that is the flowerpot case.
        if ((kind == GatherKind::Item || kind == GatherKind::Unknown) &&
            c.nodeType && c.nodeType->kind != "container")
        {
            const GatherKind agreed = YieldListKind(c);
            if (agreed != GatherKind::Unknown) kind = agreed;
        }
        return kind;
    }

    // The switch that owns this kind, or null when it is on. Both the verdict
    // and the arm loop ask, and they must give the same answer: arming a node
    // the verdict will refuse is work done for nothing at best, and on a
    // damaging thorn it half-transitions the vine and leaves the volume that
    // hurts the player standing in thin air. Issue #41.
    // A felled tree keeps falling for several seconds after the gather is sent,
    // and a player flying past has left that part of the map by the time it
    // lands. c4123456, bugs tab, 16 and 17 September 2026: two crashes in the
    // game's own code, each about seven seconds after the mod started on a big
    // oak 12.6 m and 15.8 m away with the gather range at 50, and the same route
    // flown with Wood off did not crash. The same session felled 135 trees
    // without trouble, so it is a race and not every tree. A tree is only
    // started close by and at a pace no faster than a gallop, whatever the
    // gather range says.
    static constexpr float kTreeReachM = 10.0f;
    static constexpr float kTreeSpeedMps = 12.0f;
    static constexpr DWORD kSettleMs = 5000;
    static const char* TreeUnsafe(GatherKind kind, float dist)
    {
        if (kind != GatherKind::Wood) return nullptr;
        if (dist > kTreeReachM) return "tree further than 10 m (a falling tree can outlive the area it is in)";
        if (g_moveSpeed > kTreeSpeedMps) return "moving too fast to start felling a tree";
        if (g_changeAt && GetTickCount() - g_changeAt < kSettleMs) return "the world just changed; trees wait a few seconds";
        return nullptr;
    }

    // The same limits for an ore break, which drives the game's state machine
    // through a raw component pointer. c4123456's third crash, 17 September
    // 2026, on the tree-guard build: the player covered 987.5 m in one scan
    // flying away from a camp, and three seconds later the mod drove a break at
    // an iron vein 45.7 m away. The drive faulted inside the game at once, and
    // the component it was aimed at no longer had RTTI, so the area had been
    // unloaded between the scan that saw the vein and the break. A vein is broken
    // only close by, at a walking or riding pace, and not in the first seconds
    // after the world changes.
    static const char* BreakUnsafe(float dist, DWORD now)
    {
        if (dist > kTreeReachM) return "vein further than 10 m; breaks are driven close by";
        if (g_moveSpeed > kTreeSpeedMps) return "moving too fast to break a vein";
        if (g_changeAt && now - g_changeAt < kSettleMs) return "the world just changed; breaks wait a few seconds";
        return nullptr;
    }

    static const char* GatherSwitchOff(GatherKind kind, const Config& cfg)
    {
        switch (kind)
        {
        case GatherKind::Plant:     return cfg.gatherPlants   ? nullptr : "plants off";
        case GatherKind::Crop:      return cfg.gatherCrops    ? nullptr : "crops off";
        case GatherKind::CampFarm:  return cfg.gatherCampFarm ? nullptr : "your own camp farm (off)";
        // Stone has no switch of its own. Every rock answers to Ore, and
        // which stones to keep is an item rule now: the database gives
        // Stone, Fine Stone, Flawless Stone and Stalactite the class
        // stone and the tag to match, so a class rule covers all four
        // without stopping the mod breaking the node it came out of.
        case GatherKind::Ore:
        case GatherKind::Stone:     return cfg.gatherOre      ? nullptr : "ore off";
        case GatherKind::Wood:      return cfg.gatherWood     ? nullptr : "wood off";
        case GatherKind::Item:      return cfg.pickUpItems    ? nullptr : "pick up off";
        case GatherKind::Furniture: return cfg.lootFurniture  ? nullptr : "furniture off";
        case GatherKind::Container: return cfg.lootContainers ? nullptr : "containers off";
        default:                    return cfg.gatherUnknown  ? nullptr : "unidentified nodes off";
        }
    }

    // The item classes a node of this kind can hand over, where the node table
    // is evidence enough to say. Read it off the 131 rows of
    // MasterLooter.nodes.tsv that name a yield, never off the kind's own name:
    // the first version of this guessed from the name and got two of four
    // wrong, which a review caught before it shipped.
    //
    //   stone   57 named, every one of them class stone
    //   ore     19 named: 8 jewel, 7 stone, 4 ore
    //   plant   17 named: 11 herb, 3 grain, 3 crafting-material
    //   wood     0 named, out of 378 prefabs
    //
    // So an ore node pays a jewel more often than it pays ore, and it pays
    // stone nearly as often, which is why all three have to be refused before
    // one can be passed over. Plant and wood are left out and get no opinion at
    // all. Plant's named rows already spill into three classes and an unnamed
    // one, gimmick_rare_collect_kudzu_0001, pays Skyroot, which is a vegetable;
    // wood has no evidence whatsoever, so a set for it would be invention.
    // Refusing the class wood therefore still does nothing to a wood node, and
    // the honest place to say so is the help text, not a guess here.
    //
    // Anything added here needs the same kind of evidence. A wrong entry does
    // not fail loudly: it quietly stops the mod touching a node the player was
    // happy to have.
    static const char* const* KindClasses(GatherKind kind, int& n)
    {
        static const char* kStone[] = { "stone" };
        static const char* kOre[]   = { "ore", "jewel", "stone" };
        switch (kind)
        {
        case GatherKind::Stone: n = 1; return kStone;
        case GatherKind::Ore:   n = 3; return kOre;
        default:                n = 0; return nullptr;
        }
    }

    static constexpr int kGatherKinds = 9;   // the enumerators of GatherKind

    // Positions in ItemDb::All() of everything each kind can yield, indexed
    // once. Small lists: 4 stone, and 18 for ore, jewel and stone together.
    static const std::vector<int>& KindItems(GatherKind kind)
    {
        static std::vector<int> lists[kGatherKinds];
        static bool built = false;
        if (!built && ItemDb::Loaded())
        {
            built = true;
            const std::vector<Item>& all = ItemDb::All();
            for (int r = 0; r < static_cast<int>(all.size()); ++r)
                for (int k = 0; k < kGatherKinds; ++k)
                {
                    int n = 0;
                    const char* const* cl = KindClasses(static_cast<GatherKind>(k), n);
                    for (int i = 0; i < n; ++i)
                        if (all[r].klass == cl[i]) { lists[k].push_back(r); break; }
                }
        }
        return lists[static_cast<int>(kind)];
    }

    // Whether the rules refuse everything a node of each kind can yield. Worked
    // out once a pass and read by the verdict and by the arm loop, which have
    // to give the same answer.
    //
    // A node is not an item, so nothing about it reaches the Classes, Tags or
    // Items tabs unless the mod already knows what it holds, and mostly it does
    // not: 275 of the 332 stone prefabs name no yield. The Stone switch covered
    // them until 1.6.13 removed it, saying the class rule was the finer
    // control. For a stone lying on the ground it is, because there is an item
    // there to judge. For the rock it came out of there was nothing to ask, so
    // Ore and stone alone decided and a player who had refused the class
    // watched stone arrive anyway. symplexity reported that on the bugs tab the
    // morning after the release, with the class off and the items set to never.
    //
    // Refusing the node takes every class of its kind refused, never one of
    // them: a vein holds a jewel as readily as ore, and a guess in that
    // direction costs the player something they asked for.
    static const char* g_kindRefusal[kGatherKinds] = { nullptr };
    static void RefreshKindRules(const Config& cfg)
    {
        static const struct { GatherKind kind; const char* reason; } kAsk[] = {
            { GatherKind::Stone, "your rules refuse all stone" },
            { GatherKind::Ore,   "your rules refuse all ore, jewels and stone" },
        };
        const std::vector<Item>& all = ItemDb::All();
        for (const auto& a : kAsk)
        {
            const std::vector<int>& rows = KindItems(a.kind);
            bool wanted = false;
            for (int r : rows) if (Rules::Decide(all[r], cfg).loot) { wanted = true; break; }
            g_kindRefusal[static_cast<int>(a.kind)] = (!rows.empty() && !wanted) ? a.reason : nullptr;
        }
    }
    static const char* KindRefused(GatherKind kind)
    {
        const int i = static_cast<int>(kind);
        return (i >= 0 && i < kGatherKinds) ? g_kindRefusal[i] : nullptr;
    }

    // Everything this node can hand over, refused. The node table reads the drop
    // entries out of the game's own gimmick row, so for 302 of its 966 prefabs
    // there is a real list to ask about rather than a guess from the kind's name.
    //
    // A list is a set of possibilities and not a promise, so this only ever
    // refuses when the rules refuse the whole set. An entry that does not belong
    // there makes the mod more willing to touch the node; a missing one is what
    // the learned [NodeYields] net covers. And one entry the item database cannot
    // name means the set cannot be judged at all, so nothing is refused.
    //
    // This is the answer to a rock that broke and paid nothing anyone wanted.
    // gimmick_collect_her_rock_b_0005 names no single yield and its kind is ore,
    // so with the class stone refused and ore allowed the mod broke it, and the
    // stone it spilled was refused where it lay. The row says it holds Stone,
    // Fine Stone and Flawless Stone and nothing else, which settles it before the
    // first swing rather than after it.
    // Named, so the translation template's collector can find a reason that
    // reaches skip() through a variable rather than as a literal argument.
    static const char* const kReason_NodeAllRefused = "your rules refuse everything this node holds";
    static const char* const kReason_NodeMechanism = "part of a mechanism, not loose loot";
    static const char* TableYieldRefused(const Cand& c, const Config& cfg)
    {
        if (!c.nodeType || c.nodeType->yields.empty()) return nullptr;
        for (const std::string& key : c.nodeType->yields)
        {
            const Item* it = ItemDb::ByStringKey(key.c_str());
            if (!it) return nullptr;
            if (Rules::Decide(*it, cfg).loot) return nullptr;
        }
        return kReason_NodeAllRefused;
    }

    // An object gets up to four attempts, each waiting longer than the last
    // (the game may refuse an event sent from too far away, and the object is
    // still there when we come closer). Only after that is it given up for
    // the session.
    static constexpr int kMaxTries = 4;
    static bool RecentlyDone(uint64_t k, DWORD now, int retryMs)
    {
        auto it = g_done.find(k);
        if (it == g_done.end()) return false;
        const DWORD wait = static_cast<DWORD>(retryMs) * static_cast<DWORD>(it->second.tries);
        return now - it->second.when < wait;
    }
    static void MarkDone(uint64_t k, DWORD now)
    {
        auto it = g_done.find(k);
        if (it == g_done.end()) { g_done[k] = { now, 1 }; return; }
        it->second.when = now;
        if (++it->second.tries > kMaxTries) g_searched.insert(k); // never disappears: give up on it
    }
    // A respawned item comes back with a new id at the same place after the
    // old one vanished. Chunks from a broken node or drops from a kill also
    // share a place and a type, but they exist at the same time, so an object
    // whose predecessor is still present is a different item, not a respawn.
    static bool SpotRecent(const Vec3& p, uint16_t tid, uint32_t eid, DWORD now, int retryMs)
    {
        for (const Spot& s : g_spots)
        {
            if (s.tid != tid || now - s.when >= static_cast<DWORD>(retryMs)) continue;
            const float dx = s.p.x - p.x, dy = s.p.y - p.y, dz = s.p.z - p.z;
            if (dx * dx + dy * dy + dz * dz >= 0.36f) continue;
            if (s.eid == eid) return true;                 // the very object we just sent to
            if (g_present.count(s.eid)) continue;         // its neighbour still exists: a sibling, not a respawn
            return true;
        }
        return false;
    }
    static void SpotMark(const Vec3& p, uint16_t tid, uint32_t eid, DWORD now)
    {
        for (Spot& s : g_spots)
        {
            const float dx = s.p.x - p.x, dy = s.p.y - p.y, dz = s.p.z - p.z;
            if (s.tid == tid && s.eid == eid && dx * dx + dy * dy + dz * dz < 0.36f) { s.when = now; return; }
        }
        if (g_spots.size() >= 128) g_spots.erase(g_spots.begin());
        g_spots.push_back({ p, tid, now, eid });
    }

    // Where the mod has broken a vein. A break spills the vein's contents as
    // loose items at one point, which is the shape the storage rule below
    // reads as a container's contents, so the mod's own spill was refused as
    // storage and the ore trickled in as the cluster thinned. Remembering the
    // site is what tells the two apart: a chest does not appear where the mod
    // just swung. Deliberately narrow. It exempts only what the mod itself
    // broke, so a container standing anywhere else keeps the full guard.
    struct Broke { Vec3 p; DWORD when; };
    static std::vector<Broke> g_broke;
    static constexpr float kBrokeRadius2  = 9.0f;      // 3 m, enough for the impulse to scatter chunks
    static constexpr DWORD kBrokeWindowMs = 45000;     // a spill is gathered in seconds, not minutes
    static void BrokeMark(const Vec3& p, DWORD now)
    {
        if (g_broke.size() >= 32) g_broke.erase(g_broke.begin());
        g_broke.push_back({ p, now });
    }
    // Not every node the table calls ore is a vein. Breaking a bismuth vein
    // spawns ore chunks that are gimmicks in their own right, and the chunk is
    // what the player picks up. The chunk's chart has no transition for the
    // break pair, so driving it there does nothing at all: no drop, no state
    // change, and the mod retired the chunk as done and left the ore lying on
    // the ground. LuxDragon and Proud Wingman both reported it as bismuth
    // being hit or miss, which is what it looks like from the saddle: the
    // chunks that got picked up were the ones the player walked over.
    //
    // Nothing in the tables says which is which, so the mod asks the game. A
    // vein answers the break with its drop event inside a frame or two. A node
    // that ignores the pair answers with nothing, and after a second and a half
    // of silence the mod takes the node back off the retired list, gathers it
    // the way the player would, and remembers the prefab so the next chunk of
    // that kind is gathered outright. One line says so the first time.
    static volatile LONG g_dropRing[32] = {};
    static volatile LONG g_dropRingAt = 0;

    void NoteBreakDrop(uint32_t eid)
    {
        const LONG slot = InterlockedIncrement(&g_dropRingAt) & 31;
        InterlockedExchange(&g_dropRing[slot], static_cast<LONG>(eid));
    }

    static bool SawDropFor(uint32_t eid)
    {
        for (int i = 0; i < 32; ++i)
            if (static_cast<uint32_t>(InterlockedCompareExchange(&g_dropRing[i], 0, 0)) == eid) return true;
        return false;
    }

    struct DrivenBreak { uint32_t eid; uint64_t key; DWORD when; char node[160]; };
    static std::vector<DrivenBreak> g_drivenBreaks;
    static constexpr DWORD kBreakAnswerMs = 1500;        // a drop lands in a frame or two

    // The answer is kept in the player's own MasterLooter.ini, under
    // [NotVeins], because it costs a wasted break on every chunk in the first
    // pile to work out and the answer does not change between launches. It is
    // learned in play on each player's machine, so no list of prefab names
    // ships with the mod and a node the next game patch adds is picked up by
    // whoever mines it first. Prefab names are stable; the type numbers beside
    // them in the log are not, which is why this is keyed by name.
    //
    // Two containers on purpose. This one belongs to the scan thread and is
    // read on the hot path with no lock. The settings copy is the durable one
    // and is only touched when something new is learned, under the settings
    // mutex, because the game thread saves and reloads that file underneath us.
    static std::unordered_set<std::string> g_notVeins;

    static bool NotAVein(const char* node)
    {
        return node && node[0] && g_notVeins.count(node) != 0;
    }

    // Anything the file already knew, plus anything learned in an earlier pass.
    // Cheap: a handful of entries, and nothing to do once they are in.
    static void SeedNotVeins(const Config& cfg)
    {
        if (g_notVeins.size() == cfg.notVeins.size()) return;
        for (const std::string& n : cfg.notVeins) g_notVeins.insert(n);
    }

    // Called once a pass. Anything driven longer ago than the answer window is
    // judged: drop seen, it was a vein and there is nothing to do; silence, and
    // the node goes back to being gatherable.
    static void ReviewBreaks(DWORD now)
    {
        for (size_t i = 0; i < g_drivenBreaks.size();)
        {
            const DrivenBreak& b = g_drivenBreaks[i];
            if (now - b.when < kBreakAnswerMs) { ++i; continue; }
            if (!SawDropFor(b.eid))
            {
                g_searched.erase(b.key);
                g_searched.erase(b.eid);
                g_retiredEid.erase(b.eid);
                g_done.erase(b.key);
                if (b.node[0] && !NotAVein(b.node))
                {
                    g_notVeins.insert(b.node);
                    {
                        std::lock_guard<std::recursive_mutex> lk(Settings::Mutex());
                        Settings::Get().notVeins.insert(b.node);
                    }
                    Settings::MarkDirty();
                    LOG("[break] %s did not answer the break, so it is not a vein: gathering these instead. "
                        "A vein drops within a frame or two of the swing; this one dropped nothing. "
                        "Written to [NotVeins] in MasterLooter.ini, so it is worked out once and not again.", b.node);
                }
            }
            g_drivenBreaks.erase(g_drivenBreaks.begin() + static_cast<long>(i));
        }
    }

    // Debug only. After the mod breaks a vein, keep reading it for a few
    // seconds so the slot probe can say whether the break moved its state.
    // A hand-broken vein goes from +0x350 = 3 to 5 and gains its point data at
    // +0x140; the question that matters is whether the mod's break does the
    // same, and the retirement shortcut in the fill loop hides the answer by
    // skipping the node from the next scan on. This is also the success test
    // for any future fix: 3 -> 5 means it worked.
    static std::unordered_map<uint32_t, DWORD> g_brokeWatch;
    static constexpr DWORD kBrokeWatchMs = 10000;
    static bool WatchingBreak(uint32_t eid, DWORD now)
    {
        auto it = g_brokeWatch.find(eid);
        if (it == g_brokeWatch.end()) return false;
        if (now - it->second > kBrokeWatchMs) { g_brokeWatch.erase(it); return false; }
        return true;
    }

    static bool NearOwnBreak(const Vec3& p, DWORD now)
    {
        for (const Broke& b : g_broke)
        {
            if (now - b.when > kBrokeWindowMs) continue;
            const float dx = b.p.x - p.x, dy = b.p.y - p.y, dz = b.p.z - p.z;
            if (dx * dx + dy * dy + dz * dz < kBrokeRadius2) return true;
        }
        return false;
    }
    static int WouldStealCached(const Cand& c)
    {
        const DWORD now = GetTickCount();
        const auto it = g_ownAns.find(c.eid);
        if (it != g_ownAns.end() && now - it->second.when <= kOwnAnsMs) return it->second.steal;
        const int steal = hooks::WouldSteal(g_me, c.ent);
        // "Not armed yet" says something about the mod, not about the object,
        // and arming can finish on the very next tick. Not worth remembering.
        if (steal != -1) g_ownAns[c.eid] = { now, steal };
        return steal;
    }
    // --- drawing water from a well ------------------------------------------
    //
    // A well raises no loot event. Both loot modes were tried and both take the
    // bucket with the water (see the note in Decide). What a hand draw does
    // raise is a run of state transitions on the winch, captured on 2026-09-08
    // beside a well with the debug log on, and this replays that run.
    //
    // Three parts matter, found by prefab. parts02 is the winch and carries
    // nearly everything; parts02_part hangs off it; parts01 is the bucket, the
    // one that grows a gather block when it fills, and it takes the last
    // transition of the run.
    //
    // The ids are numbers because eight of the nine appear nowhere in the game's
    // strings: name_events.py hashes every identifier in the exe and none of
    // these match. A gimmick whose chart has no transition for an id ignores it,
    // so a wrong id is a no-op rather than damage.
    enum WellPart { WellWinch = 0, WellWinchPart = 1, WellBucket = 2 };
    struct WellStep { uint32_t atMs; uint8_t part; uint32_t ev; };
    // The captured hand draw, fifteen transitions over eleven seconds with every
    // id and timing, is in docs/investigations/WELL.md. It lived here as a table
    // until nothing referenced it.

    // What the mod actually drives, and why it is one step and not fifteen.
    //
    // Winding the winch works: the captured run was driven back to back nineteen
    // times in two minutes and paid out every time. It is also unusable. A run
    // holds the well for eleven seconds, the cooldown is a second, so the well
    // is never free, and a player who reaches for the handle mid-run has it
    // taken out of their hands. The winch state confirms both halves: it reads
    // MinAngle (0x61964BBC) while a player turns it and Wait (0x866C7489) at
    // the start and end of every mod run, and checking that at the start of a
    // run cannot help when a run is always already in progress.
    //
    // So the mod does not touch the winch. The player winds, and the mod asks
    // the bucket alone for what it is holding, which is the last transition of
    // the captured run and the only one aimed at the bucket rather than the
    // winch. Nothing the mod drives is anything the player is holding.
    static const WellStep kWellTake[] = {
        { 0, WellBucket, 0x003ECC59 },
    };
    static constexpr int kWellSteps = static_cast<int>(sizeof kWellTake / sizeof kWellTake[0]);

    struct WellRun
    {
        uint32_t eid[3] = {};
        uintptr_t comp[3] = {};
        DWORD startedAt = 0;
        int step = 0;
        bool active = false;
    };
    static WellRun g_wellRun;
    // One well at a time, and a second apart on purpose.
    //
    // The comment here used to justify the second by saying a run takes eleven
    // seconds. It does not: kWellTake is a single transition at 0 ms, so a run
    // is over in about 35 ms and this really does allow a redraw every second.
    //
    // That is deliberate and it is the pace Seth wants. I raised it to fifteen
    // seconds after reading twenty-five draws in twenty-six seconds as spam,
    // which was the feature working. Reverted. The absence of [loot] lines for
    // the water misled me too: a well yields through a state transition, not a
    // loot event, so nothing about it appears there.
    static std::unordered_map<uint32_t, DWORD> g_wellDone;
    static constexpr DWORD kWellCooldownMs = 1000;

    // A gimmick component keeps the name id of the state it is in at +0x270.
    // Reading it is how the mod knows to keep its hands off a winch the player
    // is already turning: winding one out from under them takes it out of their
    // hands and leaves it unusable. Idle is Wait.
    static uint32_t GimmickState(uintptr_t comp)
    {
        uint32_t st = 0;
        return (comp && mem::Read32(comp + 0x270, &st)) ? st : 0;
    }

    // Was this the player's a moment ago? Both halves have to agree: the same
    // entity, and the same item in it if either side knows which.
    static bool WasOnMe(const Cand& c, DWORD now)
    {
        const auto it = g_onMe.find(c.eid);
        if (it == g_onMe.end() || now - it->second.when > kOnMeWindowMs) return false;
        return !c.tid || !it->second.tid || c.tid == it->second.tid;
    }
    // Dropped out of the bag by hand, as ClaimHandDrops recognised it. The
    // instance id carries the bag slot's own id from the moment the scan hears
    // of the drop, and every piece of a dropped stack shares it, as long as
    // the item row agrees.
    static bool Dropped(const Cand& c)
    {
        if (g_droppedEid.count(c.eid)) return true;
        if (!c.iid) return false;
        const auto it = g_droppedIid.find(c.iid);
        return it != g_droppedIid.end() && it->second == c.tid;
    }
    static DWORD AgeMs(uint32_t eid, DWORD now)
    {
        auto it = g_firstSeen.find(eid);
        if (it == g_firstSeen.end()) { g_firstSeen[eid] = now; return 0; }
        return now - it->second;
    }

    // Watching what a break pays out, so the node can be judged by its own
    // contents next time instead of by the switch its kind happens to answer to.
    //
    // symplexity's report is the case that earned this.
    // gimmick_collect_her_rock_b_0005 is filed as ore with no named yield, so
    // refusing the class stone left the Ore and stone switch deciding on its own:
    // the mod broke the rock, the stone it spilled was refused where it lay, and
    // the rock was gone for nothing. Write down what fell out and the verdict
    // refuses the node itself from the next one onwards.
    //
    // Three guards, because a wrong entry here is permanent. The game's own drop
    // event has to have fired for that node, so we know it really paid out. The
    // item has to be within the spill radius and first seen after the swing, so
    // something that was already lying there says nothing. And two different
    // items in one spill abandon the attempt rather than pick one.
    struct SpillWatch { char node[160]; Vec3 p; DWORD when; uint32_t eid; char yield[80]; bool mixed; };
    static std::vector<SpillWatch> g_spillWatch;
    static constexpr DWORD kSpillWatchMs = 6000;

    static void WatchSpill(const char* node, const Vec3& p, uint32_t eid, DWORD now)
    {
        if (!node || !node[0] || g_nodeYield.count(node)) return;
        if (g_spillWatch.size() >= 16) g_spillWatch.erase(g_spillWatch.begin());
        SpillWatch w{};
        snprintf(w.node, sizeof w.node, "%s", node);
        w.p = p; w.when = now; w.eid = eid;
        g_spillWatch.push_back(w);
    }

    // A break already being watched answers for its whole prefab, so the next
    // rock of the same kind waits rather than being broken for the same lesson.
    // Seth's run broke three in the six seconds before the first answer landed.
    static bool WatchingSpillFor(const char* node, DWORD now)
    {
        if (!node || !node[0]) return false;
        for (const SpillWatch& w : g_spillWatch)
            if (now - w.when <= kSpillWatchMs && strcmp(w.node, node) == 0) return true;
        return false;
    }

    static void NoteSpill(const Cand& c, DWORD now)
    {
        if (!c.item || !c.db || c.db->stringKey.empty() || g_spillWatch.empty()) return;
        const DWORD age = AgeMs(c.eid, now);
        for (SpillWatch& w : g_spillWatch)
        {
            if (now - w.when > kSpillWatchMs) continue;
            if (age > now - w.when) continue;             // it was there before the swing
            const float dx = w.p.x - c.pos.x, dy = w.p.y - c.pos.y, dz = w.p.z - c.pos.z;
            if (dx * dx + dy * dy + dz * dz >= kBrokeRadius2) continue;
            if (!w.yield[0]) snprintf(w.yield, sizeof w.yield, "%s", c.db->stringKey.c_str());
            else if (c.db->stringKey != w.yield) w.mixed = true;
        }
    }

    static void ReviewSpills(DWORD now)
    {
        for (size_t i = 0; i < g_spillWatch.size();)
        {
            SpillWatch& w = g_spillWatch[i];
            if (now - w.when < kSpillWatchMs) { ++i; continue; }
            if (w.yield[0] && !w.mixed && SawDropFor(w.eid) && !g_nodeYield.count(w.node))
            {
                g_nodeYield[w.node] = w.yield;
                {
                    std::lock_guard<std::recursive_mutex> lk(Settings::Mutex());
                    Settings::Get().nodeYields[w.node] = w.yield;
                }
                Settings::MarkDirty();
                const Item* it = ItemDb::ByStringKey(w.yield);
                LOG("[learn] %s holds %s (%s), from what fell out of it. Your class, tag and item "
                    "rules reach this kind of node from now on, and it is in [NodeYields] in "
                    "MasterLooter.ini so it is worked out once and not again.",
                    w.node, it ? it->Label() : w.yield, it ? it->klass.c_str() : "?");
            }
            g_spillWatch.erase(g_spillWatch.begin() + static_cast<long>(i));
        }
    }

    static bool IStr(const char* hay, const char* needle)
    {
        if (!hay || !*hay) return false;
        const size_t n = strlen(needle);
        for (const char* h = hay; *h; ++h) if (_strnicmp(h, needle, n) == 0) return true;
        return false;
    }

    // A mechanism the mod may arm, so the game offers its interaction, but must
    // never take from. Recognised by the folder it lives in: the runtime type id
    // this used to test is not stable between sessions (the same copper vein was
    // 50869 at 19:23 and 52229 at 19:29), so the number written here matched a
    // well bucket once and matches whatever holds it now. All 18 of the game's
    // well parts sit under /well/; matching the word would also claim the flags
    // at Wells, a chocolate mixer and two junk wells.
    static bool IsMechanism(const char* node)
    {
        return node && node[0] && IStr(node, "/well/");
    }

    // Nodes the mod must not touch at all, by the only durable name they
    // have. A memory fragment starts a scene when disturbed, a puzzle pillar
    // or power core breaks the puzzle it belongs to, and the abyss artifacts
    // are fast travel. Decide() refuses to loot them; this is so arming does
    // not poke them either, which it happily did.
    // The trigger ids a node will answer to, read out of its own map. The
    // layout is the one the lookup at +0x4045F0 walks; see ProbeTriggerMap.
    // Returns how many were read.
    static int ReadTriggerIds(uintptr_t comp, uint32_t* out, int max)
    {
        if (!comp || max <= 0) return 0;
        uint32_t buckets = 0;
        uintptr_t bucketArr = 0, entryArr = 0;
        if (!mem::Read32(comp + 0xF0, &buckets) || !buckets) return 0;
        if (!mem::ReadPtr(comp + 0x100, &bucketArr) || !bucketArr) return 0;
        if (!mem::ReadPtr(comp + 0x108, &entryArr) || !entryArr) return 0;
        int n = 0;
        for (uint32_t b = 0; b < buckets && b < 64 && n < max; ++b)
        {
            const uintptr_t blk = bucketArr + static_cast<uintptr_t>(b) * 256;
            uint32_t cnt = 0;
            if (!mem::Read32(blk, &cnt) || cnt > 30) continue;
            for (uint32_t i = 0; i < cnt && n < max; ++i)
            {
                uint32_t idx = 0;
                if (!mem::Read32(blk + i * 8 + 0x0C, &idx)) continue;
                uintptr_t ent = 0;
                if (!mem::ReadPtr(entryArr + static_cast<uintptr_t>(idx) * 8, &ent) || !ent) continue;
                uint32_t id = 0;
                if (!mem::Read32(ent + 4, &id) || !id) continue;
                bool dup = false;
                for (int j = 0; j < n; ++j) if (out[j] == id) { dup = true; break; }
                if (!dup) out[n++] = id;
            }
        }
        return n;
    }

    // Somewhere for an id to live between being chosen on the scan thread and
    // being dereferenced on the game thread, since events::Arm only queues the
    // pointer. A ring, because several arms can be in flight.
    static uint32_t   g_idRing[64] = {};
    static volatile LONG g_idNext = 0;
    static uintptr_t HoldId(uint32_t id)
    {
        const LONG slot = InterlockedIncrement(&g_idNext) & 63;
        g_idRing[slot] = id;
        return reinterpret_cast<uintptr_t>(&g_idRing[slot]);
    }

    // The trigger-name map on a gimmick component, laid out by the lookup at
    // +0x4045F0 that decides whether arming does anything:
    //
    //   +0xF0  u32 bucket count        zero here and the lookup returns null
    //   +0xF4  u32 size
    //   +0x100 -> buckets, 256 bytes each: u32 count, then {u32 hash, u32 idx}
    //   +0x108 -> array of entry pointers; an entry is {.., u32 name @+4, u8 @+8}
    //
    // Read only. One report per prefab, so a walk past a dozen veins is a dozen
    // lines and not a flood.
    static void ProbeTriggerMap(const Cand& k, uintptr_t comp)
    {
        if (!g_debugLog || !comp || !k.node[0]) return;
        static std::unordered_set<std::string> s_done;
        if (s_done.size() >= 64) return;
        if (!s_done.insert(k.node).second) return;

        uint32_t buckets = 0, size = 0;
        uintptr_t bucketArr = 0, entryArr = 0;
        mem::Read32(comp + 0xF0, &buckets);
        mem::Read32(comp + 0xF4, &size);
        mem::ReadPtr(comp + 0x100, &bucketArr);
        mem::ReadPtr(comp + 0x108, &entryArr);

        if (!buckets || !bucketArr || !entryArr)
        {
            LOG("[trigmap] %s has NO trigger names (buckets %u size %u): arming can never do anything to it, whatever name is passed.",
                k.node, buckets, size);
            return;
        }

        char line[420]; int p = 0; int found = 0;
        for (uint32_t b = 0; b < buckets && b < 64 && found < 24; ++b)
        {
            const uintptr_t blk = bucketArr + static_cast<uintptr_t>(b) * 256;
            uint32_t n = 0;
            if (!mem::Read32(blk, &n) || n > 30) continue;
            for (uint32_t i = 0; i < n && found < 24; ++i)
            {
                uint32_t idx = 0;
                if (!mem::Read32(blk + i * 8 + 0x0C, &idx)) continue;
                uintptr_t ent = 0;
                if (!mem::ReadPtr(entryArr + static_cast<uintptr_t>(idx) * 8, &ent) || !ent) continue;
                uint32_t name = 0; uint8_t state = 0;
                mem::Read32(ent + 4, &name);
                mem::Read8(ent + 8, &state);
                p += snprintf(line + p, sizeof line - p, "%s%u=%u", found ? " " : "", name, state);
                ++found;
            }
        }
        // What the mod itself is asking for, so the two can be compared. The
        // detour keeps a pointer to the id; the id is the u32 behind it, and
        // nothing has ever printed it.
        const uintptr_t askPtr = hooks::ArmArg3();
        uint32_t asking = 0;
        const bool askOk = askPtr && mem::Read32(askPtr, &asking);

        LOG("[trigmap] %s buckets %u size %u names(state): %s", k.node, buckets, size,
            found ? line : "(none read)");
        LOG("[trigmap]   the mod is arming with id %s, which this node %s",
            askOk ? "(see below)" : "unknown",
            askOk ? "(compare against the list above)" : "cannot be compared");
        if (askOk) LOG("[trigmap]   asking id = %u (from %llX)", asking,
            static_cast<unsigned long long>(askPtr));
    }

    // A gimmick_attach_ prefab is a piece bolted to a creature or a mechanism:
    // stoneworm and stonetoad plating, stoneowl bases, landspider queen rocks,
    // seraphim stones, thorny vines. Gathering one empties its visual and
    // leaves whatever carries its damage volume standing, so burnt vines went
    // invisible and went on hurting the player, which is issue #41. The game
    // tags the six real mining spots under this prefix itself and those keep
    // working; the other 110 rows were the generator guessing from the name
    // and are gone from the table. This still earns its place twice over: a
    // loose MasterLooter.nodes.tsv built by an older generator carries them
    // again, and with nothing in the table they fall to Unidentified nodes,
    // which anyone may switch on.
    static bool AttachedPart(const char* node, const NodeType* type)
    {
        if (type && type->tagged) return false;
        return IStr(node, "gimmick_attach_");
    }

    // A seed the player has just planted in the camp farm. The game builds one
    // of these for each of its 20 farm plants, gimmick_camp_farm_<plant>_seed,
    // and tags it catch_onehand, so it reads as a loose seed and the mod took it
    // straight back out of the soil. Kuradeon on the posts tab, 19 September
    // 2026, with GatherCampFarm=0 set: the switch covers the growing stages the
    // node table carries and never covered this one, which the table leaves out.
    // Taking a planted seed undoes the planting whatever the switch says, so it
    // is never taken at all.
    static bool PlantedSeed(const char* node)
    {
        return node && node[0] && IStr(node, "gimmick_camp_farm_") && IStr(node, "_seed");
    }

    // A Field Pot or a Bonfire, the two cooking fires a player can place. The
    // prefab is the placed form of an item with a blueprint, class
    // cooking-facility, and the game tags it as nothing, so it answered to
    // Unidentified nodes alone and no class rule could reach it. Sov1737's log
    // of 24 September 2026 gathers a Field Pot twice at 19:29, having refused
    // that class at 19:07. Nothing in the log says whose it was, and filing it under
    // Furniture, which is on by default, would have every player's mod pick
    // up the fires in their own camp, so it is never touched at all.
    static bool PlacedCookFire(const char* node)
    {
        return node && node[0] && IStr(node, "gimmick_craft_cook_campfire_");
    }

    static bool OffLimits(const char* node, bool unwornEquip = false)
    {
        if (!node || !node[0]) return false;
        if (PlantedSeed(node)) return true;
        if (PlacedCookFire(node)) return true;
        // "puzzle" earns its place: the game tags gimmick_puzzle_ice_wall_break,
        // _ice_block_break, _stone_wall_break and _pickaxe_break_point as
        // collect_mine, so they read as ordinary ore and the mod would open
        // them. Breaking the wall is the puzzle; solving it is the player's.
        // "woodthorn" is the damaging vine of issue #41. Arming one drives a
        // transition the node does not declare: the vine's visual goes and the
        // volume that hurts the player stays, which left the Duskwood pillar
        // puzzle unfinishable. It holds nothing, so nothing is lost by never
        // touching it, and this stands even when Unidentified nodes is on.
        // "equip_openclose" is the helm and cloak the game wants the player to
        // put on themselves; see the verdict's own line for the report behind it.
        // "gimmick_equip_" is the whole family those two belong to, and the
        // verdict's line says what taking one of them costs.
        // "effect_gimmick" is the folder of torch flames, lamp lights and
        // campfire sparks. None is in the node table and none has ever paid out,
        // so with Unidentified nodes on they read as nodes nobody has learned
        // yet. A reporter's 1.6.23 log of 16 September 2026 sent 355 gathers to
        // 258 of them in 70 minutes, the same ones again every half minute, all
        // "not ready (node empty)", and the game crashed in its own code ten
        // seconds after the last. Nothing ties the crash to them, but driving an
        // interaction on something that is not loot is how the woodthorn vines
        // broke, so they are never touched.
        static const char* kWords[] = { "visione", "quest", "artifact", "abyssruins", "mission", "puzzle", "woodthorn",
                                        "equip_openclose", "gimmick_equip_", "effect_gimmick" };
        for (const char* w : kWords)
        {
            // The verdict has already decided this piece is not being worn,
            // so the folder entry below would refuse it twice over. This was
            // two copies of one rule and lifting only the first of them cost
            // Sov1737 an evening on 15 September. "equip_openclose" stays in
            // the list whatever happens: those two prefabs are Beloth's helm
            // and cloak and a quest can soft lock on them.
            if (unwornEquip && strcmp(w, "gimmick_equip_") == 0) continue;
            if (IStr(node, w)) return true;
        }
        return false;
    }

    // Reads components, node data and names for one candidate.
    // A gather node reports a 16-bit id that nothing in the shipped data maps
    // to an item, so an unlearned node cannot be named. For one node of each
    // unknown type this dumps the gather block, tries every 16-bit field in it
    // as a row of every static table in the image, and lists every string
    // within two hops of the component. The item block is dumped the same way
    // as a control: its type id is known to be an iteminfo row, so a correct
    // sweep must find it. Debug logging only.
    static std::unordered_set<uint16_t> g_probed;

    static void SweepBlock(const char* what, uintptr_t block, unsigned len)
    {
        uint8_t b[0x40] = {};
        if (len > sizeof b) len = sizeof b;
        if (!mem::ReadBytes(block, b, len)) { LOG("[node]   %s block unreadable", what); return; }
        char hex[3 * sizeof b + 1] = ""; int w = 0;
        for (unsigned i = 0; i < len; ++i) w += snprintf(hex + w, sizeof hex - w, "%02X ", b[i]);
        LOG("[node]   %s block: %s", what, hex);

        const game::TableRef* tables = nullptr;
        const int n = game::EnumTables(&tables);
        int shown = 0;
        for (unsigned off = 0; off + 1 < len && shown < 40; off += 2)
        {
            const uint16_t v = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
            if (v < 64) continue;                       // small values are a row of everything
            int hits = 0;
            for (int i = 0; i < n && hits < 3 && shown < 40; ++i)
            {
                if (v >= tables[i].count) continue;
                char key[96];
                if (!game::KeyInTable(tables[i].global, v, key, sizeof key)) continue;
                ++hits; ++shown;
                LOG("[node]     %s+%X = %u is row of %s (%u rows): \"%s\"", what, off, v,
                    tables[i].name[0] ? tables[i].name : "?", tables[i].count, key);
            }
        }
        if (!shown) LOG("[node]     no field of the %s block is a named row of any table", what);
    }

    static void ProbeNodeIdentity(const Cand& k, uintptr_t inter, uintptr_t idata, uintptr_t gdata)
    {
        if (!inter || !gdata || !k.gtid) return;
        if (g_probed.size() >= 10 || !g_probed.insert(k.gtid).second) return;

        char nodeName[96] = "";
        game::NodeName(inter, nodeName, sizeof nodeName);
        LOG("[node] type %u kind %02X eid %08X %.1f m cat %02X/%02X prefab \"%s\" name \"%s\"",
            k.gtid, k.gkind, k.eid, k.d, k.cat, k.cat2, k.node, nodeName);
        SweepBlock("gather", gdata, 0x40);
        if (idata) SweepBlock("item", idata, 0x20);

        // Anything reachable that spells the node out: a prefab path, a socket
        // name, the gimmick key. Two hops, with the offset that found it, so a
        // hit can be turned into a direct read.
        char seen[1600] = ""; int sw = 0;
        const uintptr_t objs[2] = { inter, gdata };
        const unsigned  lens[2] = { 0x400, 0x80 };
        for (int o = 0; o < 2; ++o)
        {
            if (!mem::Readable(objs[o], lens[o])) continue;
            for (unsigned off = 0; off < lens[o] && sw < 1400; off += 8)
            {
                char buf[160];
                if (mem::ReadEngineString(objs[o] + off, buf, sizeof buf) && strlen(buf) >= 4)
                    sw += snprintf(seen + sw, sizeof seen - sw, " %s+%X=\"%s\"", o ? "g" : "c", off, buf);
                const uintptr_t mid = mem::Deref(objs[o], off);
                if (!mid || mem::InImage(mid) || !mem::Readable(mid, 0x100)) continue;
                for (unsigned off2 = 0; off2 < 0x100 && sw < 1400; off2 += 8)
                    if (mem::ReadEngineString(mid + off2, buf, sizeof buf) && strlen(buf) >= 4)
                        sw += snprintf(seen + sw, sizeof seen - sw, " %s+%X+%X=\"%s\"", o ? "g" : "c", off, off2, buf);
            }
        }
        LOG("[node]   strings:%s", seen[0] ? seen : " (none)");
    }

    static void Fill(Cand& k)
    {
        if (k.filled) return;
        k.filled = true;
        const uintptr_t comps = game::Comps(k.ent);
        if (!comps) return;
        // A thing that points back at the player is the player's own.
        if (g_me && mem::Readable(k.ent, 0x200))
        {
            const uintptr_t meComps = game::Comps(g_me);
            for (unsigned off = 0; off < 0x200; off += 8)
            {
                uintptr_t v = 0;
                if (mem::ReadPtr(k.ent + off, &v) && (v == g_me || (meComps && v == meComps))) { k.mine = true; break; }
            }
        }
        const uintptr_t status = game::CompByClass(comps, kCls_Status);
        if (status)
        {
            mem::Read8(status + kOff_Status_Cat,  &k.cat);
            mem::Read8(status + kOff_Status_Dead, &k.dead);
            mem::Read8(status + kOff_Status_Cat2, &k.cat2);
        }
        k.ai = game::CompByClass(comps, kCls_Ai) != 0;
        const uintptr_t inter = game::CompByClass(comps, kCls_Gimmick);
        k.inter = inter != 0;
        const uintptr_t idata = inter ? mem::Deref(inter, kOff_Gimmick_ItemData) : 0;
        const uintptr_t gdata = inter ? mem::Deref(inter, kOff_Gimmick_GatherData) : 0;
        k.item = idata != 0;
        k.gather = gdata != 0;
        // Who is dressed. An item hanging off something says that something is
        // wearing it; ten planks hanging off a wagon say nothing of the kind.
        // Worn only. Cargo classifies as items just the same, and a pack cow
        // carrying ten crates of equipment and a bolt of silk counted as a
        // person wearing eleven things; it outscored Damiane's body three to
        // one and took the centre. The worn byte is what NoteHolderWorn reads
        // at enumeration, so the two paths now agree on what gear is.
        if (k.item && k.parent && k.cat2 == 0x11) NoteHolderGear(k.parent, g_scanNow);
        if (gdata)
        {
            uint16_t t = 0; if (mem::Read16(gdata, &t)) { k.tid = t; k.gtid = t; }
            mem::Read8(gdata + 5, &k.gkind);
        }
        if (idata)
        {
            uint32_t v = 0;
            if (mem::Read32(idata, &v)) { if (v != 0xFFFFFFFF) k.iid = v; else k.noIid = true; }
            uint16_t t = 0; if (mem::Read16(idata + 8, &t)) k.tid = t;
        }
        if (inter)
        {
            mem::Read8(inter + kOff_Gimmick_Locked, &k.locked);
            // The prefab path names the node and survives a restart; the type
            // id does not, so it is only a fallback. NodeName is kept because
            // a few objects answer on it when the prefab route does not.
            if (!game::NodePrefab(inter, k.node, sizeof k.node))
                game::NodeName(inter, k.node, sizeof k.node);
            // Not gated on k.gather: the prefab path is readable straight away
            // and the gather data is what arming has yet to produce. Waiting for
            // it meant an ore vein was never recognised as ore until after it had
            // already opened, which is the one moment the answer is of no use.
            if (k.node[0]) k.nodeType = NodeDb::ByPrefab(k.node);
            // The well bucket. It grows a gather block once the player is close
            // (none at 14.5 m, one at 1.8 m) and it really does hold Water, but
            // there is no way to take that Water and leave the bucket. Marked a
            // container so nothing downstream tries again. See the note in
            // Decide for what was tried.
            //
            // This used to sit up with the gather read, where k.node is still
            // empty because the prefab is not read until here, so IsMechanism
            // never matched and nothing was ever marked. Harmless so far only
            // because Decide and the arm loop both re-test the prefab
            // themselves, but the comment claimed a marking that never
            // happened, and anything later built on g_containers holding well
            // parts would have quietly not worked.
            if (k.gkind == 0x04 && IsMechanism(k.node)) g_containers.insert(k.eid);
        }
        // Live creatures: which species, from the CharacterInfo row they point at.
        //
        // The narrow gate is the same test that decides whether a creature can
        // be caught at all, which made the log useless for arguing about that
        // test: anything it turned down stayed anonymous, so a report of "the
        // lizards are not picked up" could only ever come back as "creature".
        // Across 41 logs that is 100 creatures carrying a catchable category
        // byte but the wrong type tag, and 352 more the other way about, none
        // of them named. With the debug log on, every live creature standing
        // free of an interaction node is identified, so the log says what is
        // being refused and the gate can be argued from evidence.
        //
        // Costs nothing in normal play, and nothing downstream either: the
        // species only reaches a verdict through the Catch branch, which still
        // needs the narrow gate to be entered at all.
        // The three bytes a live creature can be picked up under. Kept in step
        // with the catchable test in Decide, which needs the species to judge
        // the lizard byte at all.
        // 0x07 and 0x0F were added on 10 September 2026: stag beetles, moths,
        // silkworm moths and spiders wear 0x07 with a non-zero first byte,
        // grasshoppers wear 0x0F, and none of them ever flips to 0x05 in a
        // session. Butterflies wear 0x05 and were the only insects being
        // caught, which is the "intermittent" half of issue #42.
        const bool nameable = k.cat2 == 0x05 || k.cat2 == 0x09 || k.cat2 == 0x08 || k.cat2 == 0x07 || k.cat2 == 0x0F ||
                              (k.cat2 == 0x0C && k.dead == 1); // a corpse: beast or person, decided by the creature table
        if (k.ai && !k.inter && (nameable || g_debugLog))
        {
            const uintptr_t aiComp = game::CompByClass(comps, kCls_Ai);
            const Species sp = FindSpecies(k.eid, k.ent, comps, status, aiComp, k.cat2, k.type);
            k.species = sp.row; k.speciesClass = sp.klass; k.speciesExact = sp.exact;
            // Only for the ones that stayed anonymous, which is the case worth
            // solving, and only with the debug log on.
            if (g_debugLog && !sp.exact) ProbeCreatureIdentity(k.eid, k.ent, comps, status, aiComp, k.cat2, sp.klass);
        }
        if (k.gather && k.gtid)
        {
            if (g_nodeType.size() > 4096) g_nodeType.clear();
            g_nodeType[k.eid] = k.gtid;
            if (g_nodePrefab.size() > 4096) g_nodePrefab.clear();
            if (k.node[0]) g_nodePrefab[k.eid] = k.node;
        }
        if (g_actorEid.size() > 4096) g_actorEid.clear();
        g_actorEid[comps] = k.eid;
        // An empty node close by that will not fill: dump which of its gimmick
        // slots hold pointers, so a node the game fills elsewhere shows up.
        if (g_debugLog && inter && !k.item && !k.gather && k.cat2 == 0x00 && k.d < 20.0f)
        {
            static std::set<std::string> s_dumped;
            const std::string pk = k.node[0] ? k.node : "(no prefab)";
            if (s_dumped.size() < 24 && s_dumped.insert(pk).second)
            {
                // What the entity is made of, which is the question a coin pile
                // has never been asked. A loose item that works carries item
                // data on its gimmick; these carry a gimmick and nothing else,
                // and the component list says whether that is the whole story.
                char comp[600]; int cw = snprintf(comp, sizeof comp, "[probe] empty eid %08X %.1f m cat %02X/%02X components:", k.eid, k.d, k.cat, k.cat2);
                for (unsigned off = 0; off < kComps_SlotsEnd && cw < 520; off += 8)
                {
                    const uintptr_t c = mem::Deref(comps, off);
                    if (!c) continue;
                    const char* n = mem::RttiShort(c);
                    cw += snprintf(comp + cw, sizeof comp - cw, " +%X=%s", off, n ? n : "?");
                }
                LOG("%s", comp);
                LOG("[probe] empty eid %08X prefab %s", k.eid, k.node[0] ? k.node : "(none)");
                char line[1200]; int w = snprintf(line, sizeof line, "[probe] gimmick eid %08X %.1f m slots:", k.eid, k.d);
                for (unsigned off = 0; off < 0x400 && w < 1100; off += 8)
                {
                    uintptr_t v = 0;
                    if (!mem::ReadPtr(inter + off, &v)) { uint64_t raw = 0; if (mem::Read64(inter + off, &raw) && raw) w += snprintf(line + w, sizeof line - w, " +%X=%llX", off, static_cast<unsigned long long>(raw)); continue; }
                    const char* cls = mem::RttiShort(v);
                    w += snprintf(line + w, sizeof line - w, " +%X->%s", off, cls ? cls : "ptr");
                }
                LOG("%s", line);
            }
        }
        if (g_debugLog && k.gather && k.gtid && !k.nodeType && !LearnedYield(k.gtid, k.node)) ProbeNodeIdentity(k, inter, idata, gdata);
        if (k.tid && k.item)
        {
            if (!game::ItemKeyForType(k.tid, k.key, sizeof k.key)) game::GimmickKeyForType(k.tid, k.key, sizeof k.key);
            // Prefer the live key string; fall back to the row mapping only when verified.
            k.db = k.key[0] ? ItemDb::ByStringKey(k.key) : nullptr;
            if (!k.db && game::ItemTableState() == 1) k.db = ItemDb::ByRow(k.tid);
        }
    }

    static Verdict Decide(const Cand& c, const Config& cfg)
    {
        Verdict v;
        v.own = c.mine || IsMine(c.parent);
        auto skip = [&](const char* why) { v.loot = false; v.why = why; return v; };
        if (c.banned || g_searched.count(Key(c)))
        {
            if (c.dead == 1) return skip("corpse already searched");
            if (!c.item && !c.gather) return skip("does not respond");
        }
        // The legacy category byte (cat) is garbage on current builds; only cat2 is used.
        if (c.locked == 1) return skip("locked");
        // (Twin nodes are not skipped here: an empty node next to a filled
        // one is armed like any other and proves itself by filling or not.)
        if (c.d < cfg.minRange) return skip("on the player");
        if (IsMine(c.parent)) return skip("worn or carried by you");
        // And for a while after it stops being carried. A weapon thrown with the
        // Weapon Throw skill comes off the player for as long as it is in the
        // air, so the line above stops covering it at exactly the moment it
        // becomes reachable, and the game is going to put it back in their hand
        // regardless. Taking it in between left them holding two. Whatever else
        // they let go of on purpose answers to the same rule.
        //
        // Deliberately not asking what it is attached to now. Whatever the game
        // hangs a weapon off mid-flight, having been on the player is enough.
        //
        // Creatures are exempt. A crow was seen attached to the player for a
        // moment, in a catch, and this then refused it for the next
        // forty-five seconds. Nothing alive is the player's kit coming back.
        if (!c.ai && WasOnMe(c, GetTickCount())) return skip("yours, just out of your hand");
        // Anything the player dropped out of the bag by hand, for as long as it
        // lies there. The burst key does not lift this; picking it up by hand
        // does, since that ends the object. See HandDrop.
        if (c.item && Dropped(c)) return skip("you dropped this");
        // Category 0x11 is the game saying a thing is being worn, and that is
        // enough by itself. Requiring a parent as well left a hole. Sov1737's
        // log of 14 September 2026 on 1.6.17 refuses his own Black Sun 118
        // times, parent A0100001 and the equip prefab both present, and then
        // hands him three more copies of that same shield from entities with no
        // parent and no prefab at all. With no prefab the /00_common/equip/ rule
        // below cannot fire either, so nothing was left standing between them
        // and Take. Across the archived logs 7,911 lines carry cat 00/11 and not
        // one of them is unparented, so an unparented 0x11 is the scan reaching
        // an entity before the game has filled its parent in, never loot.
        if (c.item && c.cat2 == 0x11) return skip("worn by someone");
        // "already in your bag" used to live here and it could never fire.
        // A world entity's instance id and the ids inside the inventory are
        // not the same numbering, so the lookup was always a miss. That is
        // measured rather than reasoned: the duplicate probe of 15 September
        // 2026 asked the question 133 times with a real non-zero id on the
        // world object and found it in the bag on none of them, and the skip
        // reason itself appears in none of the twelve logs on this machine.
        // It cost a linear walk of up to 2,048 entries for every candidate of
        // every scan to answer no.
        //
        // What it was reaching for is covered: g_done and the recent-spot
        // table stop the same entity being asked for twice. If a real test
        // is ever wanted, it has to come from something both sides share,
        // and the instance id is not it.
        // Set when the worn-gear folder let this one through on its
        // category, read only by OffLimits below, which carries the same
        // prefix in its own list.
        bool unwornEquip = false;
        if (c.node[0])
        {
            if (IStr(c.node, "visione") || IStr(c.node, "quest") || IStr(c.node, "artifact")) return skip("quest or memory trigger");
            if (IStr(c.node, "abyssruins")) return skip("fast-travel artifact");
            // A piece of gear the game wants worn, not pocketed. LuxDragon on
            // Discord, 11 September 2026: Beloth, the Darksworn's helmet has to
            // be taken by hand or the fight can soft lock and the only way out
            // is an older save. His log names it: Ferman Plate Helm, no parent,
            // from gimmick_equip_openclose_helm. The item itself is an ordinary
            // tier-2 helm carrying no flag at all, so nothing on the Classes or
            // Tags tabs could ever have caught it; the prefab is the only thing
            // that says this one is different. Two prefabs in the whole of
            // gimmickinfo's 13,906 rows are built this way, the helm and the
            // cloak, and both hold an open and shut state the game drives
            // through the interaction. Taking one as loose loot skips that.
            if (IStr(c.node, "equip_openclose")) return skip("take this one by hand, looting it can lock the quest");
            if (PlantedSeed(c.node)) return skip("a seed planted in your camp farm");
            if (PlacedCookFire(c.node)) return skip("a placed Field Pot or Bonfire");
            // Gear that is being worn, which the game keeps in the world as an
            // object of its own under /00_common/equip/. Taking one hands over a
            // copy and leaves the original equipped, so the player ends up with
            // the same sword several times over and a save that says so.
            // mrbryan23 reported it on the bugs tab on 12 September 2026 with a
            // picture of the duplicates and the log beside them: two pick-ups of
            // a Tauria Curved Sword four minutes apart, both from
            // gimmick_equip_onehandsword. The family is 131 prefabs and the
            // reporter's three cases are all in it, gimmick_equip_onehandsword,
            // gimmick_equip_shield and gimmick_equip_hexe_marie_earring.
            //
            // Nothing legitimate is lost. Gear that has actually been dropped
            // reaches the ground as /00_common/item/gimmick_item_basic_equip and
            // its cousins, which this does not touch; the same log shows a
            // Hartmann Plate Helm picked up that way in the same minute. The
            // prefix appears nowhere outside that folder, so it is the folder.
            //
            // The folder, and not the parent, and that was tested rather than
            // assumed. A log of 13 September 2026 refused 82 of these in one
            // session with no parent on any of them, which read as though the
            // rule were catching gear nobody was wearing. Sov ran a build that
            // let the unparented ones through: 27 were taken and his own axe
            // and a shield duplicated in his bag. The same log settles why. One
            // pair of Strongbow Gloves was refused 53 times as worn, each with
            // a real parent and cat2 0x11, while four other instances of that
            // same item came through with parent 0 and cat2 0x19, at one to
            // four metres. Gloves are not loot lying on the ground. An
            // unresolved parent is not an absent wearer, so the parent cannot
            // carry this test and the folder has to.
            if (IStr(c.node, "gimmick_equip_"))
            {
                // Issue #73, settled on 15 September 2026, and the category is
                // the rule now rather than the folder.
                //
                // Across 1,944 sightings of this folder in three of
                // Sov1737's sessions the split is total: every one of the 1,885
                // at cat2 0x11 has a wearer recorded, and not one of the 59 at
                // 0x00, 0x0F or 0x19 ever does, at any range or at any moment in
                // the session. 301 of these entities were tracked from first
                // sighting to last and not one ever gained a wearer or changed
                // its category, so "the game had not handed the link over yet"
                // does not survive the data.
                //
                // Sov's own reading of it, unprompted: the ones left behind are
                // a shield on an armour stand and swords stuck in the ground.
                // Scenery built from the same prefabs as real gear.
                //
                // Measured rather than argued. The duplicate probe read the bag
                // at both ends of every loot in a bandit camp: 154 measurements,
                // no copies. Every piece left the world and added exactly one to
                // the bag. Three Bekker Shields came out of that camp on three
                // entity ids with the bag climbing 0, 1, 2, 3, which is what
                // three shields look like and not what one shield coming back
                // looks like. Two of them wore cat2 0x16 and behaved like the
                // rest.
                //
                // 0x11 stays refused because every one of the 6,194 sightings of
                // it that could be checked had a wearer.
                //
                // 0x19 is taken since 1.6.24, issue #69. It is a weapon left on
                // the ground after its wielder died or was disarmed: in a fight
                // on 16 September 2026 seventeen of them landed in this folder
                // at 0x19, unparented, seconds after the kills, and every one was
                // refused as worn. A test build let 0x19 through with the dupe
                // probe reading the bag either side of each pickup, across several
                // bandit camps: 16 pickups, all "moved, not copied", covering
                // swords, axes, maces, bows, shields and a mask, and 59 clean
                // readings in the session with no copy. It had only been refused
                // on a report that Strongbow Gloves duplicated at 0x19 on 13
                // September, from a log that is not on this machine. No gloves
                // dropped in the test, so gloves are the one kind unmeasured.
                //
                // 0x01 joined them on 15 September 2026, and it is the one
                // byte here chosen from a single reporter rather than from a
                // population. docwat232's Spada Sword is taken off a stand
                // during a quest, and taking it any other way leaves the quest
                // with a step that can no longer happen; his own log shows
                // 1.6.5 gathering it. In eighteen logs 0x01 appears inside this
                // folder exactly twice, both of them that sword. So the whole
                // byte costs 2 of the 569 sightings the narrowed rule lets
                // through, and the two are the ones that break something.
                //
                // Frostfang and the Plate Helm of the Shadows, which are what
                // issue #57 is actually about, were never at risk: every one of
                // the 96 sightings of a stand prefab across these logs reads
                // 0x11 with a real wearer, so 1.6.21 never reached them. This
                // is the neighbouring case, not that one.
                //
                // The obvious wider rule was measured first and would have been
                // a disaster. All four quest pieces carry the tag "docking",
                // and so do 563 of the 567 folder sightings at the allowed
                // bytes, Bekker Shields and Glenmore Swords included. Refusing
                // that tag here would have undone issue #73 the day after it
                // shipped.
                //
                // EquipStrict in the ini brings the whole folder back for anyone
                // whose game disagrees, so a report does not have to wait for a
                // build.
                if (cfg.equipStrict || c.cat2 == 0x11 || c.cat2 == 0x01)
                    return skip("someone is wearing this; taking it would copy it");
                unwornEquip = true;
            }
            if (IStr(c.node, "mission")) return skip("mission object");
            if (AttachedPart(c.node, c.nodeType)) return skip("part of a creature or a mechanism");
            // Whatever arming refuses, the verdict refuses too. This was a
            // hand-copied list and it had drifted: "puzzle" was in OffLimits
            // and missing here. Five puzzle prefabs are tagged ore in the node
            // table (ice_block_break, two ice_wall_break, pickaxe_break_point,
            // stone_wall_break), and because OffLimits stops them being armed
            // they never fill, fall through to the ore branch below, and were
            // reaching the vein break. The mod was swinging at an ice wall.
            if (OffLimits(c.node, unwornEquip)) return skip("puzzle or protected mechanism");
            // Kept in step with OffLimits(), which stops arming touching the same
            // things. The words above are the detail this one summarises.
            // These read the prefab path, so a gather node whose name happens to
            // carry one of the words is not a container: cd_box_mushroom_02 is a
            // plant. A node the table has already classified keeps its kind.
            // The row does not excuse the name. This was written as
            // `!c.nodeType` alone, so the folder rule's 540 container rows
            // switched the test off for the 164 prefabs it was aimed at: a
            // dropset food box arrives carrying the item data of the apple
            // inside it, takes the Take path on c.item, and its class decides,
            // which is Crop and on. It read as container (off) until the row
            // existed. Both sources have to say container for the name to be
            // ignored, and all 164 of them do; the four that had a row at HEAD
            // are three wood and one plant, which is the case the comment
            // below is about and they stay exempt.
            if (!c.nodeType || c.nodeType->kind == "container")
            {
                // Only a last resort, for a node holding nothing the database
                // can name. Anything identified is decided by its class below,
                // which is what catches the breakable tables and chairs whose
                // prefab path never says furniture.
                const bool container = IStr(c.node, "_chest") || IStr(c.node, "_box") || IStr(c.node, "dropset");
                const bool furniture = IStr(c.node, "furniture");
                if (container && !cfg.lootContainers) return skip("container (off)");
                if (furniture && !cfg.lootFurniture)  return skip("furniture node (off)");
            }
        }
        // A well is refused whole, and it is a measured refusal rather than a
        // guess about scenery. Six of its seven parts never fill. The seventh is
        // the bucket, it does fill, and it does pay Water.
        //
        // Both ways of asking take the bucket with the water. Loot travels on
        // one descriptor, 0x0809, with a mode byte at payload+3: 0x05 gathers
        // and 0 picks up. Gathering was tried on 2026-09-08 and the bucket
        // vanished; picking up was tried straight after, with the bucket already
        // wound up full by hand, and the bucket vanished again. The mode is not
        // the difference.
        //
        // Drawing by hand raises no loot event at all, only a sequence of state
        // transitions, so the game's own way of emptying a bucket does not go
        // through this descriptor. Replaying that sequence is the only path
        // left and it is eleven timed transitions across parts that drive each
        // other, for a three copper item that pottery drops anyway. See #23.
        if (IsMechanism(c.node) || g_containers.count(c.eid)) return skip("mechanism part");
        if (c.heap) return skip("stack at one point (storage contents)");
        // (A pointer to the player inside the object used to mean "yours"; the
        // parent and bag checks above cover that, and arming can plant such a
        // pointer in a node we just touched.)

        // What can be picked up alive, decided on the category byte alone. 05 and
        // 09 are the insects and the small things; 08 is the ground lizards,
        // which were refused outright until now and are why iguanas, chameleons
        // and the two lizards were never caught however the switches were set.
        //
        // The type tag used to be required to be 06 and is not consulted at all
        // now. It separates nothing: the same insect appears at 03 and at 06 in
        // one session, three metres apart, and the pair that were 03 were being
        // silently refused. What keeps people and livestock out is the byte, not
        // the tag. NPCs are 0A and beasts are 0C, and neither is listed here.
        //
        // The lizard byte is held to a higher bar than the other two, because it
        // has not been mapped the way they have and a goat could yet turn up
        // wearing it. The creature table has to name the thing outright, not
        // offer a relative of it, and it has to be something that becomes an
        // item: no item row, nothing to catch, so leave it alone.
        const bool smallGame    = c.cat2 == 0x09 || c.cat2 == 0x05;
        // Issue #78. The byte used to decide this and it cannot: one species
        // does not wear one byte. A Black-Naped Oriole is at 07, at 01 and at
        // 08 across the logs here and was reachable only at 08, refused as
        // "creature" at the other two before any switch was asked. Blue Jay
        // turns up at 01, 07 and 0C. Sparrow, Meadow Bunting, Three-Toed
        // Woodpecker, White-Winged Redstart, Burrowing Toad and Northern Pike
        // all sit in the refused pile carrying item rows, and the mod page
        // promises birds under Small animals.
        //
        // So the test is the bar the insects already used, minus the byte: the
        // creature table has to name the thing outright and it has to become an
        // item. That is safe at any byte because the item row is itself the
        // discriminator. Of the table's 1004 rows, 165 carry an item row and not
        // one is a predator or livestock; the game gives an item row to what you
        // can catch and pocket. 0x0C is the byte that would otherwise be
        // alarming, being what bandits and beasts wear, and a bandit has no
        // species row while a wolf has no item row.
        //
        // Counted across sixteen logs before it was written. At every byte the
        // bar admits toads, frogs, flies, spiders, snails, squirrels, chipmunks,
        // salamanders, four kinds of bird, two beetles, a pike and an iguana,
        // and nothing else. No horse, cow, wolf, bandit or guard matches it.
        //
        // This subsumes the old groundLizard and insectByName, which were this
        // same test with a byte list bolted on. smallGame keeps its byte-only
        // path: 0x05 and 0x09 are mapped and admit things the table does not
        // always name, so dropping it would lose catches.
        const bool namedGame = c.speciesExact && c.species && c.species->itemRow >= 0;
        const bool catchable = (smallGame || namedGame) && !c.inter;
        const bool beastCorpse = c.dead == 1 && (c.cat2 == 0x0C || c.ai);
        if (c.dead == 1 && !beastCorpse) return skip("corpse: loot drops separately");
        if (!catchable && c.dead != 1 && !c.inter && c.ai) return skip("creature");
        if (!catchable && c.dead != 1 && !c.inter) return skip("no interaction node");

        // Mine ore veins for you, off, leaves every vein to the pickaxe. The
        // arm loop has always honoured it, but a vein the player walks up to
        // fills on its own, took the plain gather branch below, and was broken
        // for them at the lower drop. rokugin, 19 September 2026. The chunks a
        // vein throws are not veins by the table's breaks column, so they are
        // still picked up.
        if (!cfg.gatherVeins && c.nodeType && c.nodeType->tagged &&
            KindFromName(c.nodeType->kind) == GatherKind::Ore && c.nodeType->breaks && !NotAVein(c.node))
            return skip("an ore vein, left to your pickaxe while Mine ore veins for you is off");

        if (beastCorpse)     v.act = Action::Search;
        else if (catchable)  v.act = Action::Catch;
        else if (c.gather)   v.act = Action::Gather;
        else if (c.item)     v.act = Action::Take;
        // A vein the table names as ore does not have to answer first. The
        // gather event carries nothing but the target's id (see the payload
        // built in events.cpp), so there was never anything to wait for; the
        // wait was this function needing data to pick an action and to judge
        // the yield, and for a node the game itself tags as ore neither is in
        // question. Worst case the game ignores an event aimed at something
        // it will not open, which is what happened anyway while we waited.
        else if (cfg.gatherVeins && c.nodeType && c.nodeType->tagged &&
                 KindFromName(c.nodeType->kind) == GatherKind::Ore)
            v.act = Action::Gather;
        // And a node the table vouches for as an item does not have to answer
        // either, for the same reason: the pick-up event carries nothing but
        // the target's id. These never answer arming at all, and the game's own
        // data says why. interactioninfo has two families, Gimmick_PickUp at
        // rows 14 to 25 and Gimmick_Collect at 28 to 34, and arming is the
        // collect one: every arm that has ever filled in any log here came back
        // as a gather node, and the gold bars, the loose coins, the firewood
        // and the small stones are pick-ups. Arming them was the wrong verb, so
        // it did nothing however close the player stood. LuxDragon, 13
        // September 2026: forty-one arms across one hoard, one of them at half
        // a metre, not one fill.
        else if (c.nodeType && c.nodeType->tagged && c.nodeType->pickup)
        {
            v.act = Action::Take;
            v.nodeReach = true;
        }
        // A plant the game only gives up through its own state event. It never
        // fills, so every other branch above has already declined it.
        else if (StatePick(c.nodeType))
        {
            v.act = Action::Gather;
            v.pick = true;
        }
        else return skip("not ready (node empty)");

        // Item rules from the database. A live key that our table knows gets the
        // full class/tag/item verdict; a node whose yield has been learned gets
        // the same verdict on the yield; unknown names fall back to name checks.
        // Search is in this list, and it closes less than the issue title
        // suggests. A carcass has no interaction gimmick, so idata and gdata
        // are both null, c.tid is zero, and the `&& c.tid` gates the whole
        // block out for one regardless. What it does close is the entity that
        // is flagged dead and still carries real item identity: the
        // beastCorpse test above wins the else-if chain ahead of c.item, so
        // such a thing was routed to Search and lost every rule on the way.
        //
        // It does not filter what skinning pays out, and nothing here can. The
        // Search event carries the carcass eid and nothing else. The yield
        // never exists as an entity this scan could see, and there is no
        // discard descriptor to shed one after it lands. The Carcasses switch
        // is the only control over that, which is now what the menu and the
        // README say instead of promising a filter. Issue #33.
        if ((v.act == Action::Take || v.act == Action::Gather || v.act == Action::Search) && c.tid)
        {
            const Item* ruled = c.db ? c.db : (v.act == Action::Gather ? NodeYield(c) : nullptr);
            if (ruled)
            {
                const Rules::Verdict r = Rules::Decide(*ruled, cfg);
                if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
            }
            else if (c.key[0])
            {
                static const char* forbidden[] = { "quest", "artifact", "sealed_", "_seal", "recipe", "puzzle", "_core", "abysscore", "visione", "abyssgear" };
                for (const char* f : forbidden) if (IStr(c.key, f)) return skip("protected item name");
                if (v.act == Action::Take && !cfg.takeUnknownItems) return skip("not in item database");
            }
            else if (v.act == Action::Take && !cfg.takeUnknownItems) return skip("unnamed item");
        }

        switch (v.act)
        {
        case Action::Search:
            // Bandits and beasts wear the same category byte, 0x0C, and the
            // game loots both with one action. The creature table tells them
            // apart: a beast's character row is in it, a person's is not
            // (fourteen bandits in one fight on 10 September 2026, none named).
            if (c.species) { if (!cfg.lootCorpses)  return skip("carcasses off"); }
            else           { if (!cfg.searchBodies) return skip("bodies off"); }
            break;
        case Action::Catch:
        {
            if (c.speciesClass)
            {
                const std::string cl = c.speciesClass;
                if (cl == "insect")                       { if (!cfg.catchInsects) return skip("insects off"); }
                else if (cl == "fish" || cl == "seafood") { if (!cfg.catchFish)    return skip("fish off"); }
                else                                      { if (!cfg.catchAnimals) return skip("small animals off"); }
                if (const Item* it = c.species ? ItemDb::ByRow(c.species->itemRow) : nullptr)
                {
                    const Rules::Verdict r = Rules::Decide(*it, cfg);
                    if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
                }
                else
                {
                    // No item to judge, so the class the creature table gives
                    // it answers instead. A Firefly Colony has no item row and
                    // went into the bag with the insect class refused, Sov1737
                    // on 24 September 2026, because only the item was ever
                    // asked. The table's five classes, insect, fish, seafood,
                    // animal and amphibian, are all item classes as well, so
                    // this is the same switch on the Classes tab that refuses
                    // a Firefly. A tag or item rule still has nothing to read.
                    const Rules::Verdict r = Rules::DecideClass(cl, cfg);
                    if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
                }
            }
            else if (c.cat2 == 0x05) { if (!cfg.catchFish || !cfg.catchInsects || !cfg.catchAnimals) return skip("unidentified: could be a fish, a flying insect or a bird"); }
            else                     { if (!cfg.catchInsects || !cfg.catchAnimals) return skip("unidentified: could be an insect or a small animal"); }
            break;
        }
        case Action::Gather:
        {
            const GatherKind kind = NodeKind(c);
            if (const char* off = GatherSwitchOff(kind, cfg)) return skip(off);
            if (const char* tree = TreeUnsafe(kind, c.d)) return skip(tree);
            const Item* yield = c.db ? c.db : NodeYield(c);
            // The item-rule block above is gated on c.tid, and a vein the table
            // vouches for is routed to Gather before it has answered, when tid
            // is still zero and the gather data has not arrived. Every one of
            // the nineteen ore prefabs whose yield the table names is that
            // kind, and nine of them pay stone or a stalactite, so a player who
            // refused the stone class walked up to gimmick_quarry_stone_0001
            // and got stone: the yield was named all along and nothing ever
            // asked a rule about it. Ask here, where the answer is the same one
            // the block above would have given.
            // Not "when tid is zero". The block above runs only when c.tid and
            // c.db are both set, and c.db is filled nowhere else, so a node with
            // a live tid whose row the item table cannot resolve was judged by
            // nothing at all and then fell through this test as well.
            // symplexity's mine rock carries type 50875, which is a gather type
            // rather than an item row, so ItemDb::ByRow returned null and both
            // halves declined to look at it. An exemption written as "the block
            // above already judged this" is a claim about control flow, and this
            // one was false.
            if (yield && !c.db)
            {
                const Rules::Verdict r = Rules::Decide(*yield, cfg);
                if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
            }
            // And a node that names nothing it holds has no item rule to be
            // asked about it at all. Ask what the gimmick row says it can hand
            // over, then fall back on its kind, or a class the player refused
            // reaches the bag through the node it came out of.
            if (!yield)
            {
                if (const char* none = TableYieldRefused(c, cfg)) return skip(none);
                if (const char* none = KindRefused(kind)) return skip(none);
            }
            break;
        }
        default:
        {
            // Ground items owns a thing lying loose. A node the table reached
            // for owns its own switch instead, because it has a kind of its
            // own and the player set that switch meaning it. Firewood is the
            // case: it is reached with the pick-up verb now, and it is still
            // wood, so turning Wood off has to stop it. Issue #63. For an
            // ordinary pick-up the kind is Item and this asks Ground items,
            // which is the same question it asked before.
            if (v.nodeReach) { if (const char* off = GatherSwitchOff(NodeKind(c), cfg)) return skip(off); }
            else if (!cfg.pickUpItems) return skip("pick up off");
            // A node the table vouches for arrives with no item of its own, so
            // c.tid is zero, the item-rule block above is gated out, and the
            // switch below reads a null database row. Both were holes: an item
            // the player set to never was honoured when the same thing came out
            // of a gather node and taken here, and a dry apple went into the
            // bag with Crops off. Ask the same questions the Gather case asks,
            // in the same order, and let the kind come off the node.
            if (v.nodeReach)
            {
                const Item* yield = NodeYield(c);
                if (yield)
                {
                    const Rules::Verdict r = Rules::Decide(*yield, cfg);
                    if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
                }
                else
                {
                    if (const char* none = TableYieldRefused(c, cfg)) return skip(none);
                    if (const char* none = KindRefused(NodeKind(c))) return skip(none);
                    // A thing the game drives through states and triggers, that
                    // nothing can be asked about. Twenty-eight prefabs: the
                    // Demeniss knowledge tower's monument, globe, telescope and
                    // quill, both Marni EMP capsule parts, the troll tower
                    // cubes, the laser safe buttons, three kinetic tools, four
                    // bombs and a trap. Sending Take at one of these reached
                    // past every rule the mod has, because none of them has
                    // item identity for the block above to work on, and a
                    // longer list of names in OffLimits would only cover the
                    // ones somebody had already walked into. Issue #41's shape.
                    if (c.nodeType->driven) return skip(kReason_NodeMechanism);
                    // Deliberately not gated on takeUnknownItems, though a
                    // review found that setting unenforced here and the help
                    // text ("leave anything unidentified") reads as though it
                    // should be. It was enforced on 13 September 2026 and taken
                    // back out the same evening: 366 of the 492 pick-up rows
                    // name no item, so the gate refused coins, gold bars and
                    // most of the rest, and LuxDragon's next run picked up
                    // nothing at all.
                    //
                    // The switch has never reached a gather node or a node the
                    // table vouches for. Anyone who turned it off did so to
                    // keep unnamed junk out of the bag, and it has never cost
                    // them a coin, so enforcing it here widens a decision they
                    // already made without being asked. A prefab the game tags
                    // as a pick-up is identified; the item inside it is what
                    // has no name, and that is a different question. If this is
                    // ever revisited, it wants its own switch rather than this
                    // one.
                }
            }
            // Ore, stone and wood reach the ground as drops from broken nodes;
            // the same toggles cover the chunks. Furniture reaches it by being
            // smashed, and a table is furniture whether it is still standing or
            // lying in pieces, so it answers to the same switch either way.
            // NodeKind for a vouched pick-up, KindOf for everything else. The
            // two agree wherever an item row exists; where one does not,
            // KindOf(nullptr) is Unknown and every case below falls through,
            // which is how thirteen cloth rows that yield furniture and four
            // dried crops walked past their own switches. A node whose yield
            // is still unknown stays Item and is unaffected, so nothing that
            // the pick-up fix reached stops working.
            switch (v.nodeReach ? NodeKind(c) : KindOf(c.db, true))
            {
            case GatherKind::Plant: if (!cfg.gatherPlants) return skip("plants off"); break;
            case GatherKind::Crop:  if (!cfg.gatherCrops)  return skip("crops off"); break;
            case GatherKind::Ore:
            case GatherKind::Stone: if (!cfg.gatherOre)   return skip("ore off"); break;
            case GatherKind::Wood:  if (!cfg.gatherWood)  return skip("wood off"); break;
            case GatherKind::Furniture: if (!cfg.lootFurniture) return skip("furniture off"); break;
            case GatherKind::Container: if (!cfg.lootContainers) return skip("containers off"); break;
            default: break;
            }
            break;
        }
        }
        const float lim = v.act == Action::Search ? cfg.corpseRange : v.act == Action::Catch ? cfg.catchRange
                        : v.act == Action::Gather || v.nodeReach ? cfg.gatherRange : cfg.lootRange;
        if (lim > 0 && c.d > lim) return skip("out of range");

        if (!cfg.lootOwned && v.act != Action::Catch)
        {
            const int steal = WouldStealCached(c);
            if (steal == 1)  return skip("owned by someone (theft)");
            // Not "owner unknown": nothing at all can be taken yet. The game
            // has to run its own ownership check once before the mod can ask
            // it anything, and interacting with anything makes that happen.
            if (steal == -1) return skip("waiting for the game to check ownership once");
        }
        v.loot = true;
        v.why = events::ActionName(v.act);
        return v;
    }

    static const char* Label(const Cand& c)
    {
        if (c.db) return c.db->Label();
        // The prefab table names a node whether or not it has answered yet.
        // This used to sit inside the c.gather branch below, so a vein taken
        // without waiting printed its entire path instead of its name.
        if (c.nodeType)
        {
            static char named[96];
            snprintf(named, sizeof named, "%s node", c.nodeType->name.c_str());
            return named;
        }
        if (c.gather)
        {
            static char buf[96];
            if (const Item* y = LearnedYield(c.gtid, c.node)) { snprintf(buf, sizeof buf, "%s node", y->Label()); return buf; }
            if (c.node[0]) return c.node;
            if (c.tid) { snprintf(buf, sizeof buf, "node type %u", c.tid); return buf; }
        }
        if (c.key[0]) return c.key;
        if (c.node[0]) return c.node;
        if (c.dead == 1) return "corpse";
        if (c.species && c.speciesExact) return c.species->name.c_str();
        if (c.speciesClass) return c.speciesClass;
        if (c.ai) return "creature";
        return c.inter ? "object" : "entity";
    }

    // Probes the manager for {count, capacity, array} triples and visits every
    // entity pointer in them.
    static bool EntityLike(uintptr_t e)
    {
        uint32_t id = 0;
        if (!mem::Readable(e, 0x100) || !game::Eid(e, &id) || !id) return false;
        const uint8_t tag = static_cast<uint8_t>(id >> 24);
        return tag == game::kTagPlayer || tag == game::kTagWorld;
    }
    template <typename F>
    static void ForEachEntity(uintptr_t mgr, F&& fn)
    {
        for (unsigned off = kOff_Mgr_ListsBegin; off + 16 <= kOff_Mgr_ListsEnd; off += 8)
        {
            uint32_t count = 0, cap = 0; uintptr_t arr = 0;
            if (!mem::Read32(mgr + off, &count) || !mem::Read32(mgr + off + 4, &cap) || !mem::ReadPtr(mgr + off + 8, &arr)) continue;
            if (!count || !cap || count > cap || cap > 0x10000) continue;
            if (!mem::Readable(arr, 8)) continue;
            for (uint32_t i = 0; i < count && i < 4000; ++i)
            {
                uintptr_t e = 0;
                if (!mem::ReadPtr(arr + 8ull * i, &e)) break;
                if (!mem::Readable(e, 0x100)) continue;
                if (!fn(e)) return;
            }
        }
        // The lists above hold one entity or none on most ticks, and for a
        // long time the mod believed that was all the manager offered: a
        // few entities a tick, a burst of hundreds now and then, and a
        // one-second window to remember them. The world is in fact kept in
        // a dozen pools between +0x128 and +0x2E0, each a pointer to an
        // array of entity pointers followed by a packed count and capacity
        // word, found by the roster probe on 2760. The count in that word
        // is not the live count (it read zero over 246 entities), so this
        // takes no count from anywhere: from the lowest such pointer it
        // walks forward while the entries are entities, gives up after
        // sixteen that are not, and skips every other pointer inside a run
        // it has already covered. Entities freed and not yet reused leave
        // holes, which the sixteen absorb.
        uintptr_t runs[24]; int runN = 0;
        for (unsigned off = 0x100; off + 8 <= 0x300 && runN < 24; off += 8)
        {
            uintptr_t arr = 0;
            if (!mem::ReadPtr(mgr + off, &arr) || !mem::Readable(arr, 8ull * 4)) continue;
            bool ok = true;
            for (uint32_t i = 0; i < 4 && ok; ++i) { uintptr_t e = 0; ok = mem::ReadPtr(arr + 8ull * i, &e) && EntityLike(e); }
            if (ok) runs[runN++] = arr;
        }
        std::sort(runs, runs + runN);
        uintptr_t coveredTo = 0;
        for (int r = 0; r < runN; ++r)
        {
            if (runs[r] < coveredTo) continue;
            uintptr_t at = runs[r]; int misses = 0;
            for (uint32_t i = 0; i < 8000; ++i, at += 8)
            {
                uintptr_t e = 0;
                if (!mem::ReadPtr(at, &e) || !EntityLike(e)) { if (++misses >= 16) break; continue; }
                misses = 0;
                if (!fn(e)) return;
            }
            coveredTo = at;
        }
    }

    // ---------------------------------------------------------------- scan ----
    // Wind a well and take what comes up. Started when a bucket is in reach and
    // pumped once a scan; the run is eleven seconds of transitions, so it plays
    // out across many scans rather than in one.
    static void WellTick(const std::vector<Cand>& list, const Config& cfg, DWORD now)
    {
        if (g_wellRun.active)
        {
            const DWORD since = now - g_wellRun.startedAt;
            while (g_wellRun.step < kWellSteps && kWellTake[g_wellRun.step].atMs <= since)
            {
                const WellStep& st = kWellTake[g_wellRun.step];
                if (const uintptr_t comp = g_wellRun.comp[st.part])
                    events::DriveEvent(comp, st.ev, g_meEid, g_me, g_wellRun.eid[st.part]);
                ++g_wellRun.step;
            }
            if (g_wellRun.step >= kWellSteps)
            {
                LOG("[well] took from bucket %08X; winch is in state %08X",
                    g_wellRun.eid[WellBucket], GimmickState(g_wellRun.comp[WellWinch]));
                g_wellDone[g_wellRun.eid[WellBucket]] = now;
                g_wellRun.active = false;
            }
            return;
        }
        if (!cfg.drawWells || !g_me) return;
        if (g_wellDone.size() > 256) g_wellDone.clear();

        for (const Cand& b : list)
        {
            // The bucket, by prefab. parts01 is the one that grows a gather
            // block, and the gather block is how we know it has something in it.
            if (!b.filled || !b.node[0] || !IStr(b.node, "/well/")) continue;
            if (!IStr(b.node, "parts01")) continue;
            if (b.d > (cfg.gatherRange > 0 ? cfg.gatherRange : 12.0f)) continue;
            const auto done = g_wellDone.find(b.eid);
            if (done != g_wellDone.end() && now - done->second < kWellCooldownMs) continue;

            // Its siblings. The winch shares the bucket's parent; the winch part
            // hangs off the winch. "parts02.prefab" rather than "parts02", or it
            // matches parts02_part as well.
            const Cand* winch = nullptr;
            const Cand* winchPart = nullptr;
            for (const Cand& o : list)
                if (o.node[0] && o.parent == b.parent && IStr(o.node, "parts02.prefab")) { winch = &o; break; }
            if (!winch) continue;
            for (const Cand& o : list)
                if (o.node[0] && o.parent == winch->eid && IStr(o.node, "parts02_part")) { winchPart = &o; break; }

            WellRun r;
            const Cand* part[3] = { winch, winchPart, &b };
            for (int i = 0; i < 3; ++i)
            {
                if (!part[i]) continue;
                r.eid[i] = part[i]->eid;
                const uintptr_t comps = game::Comps(part[i]->ent);
                r.comp[i] = comps ? game::CompByClass(comps, kCls_Gimmick) : 0;
            }
            if (!r.comp[WellWinch]) continue;   // nothing to drive

            // The winch is read but never driven, so the log can say what the
            // player was doing when the water was taken. MinAngle means they had
            // hold of the handle, and taking the bucket's contents while they do
            // is the whole point rather than something to avoid.
            const uint32_t st = GimmickState(r.comp[WellWinch]);
            r.startedAt = now;
            r.step = 0;
            r.active = true;
            g_wellRun = r;
            LOG("[well] taking from bucket %08X at %.1f m (winch %08X is in state %08X)",
                b.eid, b.d, r.eid[WellWinch], st);
            break;
        }
    }

    // Where a fault inside the scan landed, so the log names it instead of the
    // process disappearing. Until now the only guarded code in the whole plugin
    // was DrawOverlay, which meant an access violation anywhere in the loot
    // engine killed the game with an empty log and nothing to go on: the last
    // line written would be whatever ran just before, which reads like a clean
    // shutdown and is not one.
    static LONG ScanFaultFilter(unsigned code, EXCEPTION_POINTERS* xp)
    {
        void* at = (xp && xp->ExceptionRecord) ? xp->ExceptionRecord->ExceptionAddress : nullptr;
        char mod[MAX_PATH] = "unknown";
        uintptr_t off = 0;
        HMODULE h = nullptr;
        if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     static_cast<LPCSTR>(at), &h) && h)
        {
            char full[MAX_PATH] = "";
            if (GetModuleFileNameA(h, full, MAX_PATH))
            {
                const char* slash = strrchr(full, '\\');
                strncpy(mod, slash ? slash + 1 : full, sizeof mod - 1);
                mod[sizeof mod - 1] = 0;
            }
            off = reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(h);
        }
        // An access violation carries the address it tried to touch and whether
        // it was reading or writing, which is most of the diagnosis.
        char where[160] = "";
        if (xp && xp->ExceptionRecord &&
            xp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            xp->ExceptionRecord->NumberParameters >= 2)
        {
            const ULONG_PTR kind = xp->ExceptionRecord->ExceptionInformation[0];
            snprintf(where, sizeof where, " %s %p",
                     kind == 0 ? "reading" : (kind == 1 ? "writing" : "executing"),
                     reinterpret_cast<void*>(xp->ExceptionRecord->ExceptionInformation[1]));
        }
        LOG_ERR("[scan] fault 0x%08X at %p (%s+0x%llX)%s - looting is off for this session, the game keeps running.",
                code, at, mod, static_cast<unsigned long long>(off), where);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    static bool g_scanDisabled = false;

    static void Scan(const Config& cfg, bool act, bool burst);

    // No C++ objects in this frame, so __try is allowed here where it is not
    // allowed inside Scan itself. One fault turns looting off rather than
    // letting the same one repeat thirty times a second.
    static void ScanGuarded(const Config& cfg, bool act, bool burst)
    {
        __try
        {
            Scan(cfg, act, burst);
        }
        __except (ScanFaultFilter(GetExceptionCode(), GetExceptionInformation()))
        {
            g_scanDisabled = true;
        }
    }

    // The pools ForEachEntity walks, as the log can show them: each one's
    // length, what it holds and how much of it stands within forty metres,
    // with the raw words round the pointer. Verbose log, twice a session.
    // This is what found them, and on a new build it is the first thing to
    // read when the Nearby list goes back to flipping.
    static void RosterProbe(uintptr_t mgr, DWORD now, const Vec3& mp)
    {
        static DWORD s_last = 0; static int s_runs = 0;
        if (s_runs >= 2 || (s_last && now - s_last < 60000)) return;
        if (!s_last) { s_last = now - 40000; return; }   // first run twenty seconds in
        s_last = now; ++s_runs;
        int lines = 0;
        for (unsigned off = 0x100; off + 8 <= 0x300 && lines < 40; off += 8)
        {
            uintptr_t arr = 0;
            if (!mem::ReadPtr(mgr + off, &arr) || !mem::Readable(arr, 32)) continue;
            bool ok = true;
            for (uint32_t i = 0; i < 4 && ok; ++i) { uintptr_t e = 0; ok = mem::ReadPtr(arr + 8ull * i, &e) && EntityLike(e); }
            if (!ok) continue;
            uint64_t w[6] = {};
            for (int i = 0; i < 6; ++i) mem::Read64(mgr + off - 16 + 8ull * i, &w[i]);
            int ents = 0, a0 = 0, b0 = 0, near40 = 0, withPos = 0, misses = 0; uint32_t len = 0;
            uintptr_t at = arr;
            for (uint32_t i = 0; i < 8000; ++i, at += 8)
            {
                uintptr_t e = 0; uint32_t id = 0; Vec3 q;
                if (!mem::ReadPtr(at, &e) || !EntityLike(e) || !game::Eid(e, &id)) { if (++misses >= 16) break; continue; }
                misses = 0; len = i + 1;
                ++ents; if ((id >> 24) == game::kTagPlayer) ++a0; else ++b0;
                if (!game::WorldPos(e, &q)) continue;
                ++withPos;
                const float dx = q.x - mp.x, dy = q.y - mp.y, dz = q.z - mp.z;
                if (dx * dx + dy * dy + dz * dz < 1600.0f) ++near40;
            }
            LOG("[roster] +%X -> %llX: run of %u, %d entities (%d player-tagged, %d world), %d placed, %d within 40 m | words -16..+24: %llX %llX [%llX] %llX %llX %llX",
                off, static_cast<unsigned long long>(arr), len, ents, a0, b0, withPos, near40,
                static_cast<unsigned long long>(w[0]), static_cast<unsigned long long>(w[1]), static_cast<unsigned long long>(w[2]),
                static_cast<unsigned long long>(w[3]), static_cast<unsigned long long>(w[4]), static_cast<unsigned long long>(w[5]));
            ++lines;
        }
        LOG("[roster] probe %d done, %d pools", s_runs, lines);
    }

    // --- pets follow the filters (issue #32) ---------------------------------
    // A pet takes whatever the game lets it, and nothing in its condition row
    // looks at the item. Its pick-up still crosses the event queue this mod
    // watches, stamped with the pet's id and the item's, so the mod sees it
    // happen. What lands is held against the inventory as it stood before,
    // and any increase the item rules would have refused is deleted through
    // TrocTrDeleteItemFromInventoryOnceTimer, the event the game raises for
    // its own removals. The delete names the slot and the slot's instance,
    // so it cannot take anything but what it was aimed at. Quest, protected
    // and dev items are never deleted: refusing to pick one up is caution,
    // destroying one is not.
    static uint16_t g_petKeyByType[64]; static uint8_t g_petKeyKnown[64];

    static uint16_t PetTypeKey(uint16_t type)
    {
        if (type >= 64) return 0xFFFF;
        if (!g_petKeyKnown[type])
        {
            uint16_t ks[4]; const int n = game::InvTypeKeysFor(type, ks, 4);
            if (!n) return 0xFFFF;   // the table may not be up yet; ask again next time
            g_petKeyByType[type] = ks[0]; g_petKeyKnown[type] = 1;
            LOG("[delete] inventory type %u is reached through key %u%s", type, ks[0], n > 1 ? " (more than one key resolves to it)" : "");
        }
        return g_petKeyByType[type];
    }

    // Delete `amount` of item row `tid` from the player's inventory, largest
    // stack first. Returns how many were asked for.
    // What the filter will not destroy, whatever the rules say about picking
    // it up off the ground. Declining to take something is caution; taking
    // something out of the player's bag is a loss, so the bar is higher here
    // than in Rules::Decide, and all three places that ask share this one
    // answer: the prevention hook, the pick-up window and the sweep.
    //
    // Currency is on the list because of 15 September 2026, when 4,433 Copper
    // and 4,290 Camp Weapons left Seth's bag as "pet loot, unsellable". Money
    // is class currency and no-sell by definition, so SkipNoSell refuses it,
    // and SkipNoSell governs what to pick up off the ground. Nothing should
    // route that into a delete. no-discard is the game's own word for the same
    // idea and travels with it.
    static bool NeverDelete(const Item& it, const Rules::Verdict& r)
    {
        if (it.klass == "currency") return true;
        if (it.HasTag("no-discard")) return true;
        return strcmp(r.rule, "protected") == 0 || strcmp(r.rule, "quest item") == 0 ||
               strcmp(r.rule, "dev item") == 0 || strcmp(r.rule, "quest equipment") == 0;
    }

    // Everything above, and two more that only matter where an item is about to
    // stop existing. NeverDelete has three other callers: the pet-prevention
    // hook, which leaves a refused item on the ground, and the body-drop path,
    // which puts one back on the ground. Neither costs the player the item, so
    // neither wants this list, and widening NeverDelete itself would have let a
    // pet start pocketing the unsellable things its owner switched off.
    //
    // LuxDragon lost a Sealed Abyss Artifact on 1.6.34, issue #84. He runs Skip
    // unsellable items, which is an ordinary way of saying do not fill my bag
    // with what I cannot sell, and the sweep read that refusal as leave to
    // destroy one. An Abyss Artifact carries no-discard and would have lived. A
    // Sealed Abyss Artifact carries no-sell and nothing else that is checked.
    //
    // The line is whether the player named the thing. An item override, a tag
    // never and a class skipped each name what they refuse, so acting on one is
    // doing as asked. Skip unsellable and the value floor are switches across
    // the whole database about what is not worth carrying, which is a different
    // statement from what is worth losing. Counted before it was written: 1,055
    // items carry no-sell, NeverDelete already spared 366, and this covers the
    // remaining 689, among them 424 keepsakes, 150 sealed artifacts and 5 keys.
    static bool NeverDestroy(const Item& it, const Rules::Verdict& r)
    {
        return NeverDelete(it, r) ||
               strcmp(r.rule, "unsellable") == 0 || strcmp(r.rule, "below value floor") == 0;
    }

    // --- refused body loot back on the ground (DROP.md) -------------------
    // A body or a carcass hands over everything it holds and no rule sees any
    // of it first. With the switch on, what the rules refuse goes back on the
    // ground through the routine the game's own discard request calls, one
    // pile per stack.
    //
    // That routine only works on the game's server thread. It reads a context
    // from the thread's TLS block at +0x250 that no other thread has, and a
    // first try from the mod's own game thread faulted on it every time. The
    // server also keeps its own actor for the player and its own holder for
    // the same bag. So this runs inside the server's parse of the mod's own
    // search, with the sender that parse was handed and the holder
    // GetInventoryHolder gives it there. Everything a search pays lands during
    // that parse: across eighteen searches in the first session, none paid
    // anything later.
    //
    // What is new is told apart by instance, not by item: the bag is read
    // slot by slot either side of the parse, and only an instance that was not
    // there before, or the amount a stack grew by, is ever dropped. A sword
    // the player already carried is never the one that goes, even when the
    // search paid another of the same.
    //
    // The two snapshots are read and written on the server thread only.
    static std::unordered_map<uint32_t, long long> g_searchBefore;   // instance -> count
    static uintptr_t g_searchHolder = 0;
    // The search being parsed was a pet's or a companion's. Its sender is that
    // companion, so the drop goes where the player stands, not where the pet
    // does, and GetInventoryHolder is asked about the pet: the game gives a
    // pet the bag its loot lands in.
    static bool g_searchByPet = false;

    // Where the played body stands, in the frame the drop routine wants: the
    // transform's +0x324, which +0x3D0 repeats. The +0xB4 the scan reads sits
    // a regional origin away from it and that origin moves, so it is read off
    // the body itself as the enumeration hands the body over. As Kliff there
    // is no separate body and the search's own sender stands where he does;
    // as Damiane or Oongka the sender is the identity, which stands somewhere
    // else entirely, so the body's position is the only one to use.
    //
    // The scan's reading of the same point sits beside it, so the difference
    // between the two frames is known wherever the player stands. A hand drop
    // names its spot in the world frame and the scan works in the other.
    struct DropAt { uint32_t eid; DWORD at; float pos[3]; float scan[3]; };
    static DropAt g_bodyDropAt{};
    static SRWLOCK g_bodyDropLock = SRWLOCK_INIT;

    static bool ReadWorldPos(uintptr_t ent, float out[3])
    {
        const uintptr_t tf = ent ? game::Transform(game::Comps(ent)) : 0;
        float twin[3] = {};
        if (!tf || !mem::ReadF32x3(tf + 0x324, out) || !mem::ReadF32x3(tf + 0x3D0, twin)) return false;
        const float dx = out[0] - twin[0], dy = out[1] - twin[1], dz = out[2] - twin[2];
        if (dx * dx + dy * dy + dz * dz > 4.0f) return false;
        return !(fabsf(out[0]) < 1.0f && fabsf(out[1]) < 1.0f && fabsf(out[2]) < 1.0f);
    }

    static void PublishBodyDropAt(uintptr_t ent, uint32_t eid, const Vec3& scanAt, DWORD now)
    {
        float w[3];
        if (!ReadWorldPos(ent, w)) return;
        AcquireSRWLockExclusive(&g_bodyDropLock);
        g_bodyDropAt = { eid, now, { w[0], w[1], w[2] }, { scanAt.x, scanAt.y, scanAt.z } };
        ReleaseSRWLockExclusive(&g_bodyDropLock);
        static uint32_t s_said = 0;
        if (g_debugLog && s_said != eid)
        {
            s_said = eid;
            LOG("[drop] %08X, the character being played, stands at %.1f %.1f %.1f in the world, %.1f %.1f %.1f as the scan reads it",
                eid, w[0], w[1], w[2], scanAt.x, scanAt.y, scanAt.z);
        }
    }

    // -1 when the holder could not be read, which is not the same answer as an
    // empty bag. Only the carried bag, the key every hand drop names.
    static int ServerBag(uintptr_t holder, game::InvEntry* ents, int max)
    {
        static game::BucketInfo bks[64];
        const int bn = game::HolderBuckets(holder, bks, 64);
        if (bn <= 0) return -1;
        const int n = game::HolderEntries(holder, ents, max);
        const uint16_t bagType = game::InvTypeLookup(2);
        if (bagType == 0xFFFF) return -1;
        int k = 0;
        for (int i = 0; i < n; ++i)
            if (ents[i].bucket < bn && bks[ents[i].bucket].type == bagType) ents[k++] = ents[i];
        return k;
    }

    bool SearchParse(uintptr_t sender, uint32_t target, bool after)
    {
        static game::InvEntry ents[4096];
        if (!after)
        {
            g_searchHolder = 0;
            g_searchByPet = false;
            if (!Settings::Get().dropRefused || !hooks::DropReady() || !sender) return false;
            // A body the player searched by hand keeps what it paid. One this
            // mod searched, or one a pet or a companion did, is judged.
            uint32_t eid = 0;
            if (!game::Eid(sender, &eid)) return false;
            if (events::SearchedRecently(target))
            {
                if ((eid >> 24) != game::kTagPlayer) return false;
            }
            else if (events::CompanionSearchedRecently(target)) g_searchByPet = true;
            else return false;
            if (!hooks::ThreadContext()) return false;
            const uintptr_t holder = game::HolderNow(sender);
            if (!holder) return false;
            const int n = ServerBag(holder, ents, 4096);
            if (n < 0) return false;
            g_searchBefore.clear();
            for (int i = 0; i < n; ++i) g_searchBefore[ents[i].iid] += ents[i].count;
            g_searchHolder = holder;
            if (g_searchByPet)
            {
                static int s_said = 0;
                if (s_said++ < 3)
                    LOG("[drop] watching a companion's search of %08X: sender %08X, bag holder %llX, %d stacks in the bag",
                        target, eid, static_cast<unsigned long long>(holder), n);
            }
            return true;
        }
        const uintptr_t holder = g_searchHolder;
        g_searchHolder = 0;
        if (!holder) return false;
        const int n = ServerBag(holder, ents, 4096);
        if (n <= 0) return false;

        // What each instance gained. An instance that was not there before
        // gained all of it.
        std::unordered_map<uint32_t, long long> gained;
        {
            std::unordered_map<uint32_t, long long> now;
            for (int i = 0; i < n; ++i) now[ents[i].iid] += ents[i].count;
            for (const auto& [iid, c] : now)
            {
                const auto b = g_searchBefore.find(iid);
                const long long g = c - (b == g_searchBefore.end() ? 0 : b->second);
                if (g > 0) gained[iid] = g;
            }
        }
        if (gained.empty()) return true;

        // Beside the player and a little up, near where a hand drop lands.
        uint32_t senderEid = 0; game::Eid(sender, &senderEid);
        float at[3] = {};
        bool placed = false;
        const char* where = "";
        {
            AcquireSRWLockShared(&g_bodyDropLock);
            const DropAt b = g_bodyDropAt;
            ReleaseSRWLockShared(&g_bodyDropLock);
            const uint32_t body = g_bodyEid;
            if (g_searchByPet)
            {
                const uint32_t me = body ? body : g_meEid;
                if (b.eid == me && GetTickCount() - b.at < 3000)
                { at[0] = b.pos[0]; at[1] = b.pos[1]; at[2] = b.pos[2]; placed = true; where = body ? "the body" : "the player"; }
            }
            else if (body && body != senderEid)
            {
                if (b.eid == body && GetTickCount() - b.at < 3000)
                { at[0] = b.pos[0]; at[1] = b.pos[1]; at[2] = b.pos[2]; placed = true; where = "the body"; }
            }
            else if (ReadWorldPos(sender, at)) { placed = true; where = "the player"; }
        }
        const float tfm[7] = { at[0] + 0.8f, at[1] + 0.5f, at[2], 0.f, 0.f, 0.f, 1.f };

        // The rules as one consistent copy: the menu edits the live maps under
        // the settings lock, and this is not the thread holding it.
        const Config cfg = Settings::Snapshot();
        int drops = 0;
        for (const auto& [iid, g] : gained)
        {
            long long left = g;
            for (int i = 0; i < n && left > 0 && drops < 16; ++i)
            {
                game::InvEntry& e = ents[i];
                if (e.iid != iid || e.count <= 0) continue;
                const Item* it = ItemDb::ByRow(e.tid);
                if (!it) break;   // no rule can be asked about a thing with no name
                // Never a document or a quest item by any route, the rule
                // auto-store keeps as well, and nothing the pet filter would
                // refuse to delete.
                if (it->klass == "document" || it->tags.find(" quest ") != std::string::npos) break;
                const Rules::Verdict v = Rules::Decide(*it, cfg);
                if (v.loot || NeverDelete(*it, v)) break;
                if (!placed)
                {
                    LOG("[drop] no fresh world position for the body being played; %lld %s stays in the bag", left, it->name.c_str());
                    break;
                }
                const long long take = e.count < left ? e.count : left;
                uint32_t err = 0xFFFFFFFFu;
                ++drops;
                if (!hooks::CallDrop(holder, &err, sender, 2, static_cast<uint16_t>(e.slot), take, tfm))
                { LOG_ERR("[drop] the game's drop faulted on %lld %s; it stays in the bag", take, it->name.c_str()); break; }
                if (err) { LOG_ERR("[drop] the game would not drop %lld %s: error %08X", take, it->name.c_str(), err); break; }
                LOG("[drop] left %lld %s beside %s from %08X%s: %s%s%s", take, it->name.c_str(), where, target,
                    g_searchByPet ? ", which a pet or a companion searched" : "", v.rule, v.detail.empty() ? "" : " ", v.detail.c_str());
                left -= take; e.count -= take;
            }
        }
        return true;
    }

    // --- what the player drops by hand (HandDrop) ---------------------------
    // The slot a discard names, read on the server thread before the parse so
    // the note says which item it is, and after it so the log says whether it
    // left. Server thread only, like the search watch above.
    //
    // A full slot after the parse does not withdraw the note. Nothing has shown
    // yet that the routine empties the slot before it returns rather than a
    // moment later, and if it is later, withdrawing would hand every drop back
    // to the scan. A note left open for a refused drop costs fifteen seconds
    // of watching for something that never lands.
    struct HandParse { uintptr_t holder; uint16_t key, slot, tid; uint32_t iid; long long count; };
    static HandParse g_handParse{};

    // The occupied slot `slot` of the inventory the key names. False when it
    // is empty or the holder cannot be read.
    static bool SlotAt(uintptr_t holder, uint16_t key, uint16_t slot, game::InvEntry* out)
    {
        static game::BucketInfo bks[64];
        static game::InvEntry ents[4096];
        const int bn = game::HolderBuckets(holder, bks, 64);
        const uint16_t type = game::InvTypeLookup(key);
        if (bn <= 0 || type == 0xFFFF) return false;
        const int n = game::HolderEntries(holder, ents, 4096);
        for (int i = 0; i < n; ++i)
            if (ents[i].slot == slot && ents[i].bucket < bn && bks[ents[i].bucket].type == type) { *out = ents[i]; return true; }
        return false;
    }

    bool HandDropParse(uintptr_t sender, uint16_t key, uint16_t slot, long long amount, const float* at, bool after)
    {
        if (!after)
        {
            g_handParse = {};
            if (!sender || !at || slot == 0xFFFF || amount <= 0 || !hooks::ThreadContext()) return false;
            const uintptr_t holder = game::HolderNow(sender);
            game::InvEntry e{};
            if (!holder || !SlotAt(holder, key, slot, &e)) return false;
            HandDrop d;
            d.tid = e.tid;
            d.bagIid = e.iid;
            d.left = static_cast<int>(amount < 256 ? amount : 256);
            for (int a = 0; a < 3; ++a) d.world[a] = at[a];
            d.at = GetTickCount();
            d.pass = static_cast<uint32_t>(InterlockedCompareExchange(&g_handPass, 0, 0));
            // Two seconds covers the slowest scan rate the menu allows.
            d.scanLive = d.pass && d.at - static_cast<DWORD>(InterlockedCompareExchange(&g_handScanAt, 0, 0)) < 2000;
            // Handed over before the parse runs, so nothing the parse puts in
            // the world can reach a scan that has not been told to expect it.
            AcquireSRWLockExclusive(&g_handDropLock);
            if (g_handDropIn.size() >= 256) g_handDropIn.erase(g_handDropIn.begin());
            g_handDropIn.push_back(d);
            ReleaseSRWLockExclusive(&g_handDropLock);
            g_handParse = { holder, key, slot, e.tid, e.iid, e.count };
            return true;
        }
        const HandParse p = g_handParse;
        g_handParse = {};
        if (!p.holder) return false;
        game::InvEntry e{};
        const long long kept = SlotAt(p.holder, p.key, p.slot, &e) && e.iid == p.iid ? e.count : 0;
        const Item* it = ItemDb::ByRow(p.tid);
        const char* name = it ? it->name.c_str() : "item";
        if (kept >= p.count)
        {
            LOG("[drop] slot %u still held all %lld %s (row %u) when the game's parse of your drop returned; "
                "if nothing lands, the game refused it", p.slot, p.count, name, p.tid);
            return true;
        }
        LOG("[drop] you dropped %lld %s (row %u) from slot %u, instance %08X in the bag; the mod leaves it where it lands",
            p.count - kept, name, p.tid, p.slot, p.iid);
        return true;
    }

    static constexpr float kHandDropRadius2  = 25.0f;   // 5 m round the spot the game was given
    static constexpr DWORD kHandDropWindowMs = 15000;
    // A stack can land as one object or as one per unit, and the note cannot
    // tell which in advance. The pieces of one drop land together, so a note
    // closes this long after its first recognition rather than holding its
    // whole count open for the window, which would hand anything of that row
    // turning up nearby later to the player's drop.
    static constexpr DWORD kHandDropTailMs   = 3000;

    // Recognise what a hand drop left on the ground. Scan thread, after Fill
    // and before Decide. Every item Fill reached is numbered with the pass that
    // first saw it, which is how a note from a running scan tells the dropped
    // object from one of the same kind that already lay there. A pass number
    // rather than a time, so the answer does not depend on the scan rate.
    //
    // A note made while the scan was not running, with auto-loot off and the
    // menu shut, has no such record to go on. It waits until the player is
    // back near the spot and takes the nearest objects of that row, so turning
    // auto-loot on beside a pile you dropped does not sweep it up.
    static void ClaimHandDrops(const std::vector<Cand>& list, const Vec3& mp, DWORD now)
    {
        // A pass more than two seconds after the one before means the scan
        // stopped in between, with auto-loot off and the menu shut. A note
        // parsed before that gap can no longer lean on the pass numbers, since
        // everything reads as new after it, nor on where the player stands.
        static uint32_t s_pass = 0, s_gapPass = 0;
        static DWORD s_passAt = 0;
        ++s_pass;
        if (s_passAt && now - s_passAt > 2000) s_gapPass = s_pass;
        s_passAt = now;
        for (const Cand& k : list)
            if (k.filled && k.item) g_itemFirstPass.emplace(k.eid, s_pass);
        if (g_itemFirstPass.size() > 4096)
            for (auto it = g_itemFirstPass.begin(); it != g_itemFirstPass.end();)
                it = g_present.count(it->first) ? std::next(it) : g_itemFirstPass.erase(it);
        // Published only once this pass's sightings are in, so a drop parsed
        // from here on is measured against everything this pass saw.
        InterlockedExchange(&g_handScanAt, static_cast<LONG>(now));
        InterlockedExchange(&g_handPass, static_cast<LONG>(s_pass));

        // The world frame to the scan's, from the body's two readings of
        // itself. The regional origin between them moves, so it is taken
        // fresh each pass rather than once per note.
        bool haveOff = false;
        float off[3] = {};
        {
            AcquireSRWLockShared(&g_bodyDropLock);
            const DropAt b = g_bodyDropAt;
            ReleaseSRWLockShared(&g_bodyDropLock);
            if (b.eid && now - b.at < 3000) { haveOff = true; for (int a = 0; a < 3; ++a) off[a] = b.pos[a] - b.scan[a]; }
        }
        auto dist2 = [](const Vec3& a, const Vec3& b) {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        };
        // Where to look. A live note was taken while the player stood at the
        // drop, so the player's position then will do when the frames cannot
        // be converted. Any other note has only the converted spot, and waits
        // for a pass that has it.
        auto spotOf = [&](const HandDrop& d, Vec3* out) {
            if (d.scanLive && (d.useStamp || !haveOff)) { *out = d.stamp; return true; }
            if (!haveOff) return false;
            out->x = d.world[0] - off[0]; out->y = d.world[1] - off[1]; out->z = d.world[2] - off[2];
            return true;
        };
        auto demote = [&](HandDrop& d) {
            if (!d.scanLive || d.pass >= s_gapPass) return;
            d.scanLive = false;
            d.useStamp = false;
            d.seenAt = 0;
        };

        {
            AcquireSRWLockExclusive(&g_handDropLock);
            for (HandDrop& d : g_handDropIn)
            {
                d.stamp = mp;
                demote(d);
                // A note still live here was made where the player stands now.
                // A converted spot ten metres from there means the frames did
                // not convert, and the player's own position is the better
                // guess.
                if (d.scanLive)
                {
                    d.seenAt = now;
                    Vec3 at;
                    if (spotOf(d, &at) && dist2(at, mp) > 100.0f) d.useStamp = true;
                }
                if (d.bagIid) g_droppedIid[d.bagIid] = d.tid;
                g_handDrops.push_back(d);
            }
            g_handDropIn.clear();
            ReleaseSRWLockExclusive(&g_handDropLock);
        }

        // What the instance id caught that the spot has not, written down by
        // entity once so the log says which of the two found it.
        static int s_iidLines = 0;
        for (const Cand& k : list)
        {
            if (!k.filled || !k.item || !k.iid || g_droppedEid.count(k.eid)) continue;
            const auto it = g_droppedIid.find(k.iid);
            if (it == g_droppedIid.end() || it->second != k.tid) continue;
            g_droppedEid.insert(k.eid);
            // Counted against its note, or the note would go on looking for
            // it by position and could hand something else of that row to
            // the player's drop.
            for (HandDrop& d : g_handDrops)
                if (d.bagIid == k.iid && d.tid == k.tid && d.left > 0)
                {
                    --d.left;
                    if (!d.claimed++) d.claimedAt = now;
                    break;
                }
            if (s_iidLines < 60)
            {
                ++s_iidLines;
                LOG("[drop] %08X %s is what you dropped, known by its instance id %08X; it stays there", k.eid, Label(k), k.iid);
            }
        }
        if (g_handDrops.empty()) return;

        // Notes that are over go first, so none of them claims anything on the
        // pass that ends it.
        static int s_claimLines = 0, s_missLines = 0;
        for (size_t i = 0; i < g_handDrops.size();)
        {
            HandDrop& d = g_handDrops[i];
            demote(d);
            const bool over = d.left <= 0 || (d.seenAt && now - d.seenAt > kHandDropWindowMs) ||
                              (d.claimedAt && now - d.claimedAt > kHandDropTailMs);
            if (!over) { ++i; continue; }
            // A note that recognised nothing is the case worth reading about:
            // whatever it was waiting for went into the bag, or never came.
            if (!d.claimed && s_missLines < 20)
            {
                ++s_missLines;
                const Item* it = ItemDb::ByRow(d.tid);
                char nearest[64] = "the scan saw none of that row";
                if (d.nearest >= 0) snprintf(nearest, sizeof nearest, "the nearest of that row was %.1f m from it", d.nearest);
                LOG("[drop] nothing new turned up within 5 m of where the game put the %s you dropped; %s%s%s",
                    it ? it->name.c_str() : "item", nearest,
                    d.scanLive ? "" : ", and the scan was not running the whole time since the drop",
                    d.useStamp ? ", measured from where you stood because the frames did not convert" : "");
            }
            g_handDrops.erase(g_handDrops.begin() + static_cast<long>(i));
        }

        for (HandDrop& d : g_handDrops)
        {
            Vec3 spot;
            if (d.left <= 0 || !spotOf(d, &spot)) continue;
            if (!d.seenAt && dist2(spot, mp) <= kHandDropRadius2) d.seenAt = now;
            std::vector<std::pair<float, size_t>> hits;
            for (size_t i = 0; i < list.size(); ++i)
            {
                const Cand& k = list[i];
                if (!k.filled || !k.item || k.tid != d.tid || g_droppedEid.count(k.eid)) continue;
                const float dd = dist2(k.pos, spot);
                if (d.nearest < 0 || dd < d.nearest * d.nearest) d.nearest = std::sqrt(dd);
                if (dd > kHandDropRadius2) continue;
                if (d.scanLive)
                {
                    const auto f = g_itemFirstPass.find(k.eid);
                    if (f != g_itemFirstPass.end() && f->second <= d.pass) continue;   // it was there first
                }
                hits.emplace_back(dd, i);
            }
            std::sort(hits.begin(), hits.end());
            for (const auto& n : hits)
            {
                if (d.left <= 0) break;
                const Cand& k = list[n.second];
                g_droppedEid.insert(k.eid);
                if (k.iid) g_droppedIid[k.iid] = k.tid;
                --d.left;
                if (!d.claimed++) d.claimedAt = now;
                if (s_claimLines < 60)
                {
                    ++s_claimLines;
                    LOG("[drop] %08X %s is what you dropped, %.1f m from where the game put it (instance %08X, the bag's was %08X); it stays there",
                        k.eid, Label(k), std::sqrt(n.first), k.iid, d.bagIid);
                }
            }
        }

        if (g_handDrops.size() > 256)
            g_handDrops.erase(g_handDrops.begin(), g_handDrops.begin() + static_cast<long>(g_handDrops.size() - 256));
    }

    static long long DeleteFromInventory(uint16_t tid, long long amount, const char* why)
    {
        static game::InvEntry ents[4096]; static game::BucketInfo bks[64];
        const int n = game::InventoryEntries(g_me, ents, 4096);
        const int bn = game::InventoryBuckets(g_me, bks, 64);
        const uint32_t route = events::RouteKnown() ? events::Route() : game::Route(g_me);
        const Item* it = ItemDb::ByRow(tid);
        long long left = amount, sent = 0;
        while (left > 0)
        {
            int best = -1;
            for (int i = 0; i < n; ++i) if (ents[i].tid == tid && ents[i].count > 0 && (best < 0 || ents[i].count > ents[best].count)) best = i;
            if (best < 0) break;
            game::InvEntry& e = ents[best];
            const uint16_t type = e.bucket < bn ? bks[e.bucket].type : 0xFFFF;
            const uint16_t key = PetTypeKey(type);
            uint64_t inst = 0; mem::Read64(e.addr, &inst);
            const long long take = e.count < left ? e.count : left;
            if (key == 0xFFFF || !inst || inst == ~0ull)
            {
                LOG_ERR("[delete] cannot delete %lld of %s (row %u) from bucket %d slot %d: type %u key %u instance %llX", take, it ? it->name.c_str() : "?", tid, e.bucket, e.slot, type, key, static_cast<unsigned long long>(inst));
                e.count = 0; continue;
            }
            if (!events::DeleteItem(inst, key, static_cast<uint16_t>(e.slot), static_cast<uint64_t>(take), static_cast<uint16_t>(take), g_meEid, route))
            { LOG_ERR("[delete] refused: sending is not allowed or the queue is full"); break; }
            LOG("[delete] %lld %s (row %u) from bucket %d slot %d, instance %llX: %s", take, it ? it->name.c_str() : "?", tid, e.bucket, e.slot, static_cast<unsigned long long>(inst), why);
            left -= take; sent += take; e.count = 0;
        }
        if (left > 0) LOG_ERR("[delete] %lld of %s (row %u) not found in the inventory", left, it ? it->name.c_str() : "?", tid);
        return sent;
    }

    // Judge a thing before a pet touches it, rather than after.
    //
    // The filter has always worked by deletion: the pet takes whatever it
    // likes, the mod sees what landed and destroys the part the rules refuse.
    // That costs the item, puts a notice on screen and, worst of it, cannot
    // tell a pet's pick-up of a loose item from the player's own, because the
    // game raises both as the player. The condition hook opened a better door.
    // The game asks, for each thing a pet is about to reach for, whether the
    // pet may loot it, and the question carries the thing. So answer no for
    // exactly what the rules refuse and the pet walks past it: nothing is
    // taken, nothing is deleted, and the player's own pick-ups are never in
    // the question at all.
    //
    // Identity comes from the Gimmick component the same way Fill reads it,
    // which is the only place an entity's item identity lives. No gimmick, no
    // identity, and then the answer is -1 and the game decides as it always
    // did, with the old delete path still behind it as the backstop.
    void DescribeEntity(uintptr_t ent, char* out, size_t n)
    {
        if (!out || !n) return;
        out[0] = 0;
        if (!ent) { snprintf(out, n, "nothing"); return; }
        uint32_t eid = 0;
        game::Eid(ent, &eid);
        char item[64] = "", node[160] = "";
        const uintptr_t comps = game::Comps(ent);
        if (const uintptr_t gm = comps ? game::CompByClass(comps, kCls_Gimmick) : 0)
        {
            game::NodePrefab(gm, node, sizeof node);
            if (const uintptr_t idata = mem::Deref(gm, kOff_Gimmick_ItemData))
            {
                uint16_t tid = 0;
                if (mem::Read16(idata + 8, &tid) && tid)
                    if (const Item* it = ItemDb::ByRow(tid))
                        snprintf(item, sizeof item, "%s", it->name.c_str());
            }
        }
        snprintf(out, n, "%08X %s%s%s", eid,
                 item[0] ? item : "unnamed",
                 node[0] ? " node " : "", node[0] ? node : "");
    }

    void DescribeEngineState(char* out, size_t n)
    {
        if (!out || !n) return;
        int act = 0, arm = 0, drv = 0;
        events::PendingCounts(&act, &arm, &drv);
        const DWORD now = GetTickCount();
        const DWORD ch = g_changeAt;
        char change[128];
        if (ch)
            snprintf(change, sizeof change, "last world change %lu ms ago (%s, %.1f m)",
                     static_cast<unsigned long>(now - ch), g_changeWhy, g_changeJumpM);
        else
            snprintf(change, sizeof change, "no world change seen this session");
        snprintf(out, n, "queued: %d send, %d arm, %d drive; well run %s; %s; scan centre %08X",
                 act, arm, drv, g_wellRun.active ? "LIVE" : "idle", change, g_bodyEid ? g_bodyEid : g_meEid);
    }

    int JudgeEntityForPet(uintptr_t ent, char* name, size_t n)
    {
        if (name && n) name[0] = 0;
        if (!ent) return -1;
        const uintptr_t comps = game::Comps(ent);
        const uintptr_t inter = comps ? game::CompByClass(comps, kCls_Gimmick) : 0;
        const uintptr_t idata = inter ? mem::Deref(inter, kOff_Gimmick_ItemData) : 0;
        if (!idata) return -1;
        uint16_t tid = 0;
        if (!mem::Read16(idata + 8, &tid) || !tid) return -1;
        const Item* it = ItemDb::ByRow(tid);
        if (!it) return -1;
        if (name && n) snprintf(name, n, "%s", it->name.c_str());
        const Rules::Verdict r = Rules::Decide(*it, Settings::Get());
        // Refuse only what the filter would have deleted. The rules the
        // sweep spares are spared here too: the mod refuses to take a quest,
        // protected or dev item itself out of caution, and turning that
        // caution into "the pet may not pick your quest item up either" would
        // lose the player something the filter has never been willing to
        // destroy. Not taking one is careful; keeping one out of the bag is not.
        const bool spare = NeverDelete(*it, r);
        return (r.loot || spare) ? 0 : 1;
    }

    static void PetFilterTick(const Config& cfg, DWORD now)
    {
        AgeHandRows(now);
        static std::unordered_map<uint16_t, long long> base, snap;
        static DWORD snapAt = 0, windowUntil = 0, windowFrom = 0;
        static uint16_t tids[64]; static int tidN = 0; static bool anyUnknown = false;
        static game::InvEntry ents[4096];
        // Which holder each sample read. Two samples of different holders are
        // two different bags, as when the game names the bag Damiane or Oongka
        // borrows a moment after the scan settles on her, and comparing them
        // reads a whole bag as arriving at once. Neither path compares them.
        static uintptr_t snapHolder = 0, baseHolder = 0;
        // The snapshot's time is the end of the walk, so a pick-up stamped
        // later than it landed after every slot was read.
        auto snapshot = [&](std::unordered_map<uint16_t, long long>& into, uintptr_t& holderOut) {
            into.clear();
            const int n = game::InventoryEntries(g_me, ents, 4096, &holderOut);
            for (int i = 0; i < n; ++i) into[ents[i].tid] += ents[i].count;
            return GetTickCount();
        };
        // A companion picks things up one or two at a time. A dozen kinds
        // rising at once, or a single row rising by hundreds, is a bag being
        // replaced rather than filled: a load the scan did not catch, a
        // storage transfer, a quest handout, a camp paying out. Falls count as
        // much as rises, because loading an older save brings back a smaller
        // bag and the giveaway there is things vanishing that nobody spent.
        //
        // This was written for the sweep and only ever ran on the sweep. The
        // pick-up window had no such test, so on 15 September 2026 a window
        // that saw 45 kinds arrive and one row rise by 4,433 deleted two of
        // them. Both callers ask this now.
        const auto bagReplaced = [](const std::unordered_map<uint16_t, long long>& before,
                                    const std::unordered_map<uint16_t, long long>& after,
                                    int* risenOut, int* fallenOut, long long* biggestOut) -> bool
        {
            int risen = 0, fallen = 0; long long biggest = 0;
            for (const auto& kv : after)
            {
                const auto b = before.find(kv.first);
                const long long d = kv.second - (b == before.end() ? 0 : b->second);
                if (d > 0) { ++risen; if (d > biggest) biggest = d; }
            }
            for (const auto& kv : before)
            {
                const auto f = after.find(kv.first);
                if ((f == after.end() ? 0 : f->second) < kv.second) ++fallen;
            }
            if (risenOut) *risenOut = risen;
            if (fallenOut) *fallenOut = fallen;
            if (biggestOut) *biggestOut = biggest;
            return risen > 4 || fallen > 4 || biggest > 200;
        };

        events::PetPickup pp[64];
        int n = events::DrainPetPickups(pp, 32);
        // The played body's own pick-ups arrive here too, since as Damiane or
        // Oongka it is not the identity the event layer compares against.
        // They are the player's, so they are recorded and taken out here.
        {
            int k = 0;
            for (int i = 0; i < n; ++i)
            {
                if (RaisedByPlayer(pp[i].pet))
                {
                    NoteHandTake(pp[i].item, !pp[i].search, now);
                    const auto it = g_tidByEid.find(pp[i].item);
                    const Item* db = it == g_tidByEid.end() ? nullptr : ItemDb::ByRow(it->second);
                    LOG("[pet] %08X (you) %s %08X: %s; yours, so never deleted", pp[i].pet, pp[i].search ? "searched" : "picked up",
                        pp[i].item, db ? db->name.c_str() : "unnamed");
                    continue;
                }
                // A pick-up of something the scan never saw names nothing, and
                // judging every rise in the bag behind it deleted what the
                // player had just been handed. Every entity in the world that
                // is not the player lands in this queue, a bandit taking up a
                // sword as much as a pet, and LuxDragon's bounty note on 23
                // September 2026 arrived in the bag 2.7 s after one such
                // pick-up by B06034FB, which no scan ever placed near him, and
                // was deleted for his document rule. Across every log on hand
                // this path never caught a pet's loot: of four windows, two
                // landed nothing and two deleted the player's own things. A
                // body search still opens the window, since a pet's search
                // names only the body and what it holds is never known first.
                {
                    const auto it = g_tidByEid.find(pp[i].item);
                    if (!pp[i].search && (it == g_tidByEid.end() || !it->second))
                    {
                        LOG("[pet] %08X picked up %08X, which the scan never saw, so nothing in the bag can be tied to it "
                            "and nothing is judged", pp[i].pet, pp[i].item);
                        continue;
                    }
                }
                pp[k++] = pp[i];
            }
            n = k;
        }
        if (n > 0)
        {
            if (!windowUntil)
            {
                if (snapAt && static_cast<long>(pp[0].at - snapAt) >= 0) { base = snap; baseHolder = snapHolder; windowFrom = snapAt; }
                else { windowFrom = snapshot(base, baseHolder); LOG("[pet] no inventory snapshot from before the pick-up; whatever landed already is kept"); }
                tidN = 0; anyUnknown = false;
            }
            for (int i = 0; i < n; ++i)
            {
                // Stamped with the pet's own id, so a pet is out and working.
                NotePetActivity(now);
                const auto it = g_tidByEid.find(pp[i].item);
                const uint16_t tid = it == g_tidByEid.end() ? 0 : it->second;
                const Item* db = tid ? ItemDb::ByRow(tid) : nullptr;
                if (tid) { if (tidN < 64) tids[tidN++] = tid; } else anyUnknown = true;
                // Say which kind of companion it was. A pet is a world actor
                // and a hired mercenary is player-tagged, and the game has one
                // looting rule, written for pets, with no mercenary of its own.
                // So a line here naming a player-tagged raiser is the evidence
                // that a mercenary loots at all, which no session has shown yet.
                const char* kind = (pp[i].pet >> 24) == game::kTagPlayer ? "a hired companion" : "a pet";
                LOG("[pet] %08X (%s) %s %08X: %s", pp[i].pet, kind, pp[i].search ? "searched" : "picked up", pp[i].item,
                    db ? db->name.c_str() : pp[i].search ? "a body; judged by what lands" : "row known, unnamed");
            }
            windowUntil = now + 2500;
            return;
        }
        if (windowUntil)
        {
            if (static_cast<long>(now - windowUntil) < 0) return;
            snapAt = snapshot(snap, snapHolder);
            if (snapHolder != baseHolder)
            {
                LOG("[pet] the bag being read changed during the window, so the two samples are of different bags "
                    "and nothing is judged");
                windowUntil = 0; tidN = 0; anyUnknown = false; base.clear();
                return;
            }
            // The test the sweep has always made. Without it this path deleted
            // 4,433 Copper out of a bag that had just gained 45 kinds.
            {
                int risen = 0, fallen = 0; long long biggest = 0;
                if (bagReplaced(base, snap, &risen, &fallen, &biggest))
                {
                    LOG("[pet] the bag changed shape all at once during the window (%d kinds up, %d down, "
                        "largest rise %lld): that is a bag being filled from somewhere else and not a companion "
                        "looting, so nothing is deleted", risen, fallen, biggest);
                    windowUntil = 0; tidN = 0; anyUnknown = false;
                    return;
                }
            }
            int deleted = 0, kept = 0;
            char notice[240] = ""; int nw = 0;
            for (const auto& kv : snap)
            {
                const auto b = base.find(kv.first);
                const long long delta = kv.second - (b == base.end() ? 0 : b->second);
                if (delta <= 0) continue;
                bool named = false;
                for (int i = 0; i < tidN; ++i) if (tids[i] == kv.first) named = true;
                // Something the player took unnamed in the same window could be
                // any row, so an unnamed pet pick-up no longer widens the net.
                const bool handUnknown = HandTookUnknown(windowFrom - 1500);
                if (!(anyUnknown && !handUnknown) && !named) continue;
                const Item* it = ItemDb::ByRow(kv.first);
                if (!it) { LOG("[pet] +%lld of row %u, not in the item database: kept", delta, kv.first); ++kept; continue; }
                if (HandTookRow(kv.first, windowFrom - 1500))
                {
                    LOG("[pet] +%lld %s: kept, you picked one up yourself in the same window", delta, it->name.c_str());
                    ++kept; continue;
                }
                const Rules::Verdict r = Rules::Decide(*it, cfg);
                const bool spare = NeverDestroy(*it, r);
                if (r.loot || spare) { LOG("[pet] +%lld %s: kept (%s%s%s)", delta, it->name.c_str(), r.rule, r.detail.empty() ? "" : " ", r.detail.c_str()); ++kept; continue; }
                char why[120]; snprintf(why, sizeof why, "pet loot, %s%s%s", r.rule, r.detail.empty() ? "" : " ", r.detail.c_str());
                const long long sent = DeleteFromInventory(kv.first, delta, why); ++deleted;
                if (sent > 0 && nw < static_cast<int>(sizeof notice) - 40)
                    nw += snprintf(notice + nw, sizeof notice - nw, "%s%lld %s", nw ? ", " : "", sent, it->name.c_str());
            }
            LOG("[pet] judged what landed: %d kinds deleted, %d kept", deleted, kept);
            // Say so on screen: something left the bag that the player never
            // saw arrive, and a silent removal reads as a bug or a theft.
            if (nw && cfg.showHud)
            {
                char msg[300];
                snprintf(msg, sizeof msg, "Master Looter: deleted %s. A companion picked it up and your rules refuse it.", notice);
                State::Get().Notify(msg, 6000, true);
            }
            windowUntil = 0; base.clear();
            return;
        }
        // Nothing to judge from an event, so judge from the bag itself.
        //
        // A companion can put something in the inventory without raising any
        // descriptor this mod watches. Seth's session of 11 September 2026 has
        // a mercenary out for six minutes, twelve events from it, not one of
        // them a pick-up, and stone arriving in the bag the whole time with
        // every stone switch off. The window machinery above never opened
        // because it had no event to open on.
        //
        // So: while a companion is out, diff the bag against the snapshot taken
        // 400 ms ago and delete any rise the rules refuse. Only while one is
        // out, because with nobody else in the world a rise is the player's own
        // doing and taking it back is the bug this all started from. Anything
        // the mod itself sent is pending in g_pend and is allowed by
        // construction, since the mod only ever sends what the rules permit.
        // The companion has to have acted since this baseline was taken. One
        // that helped half a minute ago explains nothing about what arrived in
        // the last two seconds.
        //
        // Once the pet is told no before it reaches, a pet cannot put a
        // refused loose item in the bag, and what it takes from a body is
        // judged by the window above, which opens on the search stamped with
        // its own id. So only a mercenary still needs the sweep: it is the
        // companion that started it, and nobody has seen whether it asks.
        const bool prevented = hooks::PetLootingHooked() && (cfg.petFilter || cfg.stopPetLooting);
        const DWORD companionAt = prevented ? events::MercenaryActiveAt() : events::CompanionActiveAt();
        const bool companionSince = (companionAt && static_cast<long>(companionAt - snapAt) >= 0) ||
                                    (!prevented && g_petSeenAt && static_cast<long>(g_petSeenAt - snapAt) >= 0);
        const bool companionOut = prevented ? MercenaryOutRecently(now) : PetOutRecently(now);
        if (cfg.petFilter && companionOut && companionSince && snapAt && now - snapAt >= 2000)
        {
            std::unordered_map<uint16_t, long long> fresh;
            uintptr_t freshHolder = 0;
            const DWORD freshAt = snapshot(fresh, freshHolder);

            // Everything below compares two samples of the same bag. If the
            // world moved between them it is not the same bag, so take the new
            // one as the baseline and judge nothing this pass.
            if (TakeBagBaselineStale() || freshHolder != snapHolder)
            {
                LOG("[pet] the world, or the bag being read, changed since the last bag sample, so this one starts fresh and nothing is judged");
                snap.swap(fresh); snapAt = freshAt; snapHolder = freshHolder; return;
            }

            // A companion picks things up one or two at a time. A dozen kinds
            // rising at once, or a single row rising by hundreds, is a bag
            // being replaced rather than filled: a load the scan did not catch,
            // a storage transfer, a quest handout. Re-baseline and leave it.
            int risen = 0, fallen = 0; long long biggest = 0;
            if (bagReplaced(snap, fresh, &risen, &fallen, &biggest))
            {
                LOG("[pet] the bag changed shape at once (%d kinds up, %d down, largest rise %lld): that is a bag being "
                    "replaced and not a companion looting, so nothing is deleted and the baseline resets", risen, fallen, biggest);
                snap.swap(fresh); snapAt = freshAt; snapHolder = freshHolder; return;
            }

            // A gather or a catch of the player's names no row, so the whole
            // pass is the player's and nothing in it is judged.
            if (HandTookUnknown(snapAt - 1500))
            {
                snap.swap(fresh); snapAt = freshAt; snapHolder = freshHolder; return;
            }
            char notice[240] = ""; int nw = 0;
            for (const auto& kv : fresh)
            {
                const auto b = snap.find(kv.first);
                const long long delta = kv.second - (b == snap.end() ? 0 : b->second);
                if (delta <= 0) continue;
                const Item* it = ItemDb::ByRow(kv.first);
                if (!it) continue;                      // unknown row: never touched
                if (HandTookRow(kv.first, snapAt - 1500)) continue;   // yours
                const Rules::Verdict r = Rules::Decide(*it, cfg);
                const bool spare = NeverDestroy(*it, r);
                if (r.loot || spare) continue;
                const long long sent = DeleteFromInventory(kv.first, delta, "companion loot, swept");
                LOG("[pet] sweep: +%lld %s arrived with a companion out and the rules refuse it (%s%s%s)",
                    delta, it->name.c_str(), r.rule, r.detail.empty() ? "" : " ", r.detail.c_str());
                if (sent > 0 && nw < static_cast<int>(sizeof notice) - 40)
                    nw += snprintf(notice + nw, sizeof notice - nw, "%s%lld %s", nw ? ", " : "", sent, it->name.c_str());
            }
            if (nw && cfg.showHud)
            {
                char msg[300];
                snprintf(msg, sizeof msg, "Master Looter: deleted %s. A companion picked it up and your rules refuse it.", notice);
                State::Get().Notify(msg, 6000, true);
            }
            snap.swap(fresh);
            snapAt = freshAt;
            snapHolder = freshHolder;
            return;
        }
        if (now - snapAt >= 400) snapAt = snapshot(snap, snapHolder);
    }

    static const char* RowName(uint16_t row)
    {
        const Item* it = ItemDb::ByRow(row);
        return it && !it->name.empty() ? it->name.c_str() : "unnamed item";
    }

    // Judges the rises LearnFromInventory held, after this scan has read the
    // world and checked it for a change.
    static void AutoStorePass(DWORD now, const Vec3& centre, float scanRange)
    {
        g_handTouches.erase(std::remove_if(g_handTouches.begin(), g_handTouches.end(),
                                           [now](const HandTouch& t) { return now - t.at > kHandMs; }), g_handTouches.end());
        // The player took by hand the very object this mod was reaching for, so
        // whatever it brings is theirs; and any item of a kind they took by hand
        // in the last twelve seconds is theirs too.
        std::unordered_set<uint16_t> handRows;
        for (const HandTouch& t : g_handTouches)
        {
            g_ownSends.erase(std::remove_if(g_ownSends.begin(), g_ownSends.end(),
                                            [&](const OwnSend& o) { return o.eid == t.eid; }), g_ownSends.end());
            const auto r = g_tidByEid.find(t.eid);
            if (r != g_tidByEid.end()) handRows.insert(r->second);
            const auto y = g_yieldByEid.find(t.eid);
            if (y != g_yieldByEid.end()) handRows.insert(y->second);
        }
        // A target in this scan's list, found by the walk or read back from the
        // cache, is still in the world. One the cache dropped because it no
        // longer reads back with its id and a position has gone from the world,
        // wherever it lay, and that is a delivery. One that still reads back
        // from beyond the scan range was left behind and says nothing about
        // whether it landed, so its entry is dropped. Anything else, missed
        // with no reading either way, is judged where it lay: well inside the
        // range is a delivery after a second and a half, within two metres of
        // the edge is dropped.
        const float edge = scanRange > 2.5f ? scanRange - 2.0f : scanRange * 0.8f;
        for (auto o = g_ownSends.begin(); o != g_ownSends.end();)
        {
            // The list keeps the nearest 256, so the walk's own refresh this
            // scan counts as present too.
            const auto sn = g_seen.find(o->eid);
            if (g_present.count(o->eid) || (sn != g_seen.end() && sn->second.when == now)) { o->goneAt = 0; o->missAt = 0; ++o; continue; }
            if (o->goneAt) { ++o; continue; }
            if (sn == g_seen.end()) { o->goneAt = o->missAt ? o->missAt : (now ? now : 1); ++o; continue; }
            {
                const float dx = sn->second.pos.x - centre.x, dy = sn->second.pos.y - centre.y, dz = sn->second.pos.z - centre.z;
                if (dx * dx + dy * dy + dz * dz > scanRange * scanRange) { o = g_ownSends.erase(o); continue; }
            }
            if (!o->missAt)
            {
                const float dx = o->pos.x - centre.x, dy = o->pos.y - centre.y, dz = o->pos.z - centre.z;
                o->missAt = now ? now : 1;
                o->missDist = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (now - o->missAt < 1500) { ++o; continue; }
            if (o->missDist >= edge) { o = g_ownSends.erase(o); continue; }
            o->goneAt = o->missAt;
            ++o;
        }
        g_ownSends.erase(std::remove_if(g_ownSends.begin(), g_ownSends.end(),
                                        [now](const OwnSend& o) { return o.goneAt && now - o.goneAt > kOwnGoneMs; }), g_ownSends.end());

        for (size_t i = 0; i < g_heldRises.size();)
        {
            HeldRise& h = g_heldRises[i];
            // Across a world change the bag is not the bag that was sampled, and
            // a save load reads as everything rising at once. A rise seen from
            // just before a change to three seconds after it is dropped; that
            // includes the scan that noticed the change, whose bag was read
            // before the change was.
            const LONG sinceChange = g_changeAt ? static_cast<LONG>(h.at - g_changeAt) : 0x7FFFFFFF;
            if ((sinceChange > -static_cast<LONG>(kRiseHoldMs) && sinceChange <= 3000) || !h.inPlay)
            {
                g_heldRises.erase(g_heldRises.begin() + static_cast<long>(i));
                continue;
            }
            // Taken by hand, or possibly: the rise is not offered, and it uses
            // up as many of this item's gone targets as it has units, so they
            // are not left over for some later rise to claim.
            if (handRows.count(h.row) || h.handUnnamed)
            {
                long long left = h.units;
                for (auto o = g_ownSends.begin(); o != g_ownSends.end() && left > 0;)
                {
                    if (o->row == h.row && o->goneAt) { --left; o = g_ownSends.erase(o); }
                    else ++o;
                }
                g_heldRises.erase(g_heldRises.begin() + static_cast<long>(i));
                continue;
            }
            bool lying = false;   // a target of this kind is still in the world
            for (auto o = g_ownSends.begin(); o != g_ownSends.end() && h.units > 0;)
            {
                if (o->row != h.row) { ++o; continue; }
                if (!o->goneAt) { lying = true; ++o; continue; }
                ++h.credited;
                --h.units;
                o = g_ownSends.erase(o);
            }
            // Still more than the gone targets explain, while one of this kind
            // lies there: it may be on its way, so wait for it a while.
            const bool waiting = h.units > 0 && lying && now - h.at < kRiseHoldMs;
            if (!waiting && h.units > 0)
            {
                if (h.fallback) h.credited += h.units;
                h.units = 0;
            }
            if (h.credited > 0)
            {
                if (psm::Deposit(h.row, h.credited))
                {
                    if (!(g_changeAt && now - g_changeAt < kTallyAfterChangeMs))
                    {
                        StoreTally& t = g_storeTally[h.row];
                        if (!t.opened) t.opened = now;
                        t.lastAt = now;
                    }
                    if (g_debugLog) LOG("[store] offered %lld %s to Private Storage Master", h.credited, RowName(h.row));
                    h.credited = 0;
                }
                else if (psm::FreePlay() == 0 && now - h.at < kRiseRetryMs)
                {
                    // Out of free play for now, a menu or a storage opened just
                    // after the pick-up landed. Offered again next scan.
                }
                else
                {
                    // Auto-store switched off is the ordinary reason and says
                    // nothing. Anything else is worth a line: a full queue, or
                    // out of free play for longer than the retry allows.
                    static int s_refusedSaid = 0;
                    if (psm::AutoStoreOn() && (g_debugLog || s_refusedSaid < 10))
                    {
                        ++s_refusedSaid;
                        LOG("[store] Private Storage Master did not take %lld %s; it stays in the bag",
                            h.credited, RowName(h.row));
                    }
                    h.credited = 0;
                }
            }
            if (waiting || h.credited > 0) { ++i; continue; }
            g_heldRises.erase(g_heldRises.begin() + static_cast<long>(i));
        }
    }

    static void Scan(const Config& cfg, bool act, bool burst)
    {
        const DWORD now = GetTickCount();
        g_scanNow = now;
        g_debugLog = cfg.debugLog;
        // Judge any break driven a second and a half ago before deciding
        // anything this pass: a node that answered with nothing stops being
        // treated as a vein from here on.
        SeedNotVeins(cfg);
        SeedNodeYields(cfg);
        ReviewBreaks(now);
        ReviewSpills(now);
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);

        const uintptr_t mgr = game::ActorManager();
        if (!mgr) { std::lock_guard<std::mutex> lk(g_mu); g_status.actorManager = false; g_status.playerFound = false; return; }

        // Player: remembered between scans and re-validated by its id tag.
        //
        // Which actor is "you" is not obvious. A party puts several
        // player-tagged actors in the world at once, and taking the first one
        // enumerated is a coin toss: playing as Damiane with Kliff following,
        // the scan centred on Kliff and the engine looked broken because it was
        // reading the wrong character's surroundings. That is almost certainly
        // the whole of "auto-loot only works as Kliff".
        //
        // The game settles it. When it runs its own take-or-steal check it
        // passes the actor it considers the player, and that id is captured
        // from the hook. Until it has asked once, the first player-tagged actor
        // is still the best guess available.
        // Note the tag is not checked. The character being played is not
        // necessarily player-tagged: as Damiane it is not.
        uint32_t id = 0;
        if (g_me && (!game::Eid(g_me, &id) || id != g_meEid)) g_me = 0;

        // There used to be a read here of the game's own pointer to the one
        // being played, the take-or-steal routine's global at a fixed RVA. The
        // RVA went stale in a patch and it answered nothing in any log since,
        // and when it did answer it named the A0 identity and switched off the
        // barren clock, which is what finds Damiane's body. DAMIANE.md rule 3.
        // Re-pick when the world around the current one is empty. Nothing in
        // range for several seconds while the player is standing in a field is
        // the signature of measuring from the wrong actor.
        // And on a timer while the centre is not a player-tagged actor. A
        // holder is only ever taken because no actor had anything around it,
        // which is a fact about one enumeration pass rather than about the
        // session, and the barren clock can never undo it: a holder wins for
        // being surrounded, so it is never barren. Issue #66.
        //
        // Every four seconds, not every scan. As Damiane the actor really is a
        // fixture with nothing near it and the holder really is the answer, so
        // this runs for the life of that session and has to stay cheap; it
        // re-enumerates the world, which the ordinary scan has already done.
        static DWORD s_holderRepick = 0;
        // And never off a played body. A holder carrying the played pair is
        // the answer as Damiane and as Oongka, not a mistake to be recovered
        // from, and moving off one costs the played flag and the hold through
        // ForgetBody. This also keeps the timer, and the extra enumeration it
        // pays for, out of those sessions entirely.
        //
        // g_bodyEid as well as g_meEid, because those two are not the same
        // question. g_me can be sitting on some crowded piece of scenery
        // while BestHolder has already put the scan on a played body, and a
        // guard that only read g_meEid would rescue the one at the cost of
        // the other.
        const bool onGearHolder = g_meEid
                                  && static_cast<uint8_t>(g_meEid >> 24) != game::kTagPlayer
                                  && !PlayedHolder(g_meEid) && !PlayedHolder(g_bodyEid)
                                  && now - s_holderRepick > 4000;
        if (onGearHolder) s_holderRepick = now;
        const bool noPlayer = !g_me;
        const bool barren = g_barrenSince && GetTickCount() - g_barrenSince > 4000;
        // A centre the single-actor rule chose is held only while that rule is
        // still true. The first pass of a session has no history behind it, so
        // it can see one player-tagged actor in a world that has two, and a
        // follower taken that way is a trap: being player-tagged it is out of
        // the rescue timer's reach, and walking with the player it never goes
        // barren. Once the window has met a second actor the choice is simply
        // re-opened, and with the rule no longer firing the count decides, which
        // is what would have happened with the fuller pass.
        static uint32_t s_soleChoice = 0;
        const bool soleDisproved = s_soleChoice && s_soleChoice == g_meEid
                                   && ActorsSeen(now) > 1;
        // A pass the timer alone asked for is a rescue and nothing more: it may
        // move the centre onto a player-tagged actor and it may do nothing else
        // whatever. Letting it choose freely would be issue #66 wearing a new
        // hat. As Damiane the centre is her body,
        // scoring perhaps 106, and the aeroplane beside it scores 410; a re-pick
        // with no margin hands the aeroplane the centre and calls ForgetBody on
        // the way out, throwing away the played flag and the hold that
        // BestHolder needs. Before this timer existed a settled centre was never
        // re-contested at all, and that part was right.
        const bool rescueOnly = !noPlayer && !barren && !soleDisproved;
        const bool reconsider = noPlayer || onGearHolder || barren || soleDisproved;
        if (reconsider)
        {
            // Every player-tagged actor, and how much world is standing near
            // each. The one being played has loaded content around it; a party
            // member parked elsewhere, or a template, does not.
            //
            // Neither of the obvious rules works. Taking the first enumerated
            // is a coin toss. Taking the one the game names in its own
            // take-or-steal check is worse: it asks that question about
            // followers too, and following it moved the scan to an actor with
            // nothing within forty metres.
            struct Pick { uintptr_t ent; uint32_t eid; Vec3 pos; int around; bool tagged; };   // not 'near': windows.h defines it
            Pick cand[16]; int candN = 0;
            static Vec3 world[2048]; int worldN = 0;
            // Everything seen this pass, so a parent can be turned back into the
            // entity that holds it.
            struct Seen { uintptr_t ent; uint32_t eid; Vec3 pos; bool posOk; };
            static Seen all[2048]; int allN = 0;
            struct Kids { uint32_t eid; int n; };
            Kids kid[64]; int kidN = 0;
            ForEachEntity(mgr, [&](uintptr_t e) {
                uint32_t eid = 0;
                if (!game::Eid(e, &eid)) return true;
                Vec3 q; const bool ok = game::WorldPos(e, &q);
                if (allN < 2048) all[allN++] = { e, eid, q, ok };
                const uint8_t tag = static_cast<uint8_t>(eid >> 24);
                if (tag == game::kTagPlayer)
                {
                    // Into the window before anything counts it. This pass is
                    // the freshest evidence there is about who is in the world,
                    // and it is fed in below rather than here, after the choice
                    // has already been made. Without this a pass that shows only
                    // a follower, while the window still remembers the identity,
                    // counts one actor and hands that follower the centre.
                    //
                    // Noted whether or not its position could be read, since the
                    // question here is how many actors exist, not which of them
                    // can be scored.
                    NoteActor(eid, now);
                }
                if (tag == game::kTagPlayer && ok)
                {
                    // Once each. The pools the roster walks overlap, and
                    // bulldog218's line lists A0100001 nine times over: nine of
                    // the sixteen slots spent on one actor, and any count of
                    // "how many player-tagged actors are there" wrong by nine.
                    bool dup = false;
                    for (int i = 0; i < candN; ++i) if (cand[i].eid == eid) { dup = true; break; }
                    if (!dup && candN < 16) cand[candN++] = { e, eid, q, 0, true };
                }
                else if (tag == game::kTagWorld && worldN < 2048 && ok) world[worldN++] = q;
                // Who is wearing or carrying things. A dressed character is the
                // one thing with a handful of items parented to it, and playing
                // as Damiane that is how the body was found: the player is not
                // player-tagged and only what hangs off it gives it away.
                if (const uint32_t par = game::ParentEid(e))
                {
                    bool had = false;
                    for (int i = 0; i < kidN; ++i) if (kid[i].eid == par) { ++kid[i].n; had = true; break; }
                    if (!had && kidN < 64) kid[kidN++] = { par, 1 };
                }
                return true;
            });
            // The three biggest holders join the candidates. They are usually
            // world-tagged, which is exactly why no tag rule ever found them.
            for (int round = 0; round < 3 && candN < 16; ++round)
            {
                int best = -1;
                for (int i = 0; i < kidN; ++i) if (kid[i].n > 0 && (best < 0 || kid[i].n > kid[best].n)) best = i;
                if (best < 0 || kid[best].n < 3) break;
                const uint32_t want = kid[best].eid; kid[best].n = 0;
                for (int i = 0; i < allN; ++i)
                    if (all[i].eid == want && all[i].posOk)
                    {
                        bool dup = false;
                        for (int c = 0; c < candN; ++c) if (cand[c].eid == want) dup = true;
                        if (!dup) cand[candN++] = { all[i].ent, all[i].eid, all[i].pos, 0, false };
                        break;
                    }
            }
            // Refuse to decide from a starved pass. The manager hands over one
            // entity on most ticks and a few hundred occasionally, so a single
            // enumeration is a lottery: the pass that picked last time saw only
            // player-tagged actors and none of the gear holders, and chose the
            // fixture again. Leaving the barren clock running means this simply
            // tries again next scan until a populated pass arrives.
            // Only when there is already something to fall back on. With no
            // player at all, a thin pass still beats no pick: a small interior
            // may never hand over fifty entities and waiting for ever would
            // leave the engine dead there.
            //
            // Decline to choose, rather than abandoning the scan to avoid
            // choosing. Returning from here leaves Scan without looting
            // anything, and the caller clears the burst flag before calling in,
            // so a Loot-all press landing on one of these passes was simply
            // swallowed. That was rare while this block was entered only when
            // there was no player or the centre had gone barren; the four-second
            // rescue above makes it routine. Emptying the candidate list has the
            // same effect on the choice and none on the rest of the scan: with
            // no candidates nothing is picked, and where there is no player at
            // all the WorldPos test a few lines down returns anyway.
            const bool thin = allN < 50 && g_me;
            if (thin)
            {
                static DWORD s_saidThin = 0;
                if (g_debugLog && now - s_saidThin > 5000)
                {
                    s_saidThin = now;
                    LOG("[player] only %d entities in this pass, too few to choose from; waiting for a fuller one", allN);
                }
                candN = 0;
            }
            const float lim = cfg.scanRange * cfg.scanRange;
            for (int i = 0; i < candN; ++i)
                for (int j = 0; j < worldN; ++j)
                {
                    const float dx = world[j].x - cand[i].pos.x, dy = world[j].y - cand[i].pos.y, dz = world[j].z - cand[i].pos.z;
                    if (dx * dx + dy * dy + dz * dz <= lim) ++cand[i].around;
                }
            // One player-tagged actor in the world, with anything at all near
            // it, is you. Take it and do not let a gear holder outbid it.
            //
            // The raw count alone put bulldog218 on the Marni airplane for a
            // whole session, 14 September 2026: the aeroplane's core carried 410
            // objects within forty metres against the A0100001 actor's 106, took
            // g_me and never gave it back, since a centre with 410 things round
            // it is never barren and nothing re-picks. Every verdict for the
            // next sixteen minutes read "worn or carried by you" or "on the
            // player", because the aeroplane's parts all hang off its core. Two
            // pick-ups in the session.
            //
            // Only when there is exactly one, and that restriction is the point.
            // Preferring any player-tagged actor over any holder reads well and
            // is wrong: as Damiane with Kliff following, Kliff is player-tagged
            // and walks through a full world, so she would win every pass, and
            // the note at the top of this function records where that leads. It
            // is also a trap with no way out, since a centre that walks never
            // goes barren and the body is dropped again within two seconds by
            // the actor-walked test below. With a party in the world this falls
            // through to the count exactly as it always has.
            //
            // Holders stay in the list for the case the fallback was written
            // for: as Damiane the played body is not player-tagged and the
            // identity fixture sits a kilometre from anything, scoring zero, so
            // the single-actor rule does not fire and the count decides as
            // before. Choosing the body over the actor is the job of g_bodyEid
            // and BestHolder below, which have the margin, the hold and the
            // played test that this pass has none of.
            int sole = -1;
            for (int i = 0; i < candN; ++i)
            {
                if (!cand[i].tagged) continue;
                if (sole >= 0) { sole = -2; break; }
                sole = i;
            }
            if (sole >= 0 && cand[sole].around <= 0) sole = -1;
            // And one pass is not a roster. The manager hands over a handful of
            // entities on most ticks, so a pass showing one player-tagged actor
            // is as likely to be a thin pass as a solo world, and picking a
            // follower here is a trap with no way out: a centre that walks never
            // goes barren, and the actor-walked test drops any body within two
            // seconds. The window is merged across scans and is empty only
            // before the first one has run.
            if (sole >= 0 && ActorsSeen(now) > 1) sole = -1;
            int best = sole >= 0 ? sole : -1;
            const bool onHolder = best < 0;
            if (onHolder)
                for (int i = 0; i < candN; ++i) if (best < 0 || cand[i].around > cand[best].around) best = i;
            // On a tie the one already being scanned around keeps it. Nothing
            // above breaks a tie except enumeration order, and bulldog218's log
            // has two holders on 410 apiece, so a draw is not hypothetical. It
            // matters because this pass now re-runs every four seconds while the
            // centre is a gear holder, which is the whole of a Damiane session,
            // and a changed pick calls ForgetBody: that empties the holder table
            // and with it the played flag and the 2.5 s hold. Swapping between
            // two equal holders every four seconds would wipe that for ever.
            int inc = -1;
            for (int i = 0; i < candN; ++i) if (cand[i].eid == g_meEid) { inc = i; break; }
            // An incumbent this pass never enumerated has not been beaten by
            // anything; it simply was not on the list. Evicting it on that is
            // how every one of these passes goes wrong, because the manager
            // hands over a few entities at a time and any single pass can omit
            // anyone. Hold until a pass arrives that can actually compare the
            // two. Twelve seconds of never being enumerated is something else,
            // an entity that has gone, and then the pass may choose freely.
            static DWORD s_incSeenAt = 0;
            if (inc >= 0) s_incSeenAt = now;
            const bool incAbsent = !noPlayer && g_meEid && inc < 0
                                   && s_incSeenAt && now - s_incSeenAt < 12000;
            if (best >= 0 && inc >= 0 && inc != best && cand[inc].around == cand[best].around
                && (cand[inc].tagged || !cand[best].tagged)) best = inc;
            if (incAbsent)
            {
                static DWORD s_saidAbsent = 0;
                if (g_debugLog && now - s_saidAbsent > 30000)
                {
                    s_saidAbsent = now;
                    LOG("[player] %08X was not in this pass at all, so it keeps the centre; nothing here can be compared with it", g_meEid);
                }
            }
            else if (rescueOnly && onHolder)
            {
                // No single player-tagged actor to be rescued onto, so hold
                // still. A rescue that fell through to the count would be free
                // to hand the centre to a follower or to the next crowded piece
                // of scenery, which is the fault it exists to undo.
                static DWORD s_saidHeld = 0;
                if (g_debugLog && now - s_saidHeld > 30000)
                {
                    s_saidHeld = now;
                    LOG("[player] still centred on %08X, a gear holder: no player-tagged actor has anything near it", g_meEid);
                }
            }
            else if (best >= 0)
            {
                // The mark is dropped when the centre moves off the disputed
                // actor, and not merely because a pass re-selected it. A pass
                // that re-selects it may simply not have enumerated the
                // alternatives, and clearing on that disarms the recovery for
                // the session on the strength of one thin look at the world.
                if (sole >= 0 && best == sole) s_soleChoice = cand[best].eid;
                else if (cand[best].eid != s_soleChoice) s_soleChoice = 0;
                const bool changed = g_meEid != cand[best].eid;
                if (g_meEid && g_meEid != cand[best].eid) ForgetBody("the player actor was re-picked");
                g_me = cand[best].ent; g_meEid = cand[best].eid;
                if (changed || g_debugLog)
                {
                    char line[240]; int w = 0;
                    for (int i = 0; i < candN && w < 200; ++i)
                        w += snprintf(line + w, sizeof line - w, " %08X:%d%s", cand[i].eid, cand[i].around, i == best ? "*" : "");
                    LOG("[player] %d candidates (player-tagged plus the biggest gear holders), world objects within %.0f m of each:%s (* is the one being scanned around%s)",
                        candN, cfg.scanRange, line,
                        onHolder ? ", a gear holder, because no player-tagged actor has anything near it" : "");
                }
                // A new actor usually means a reload, and a reload can bring a
                // new route. Sends follow the route the game raises the
                // player's events on (events.cpp); this puts the one the actor
                // itself carries beside it, so a log shows whether they agree
                // even when the player has raised nothing yet.
                if (changed)
                    LOG("[route] player actor %08X carries route %08X; sends go out on %08X", g_meEid,
                        game::Route(g_me), events::RouteKnown() ? events::Route() : 0);
            }
            // Not on a starved pass: nothing was judged, so the clock that says
            // the centre has nothing around it has not been answered.
            if (!thin) g_barrenSince = 0;
        }
        // Arm the ownership check the moment there is a player. It used to be
        // armed inside WouldSteal, the last test in Decide(), so a session
        // where nothing reached that test never armed it and could take
        // nothing until the game happened to run its own check a minute in.
        if (g_me) hooks::EnsureOwnerArmed(g_me);

        Vec3 mp;
        if (!g_me || !game::WorldPos(g_me, &mp))
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status.actorManager = true; g_status.playerFound = false;
            return;
        }
        // Stand where the body stands. Only once the player actor has proved
        // barren, so as Kliff, where the actor is the body and never barren,
        // nothing moves. Sticky once chosen while the body stays fresh, or the
        // first populated scan would clear the barren clock and the centre
        // would flap back to the fixture on the next tick. Loot events keep
        // going out as g_meEid, because that is the id the game itself raises
        // the player's events under.
        {
            const uint32_t playerRoute = events::RouteKnown() ? events::Route() : 0;
            const Holder* body = BestHolder(now, playerRoute);
            const bool actorWalks = g_actorMovedAt && now - g_actorMovedAt < 10000;
            const bool playedWalks = body && body->played && body->movedAt && now - body->movedAt < 10000 && !actorWalks;
            if (body && (g_barrenSince || playedWalks || g_bodyEid == body->eid))
            {
                if (g_bodyEid != body->eid)
                {
                    const uint32_t was = g_bodyEid;
                    g_bodyEid = body->eid;
                    s_challenger = 0;
                    if (was)
                        LOG("[player] moving the scan off %08X onto %08X, which stayed ahead for %lu ms: %d children, %d of them equipment, route %08X at %.1f %.1f %.1f",
                            was, body->eid, static_cast<unsigned long>(kBodyHoldMs), body->kids, body->gear,
                            body->route, body->pos.x, body->pos.y, body->pos.z);
                    else
                        LOG("[player] the player actor %08X has nothing around it; centring the scan on %08X instead, which carries %d children, %d of them equipment, on route %08X at %.1f %.1f %.1f",
                            g_meEid, body->eid, body->kids, body->gear, body->route,
                            body->pos.x, body->pos.y, body->pos.z);
                }
                mp = body->pos;
            }
            else if (!body && g_bodyEid)
            {
                LOG("[player] lost sight of the body %08X; back to the player actor", g_bodyEid);
                g_bodyEid = 0;
            }
        }
        g_meRoute = game::Route(g_me);
        // Where to leave what a pet's search paid: the player, since the
        // search's sender is the pet. As Damiane or Oongka the enumeration
        // publishes the body instead.
        if (!g_bodyEid) PublishBodyDropAt(g_me, g_meEid, mp, now);
        game::InventoryRefresh(g_me, !g_pend.empty());
        if (g_debugLog) game::DumpInventoryShape(g_me, g_bagFull);
        LearnFromInventory(now);
        // Issue #32. Pets and companions follow the filters, off by default.
        {
            const Config& cfg = Settings::Get();
            if (cfg.petFilter && g_me) PetFilterTick(cfg, now);
        }
        // Issue #32 probe, kept behind DeleteTestName in the ini and the
        // verbose log: delete two of a named item once through the same path
        // the pet filter uses, and say what the inventory did. The dumps
        // around it are what proved the layout on 2760; on a new build they
        // are the first thing to read.
        {
            const Config& cfg = Settings::Get();
            static int s_phase = 0; static DWORD s_sentAt = 0; static uint16_t s_tid = 0; static long long s_was = 0;
            if (cfg.debugLog && !cfg.deleteTestName.empty() && g_me && s_phase == 0)
            {
                static game::InvEntry ents[4096];
                const int n = game::InventoryEntries(g_me, ents, 4096);
                int pick = -1;
                for (int i = 0; i < n; ++i)
                {
                    const Item* it = ItemDb::ByRow(ents[i].tid);
                    if (it && !it->name.empty() && _stricmp(it->name.c_str(), cfg.deleteTestName.c_str()) == 0)
                        if (pick < 0 || ents[i].count > ents[pick].count) pick = i;
                }
                if (pick < 0) { LOG_ERR("[deletetest] nothing in the inventory is called %s; not sending", cfg.deleteTestName.c_str()); s_phase = 9; }
                else
                {
                    static game::BucketInfo bks[64];
                    const int bn = game::InventoryBuckets(g_me, bks, 64);
                    for (int i = 0; i < bn; ++i)
                        LOG("[deletetest] bucket %d at %llX: type %u, slots %u, used %u, cap %u, excluded %u%s", i, static_cast<unsigned long long>(bks[i].addr), bks[i].type, bks[i].slotN, bks[i].used, bks[i].cap, bks[i].exclN, i == ents[pick].bucket ? "  <- target" : "");
                    static game::InvTypeRow rows[96]; uint32_t rowCount = 0;
                    const int rn = game::InvTypeRows(rows, 96, &rowCount);
                    char rl[1500] = ""; int rw = 0;
                    for (int i = 0; i < rn; ++i)
                    {
                        uint16_t ks[4]; const int kn = game::InvTypeKeysFor(static_cast<uint16_t>(i), ks, 4);
                        rw += snprintf(rl + rw, sizeof rl - rw, " [%d %s: key %u]", i, rows[i].name, kn ? ks[0] : 0xFFFF);
                        if (rw > 1200 || i + 1 == rn) { LOG("[deletetest] inventory types (%u rows):%s", rowCount, rl); rw = 0; rl[0] = 0; }
                    }
                    s_tid = ents[pick].tid; s_was = 0;
                    for (int i = 0; i < n; ++i) if (ents[i].tid == s_tid) s_was += ents[i].count;
                    LOG("[deletetest] %s: %lld in the inventory; deleting two", cfg.deleteTestName.c_str(), s_was);
                    DeleteFromInventory(s_tid, 2, "delete test");
                    s_sentAt = now; s_phase = 1;
                }
            }
            else if (s_phase == 1 && events::LastDeleteSentAt() >= s_sentAt && now - static_cast<DWORD>(events::LastDeleteSentAt()) > 3000)
            {
                static game::InvEntry ents[4096];
                const int n = game::InventoryEntries(g_me, ents, 4096);
                long long have = 0;
                for (int i = 0; i < n; ++i) if (ents[i].tid == s_tid) have += ents[i].count;
                LOG("[deletetest] %s: %lld now, was %lld: %s", cfg.deleteTestName.c_str(), have, s_was, have == s_was - 2 ? "both gone, the amount field counts" : have < s_was ? "fewer, but not by two" : "no change");
                s_phase = 9;
            }
        }
        {
            // What the game armed by itself since the last scan, with the node it belongs to.
            static hooks::ArmSeen seen[64]; static int s_lines = 0;
            const int n = hooks::DrainArmSeen(seen, 64);
            for (int i = 0; i < n && s_lines < 120; ++i)
            {
                auto it = g_actorEid.find(seen[i].owner);
                if (it == g_actorEid.end()) continue;
                ++s_lines;
                const char* c3 = mem::RttiShort(seen[i].a3);
                LOG("[arm] game armed eid %08X mode %d a3 %llX (%s)", it->second, seen[i].mode, static_cast<unsigned long long>(seen[i].a3), c3 ? c3 : "no class");
            }
        }
        if (game::ItemTableState() == 0) game::ProbeItemTable();

        // Collect world objects in scan range.
        std::vector<Cand> list;
        list.reserve(256);
        int total = 0;
        int tagCount[256] = {};
        int inRange = 0;
        // The closest world object of any kind, in range or not. Nothing within
        // scan range is ambiguous on its own: it means either the player really
        // is standing in an empty place, or the position being measured from is
        // not where the player is. The distance to the nearest thing separates
        // those two immediately.
        float nearestD = 1e30f; uint32_t nearestEid = 0; uintptr_t nearestEnt = 0;
        // Whether the player actor walked this scan. See the swap test below
        // the loop.
        Vec3 ap; const bool apOk = g_me && game::WorldPos(g_me, &ap);
        {
            static Vec3 s_anchor; static bool s_anchored = false;
            if (!apOk) s_anchored = false;
            else if (Walked(s_anchor, s_anchored, ap)) g_actorMovedAt = now;
        }
        ForEachEntity(mgr, [&](uintptr_t e) {
            uint32_t eid = 0;
            if (!game::Eid(e, &eid)) return true;
            ++tagCount[eid >> 24];
            if ((eid >> 24) == game::kTagPlayer) NoteActor(eid, now);
            if ((eid >> 24) != game::kTagWorld) return true;
            for (const Cand& c : list) if (c.eid == eid) return true; // one entity sits in several lists
            ++total;
            Vec3 q;
            if (!game::WorldPos(e, &q)) return true;
            // Before the range filter, or gear on a body 1750 m away is never
            // seen and the body is never placed.
            if (const uint32_t par = game::ParentEid(e))
            {
                NoteHolder(par, q, game::Route(e), now);
                if (game::Cat2(e) == 0x11) NoteHolderWorn(par, now);
            }
            // Any holder is kept fresh by its own entity passing through, and
            // the body's pair of bytes is read on the way. The tag is one
            // deref and gates the category read, so most objects cost one.
            {
                const uint8_t tag = game::TypeTag(e);
                const bool played = tag == 0x04 && game::Cat2(e) == 0x0E;
                if (played || g_holderN) TouchHolder(eid, e, tag, q, now, played);
            }
            if (g_bodyEid && eid == g_bodyEid) PublishBodyDropAt(e, eid, q, now);
            const float dx = q.x - mp.x, dy = q.y - mp.y, dz = q.z - mp.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < nearestD) { nearestD = d; nearestEid = eid; nearestEnt = e; }
            if (d > cfg.scanRange) return true;
            ++inRange;
            g_seen[eid] = { e, q, now };
            Cand k; k.ent = e; k.eid = eid; k.d = d; k.pos = q;
            k.route = game::Route(e); k.type = game::TypeTag(e); k.parent = game::ParentEid(e);
            if (list.size() < 256) list.push_back(k);
            else
            {
                size_t worst = 0;
                for (size_t i = 1; i < list.size(); ++i) if (list[i].d > list[worst].d) worst = i;
                if (list[worst].d > d) list[worst] = k;
            }
            return true;
        });
        // Anything seen in the last eight seconds stays a candidate, read
        // afresh each scan: the entity must still carry the same id and still
        // have a position, or it is gone. This mattered most before the
        // pools above were found, when the lists gave a burst every twenty
        // seconds and a one-second window emptied the Nearby list between
        // them; it still covers an entity the walk misses for a tick.
        for (auto it = g_seen.begin(); it != g_seen.end();)
        {
            if (now - it->second.when > 12000) { it = g_seen.erase(it); continue; }
            bool have = false;
            for (const Cand& c : list) if (c.eid == it->first) { have = true; break; }
            if (!have && now - it->second.when <= 8000 && list.size() < 256)
            {
                uint32_t eid2 = 0; Vec3 q;
                if (!mem::Readable(it->second.ent, 0x100) || !game::Eid(it->second.ent, &eid2) || eid2 != it->first || !game::WorldPos(it->second.ent, &q))
                { it = g_seen.erase(it); continue; }
                it->second.pos = q;
                const float dx = q.x - mp.x, dy = q.y - mp.y, dz = q.z - mp.z;
                Cand k; k.ent = it->second.ent; k.eid = eid2; k.pos = q;
                k.d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (k.d <= cfg.scanRange) { k.route = game::Route(k.ent); k.type = game::TypeTag(k.ent); k.parent = game::ParentEid(k.ent); list.push_back(k); }
            }
            ++it;
        }
        std::sort(list.begin(), list.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
        g_present.clear();
        for (const Cand& c : list) g_present.insert(c.eid);

        // Scene transitions: the list is swapped before positions settle, and a
        // dozen objects can all read as "0.6 m away" for a moment. Hold fire.
        // Only two triggers: a position jump and a player-actor swap. The list
        // size is not one: the manager's lists breathe from scan to scan, and a
        // hold that re-arms itself on every scan froze the engine for good. Any
        // hold ends after five seconds no matter what.
        static Vec3 s_lastPos; static uintptr_t s_lastMe = 0; static bool s_havePrev = false;
        static DWORD s_holdUntil = 0, s_holdSince = 0, s_holdLogAt = 0;
        static char  s_holdWhy[96] = "";
        if (s_havePrev)
        {
            const float jx = mp.x - s_lastPos.x, jy = mp.y - s_lastPos.y, jz = mp.z - s_lastPos.z;
            const float jump2 = jx * jx + jy * jy + jz * jz;
            const char* why = nullptr; DWORD hold = 2000;
            if (jump2 > 400.0f) why = "player moved more than 20 m in one scan";
            else if (g_me != s_lastMe) { why = "player actor changed (mount, cutscene or area)"; hold = 800; }
            if (why)
            {
                g_changeAt = now;
                g_changeJumpM = std::sqrt(jump2);
                snprintf(g_changeWhy, sizeof g_changeWhy, "%s", why);
                const bool wasHeld = now < s_holdUntil;
                if (!wasHeld) s_holdSince = now;
                if (now - s_holdSince <= 5000) { s_holdUntil = now + hold; snprintf(s_holdWhy, sizeof s_holdWhy, "%s", why); }
                g_done.clear();
                // Whether taking something counts as stealing is asked on behalf
                // of a particular player, so a mount, a cutscene or an area
                // change makes every remembered answer somebody else's.
                g_ownAns.clear();
                // Pausing the scan used to leave the queues alone, and that is
                // what killed lsimo's game: a drive queued before a teleport
                // still held the component pointer of a gimmick the game had
                // freed by the time the queue drained. Nothing aimed at the old
                // world is worth keeping. g_actorEid goes with it because it
                // maps component pointers to ids and every one of those
                // pointers is now stale. Issue #35.
                // Accumulated, not sampled. DropPending runs on every scan of
                // a hold and only the first finds anything, but the line below
                // prints at most once every five seconds, so it read zero
                // essentially always: four world changes in one test session
                // produced four plain lines and no count at all.
                static int s_droppedRun = 0;
                s_droppedRun += events::DropPending();
                g_actorEid.clear();
                g_seen.clear();
                // g_actorsSeen deliberately survives this. One of the two
                // triggers above is the player actor changing, which this
                // mod's own re-pick sets off, so clearing the roster here
                // would throw away the sightings that say the re-pick chose
                // a follower, on every re-pick, for ever. It is the only
                // thing cleared in this block that holds no pointer: ids and
                // timestamps, which age out on their own in twelve seconds.
                // A spill watch is a place and a moment in the world that has
                // just gone. Whatever lies at those coordinates in the new one
                // is not what that rock paid, and a wrong entry here is written
                // to the ini for good.
                g_spillWatch.clear();
                // The bag that comes back after this is not the bag we sampled.
                InvalidateBagBaseline();
                // And the holder the game named for the old actor is a pointer
                // into the world that just went.
                game::ForgetHolder();
                // Auto-store. Rises held from before the change read a bag that
                // is not this one, and targets in the world that just went will
                // not be delivered from it.
                g_heldRises.clear();
                g_ownSends.clear();
                g_mineYields.clear();
                // A well run is the fourth holder of a raw component pointer and
                // the queues were only three of them. Winding a well is eleven
                // seconds of timed transitions spread over many scans, and
                // g_wellRun keeps the bucket's and the winch's component
                // pointers for all of it. Teleporting part way through leaves
                // WellTick driving gimmicks the world change has freed, on its
                // own schedule, which is precisely what #35 was.
                if (g_wellRun.active)
                {
                    LOG("[well] abandoning the run at %08X: the world changed under it",
                        g_wellRun.eid[WellBucket]);
                    g_wellRun.active = false;
                }
                if (now - s_holdLogAt > 5000)
                {
                    s_holdLogAt = now;
                    if (s_droppedRun) LOG("[scan] paused: %s; dropped %d queued action%s aimed at the old world",
                                          why, s_droppedRun, s_droppedRun == 1 ? "" : "s");
                    else              LOG("[scan] paused: %s", why);
                    s_droppedRun = 0;
                }
            }
        }
        static DWORD s_lastScanAt = 0;
        if (s_havePrev && now > s_lastScanAt)
        {
            const float mx = mp.x - s_lastPos.x, my = mp.y - s_lastPos.y, mz = mp.z - s_lastPos.z;
            g_moveSpeed = std::sqrt(mx * mx + my * my + mz * mz) * 1000.0f / static_cast<float>(now - s_lastScanAt);
        }
        s_lastScanAt = now;
        s_lastPos = mp; s_lastMe = g_me; s_havePrev = true;

        // What the scan actually saw, and where from. A world full of objects
        // with nothing in range means the scan is centred somewhere the player
        // is not, which is a different problem from everything being refused.
        // The tag census is here because "which actors carry the player tag"
        // turned out to be the wrong question and the right one is what the
        // character you are playing is tagged as at all.
        if (g_debugLog)
        {
            static DWORD s_saidAt = 0;
            if (now - s_saidAt > 5000)
            {
                s_saidAt = now;
                char tags[160]; int w = 0;
                for (int t = 0; t < 256 && w < 130; ++t)
                    if (tagCount[t]) w += snprintf(tags + w, sizeof tags - w, " %02X:%d", t, tagCount[t]);
                char nearBy[200] = "";
                if (nearestEnt)
                {
                    char node[160] = "";
                    if (const uintptr_t c = game::Comps(nearestEnt))
                        if (const uintptr_t gm = game::CompByClass(c, kCls_Gimmick))
                            game::NodePrefab(gm, node, sizeof node);
                    snprintf(nearBy, sizeof nearBy, "; nearest object %08X at %.1f m%s%s",
                             nearestEid, nearestD, node[0] ? " " : "", node);
                }
                // Name the entity the position actually belongs to. It read
                // "40 m of A0100001" while standing on the body, which is the
                // one confusion this whole area exists to clear up.
                LOG("[scan] %d world objects, %d within %.0f m of %08X%s at %.1f %.1f %.1f; tags:%s%s",
                    total, inRange, cfg.scanRange, g_bodyEid ? g_bodyEid : g_meEid,
                    g_bodyEid ? " (the body)" : "", mp.x, mp.y, mp.z, w ? tags : " none", nearBy);
            }
        }
        if (g_debugLog) RosterProbe(mgr, now, mp);
        // A world that is not there at all is a different fault from a world
        // that is there and out of reach, and one entity in the whole manager
        // is the first. Dump the lists the walk reads so the log says whether
        // they are empty or whether the manager itself is the wrong one.
        if (g_debugLog && total <= 2)
        {
            static DWORD s_saidMgr = 0;
            if (now - s_saidMgr > 5000)
            {
                s_saidMgr = now;
                char lists[220]; int w = 0;
                for (unsigned off = ml::sig::kOff_Mgr_ListsBegin; off + 16 <= ml::sig::kOff_Mgr_ListsEnd && w < 190; off += 8)
                {
                    uint32_t count = 0, cap = 0; uintptr_t arr = 0;
                    if (!mem::Read32(mgr + off, &count) || !mem::Read32(mgr + off + 4, &cap) ||
                        !mem::ReadPtr(mgr + off + 8, &arr)) continue;
                    if (count && cap && count <= cap && cap <= 0x10000)
                        w += snprintf(lists + w, sizeof lists - w, " +%X:%u/%u", off, count, cap);
                }
                LOG("[mgr] manager %llX (global +%llX) lists:%s", static_cast<unsigned long long>(mgr),
                    static_cast<unsigned long long>(mem::Rva(game::ActorManagerSlot())), w ? lists : " none usable");
            }
        }

        // Find the played character by following the gear.
        //
        // Whatever Damiane is tagged as, her sword and armour hang off her, and
        // a dressed character is the one thing in the world with a handful of
        // items parented to it. So group everything by its parent and report
        // the parents carrying the most. That works without knowing what tag a
        // playable character wears, which is the assumption every attempt so
        // far has been built on and none of them could justify.
        // Only worth running on a tick where the manager actually handed over
        // a world. It used to run when nothing was in range, which is exactly
        // the tick that enumerates one object and has nothing to group.
        if (g_debugLog && inRange == 0 && total > 50)
        {
            static DWORD s_saidWorn = 0;
            if (now - s_saidWorn > 3000)
            {
                s_saidWorn = now;
                struct Holder { uint32_t eid; int kids; Vec3 pos; uint32_t route; };
                Holder h[24]; int hn = 0;
                ForEachEntity(mgr, [&](uintptr_t e) {
                    uint32_t eid = 0;
                    if (!game::Eid(e, &eid)) return true;
                    const uint32_t par = game::ParentEid(e);
                    if (!par) return true;
                    for (int i = 0; i < hn; ++i) if (h[i].eid == par) { ++h[i].kids; return true; }
                    if (hn < 24) { Vec3 q; game::WorldPos(e, &q); h[hn++] = { par, 1, q, game::Route(e) }; }
                    return true;
                });
                for (int i = 0; i < hn; ++i)
                    for (int j = i + 1; j < hn; ++j)
                        if (h[j].kids > h[i].kids) { const Holder t = h[i]; h[i] = h[j]; h[j] = t; }
                char line[240]; int w = 0;
                for (int i = 0; i < hn && i < 5 && w < 200; ++i)
                {
                    const float dx = h[i].pos.x - mp.x, dy = h[i].pos.y - mp.y, dz = h[i].pos.z - mp.z;
                    w += snprintf(line + w, sizeof line - w, " %08X:%d@%.0fm/rt%08X", h[i].eid, h[i].kids,
                                  std::sqrt(dx * dx + dy * dy + dz * dz), h[i].route);
                }
                // The route is here because the player's own route is known
                // (events::Route) and gear should share it. A holder on the
                // player's route, with things hanging off it, is the body.
                LOG("[worn] things hang off these, most first:%s (scanning around %08X, player route %08X)",
                    w ? line : " nothing has a parent", g_meEid,
                    events::RouteKnown() ? events::Route() : 0);
            }
        }

        // A neighbourhood with nothing in it, while the world plainly has
        // objects, is the signature of measuring from the wrong actor. Start a
        // clock so the pick is reconsidered rather than sat on for ever.
        // "Nothing" allows a stray or two: the fixture this session had one
        // object within forty metres, and a clock that needs an exact zero
        // never started, so Damiane's body was never picked. An actor that
        // walked in the last ten seconds is Kliff and never barren.
        if (inRange <= 2 && total > 8 && !(g_actorMovedAt && now - g_actorMovedAt < 10000)) { if (!g_barrenSince) g_barrenSince = now; }
        // A pass of eight objects or fewer clears nothing. It is not
        // evidence that the centre has the world around it, any more than it
        // is evidence of the opposite, which is why the line above will not
        // start the clock on one either. This used to be reached only on
        // scans that had not re-picked, because a starved re-pick returned
        // out of the whole of Scan; now that it carries on, a run of thin
        // passes would reset the clock over and over and the barren recovery
        // would never come due.
        else if (total > 8) g_barrenSince = 0;
        // The other way round. A body is held for as long as it keeps being
        // enumerated, and a parked body is enumerated for ever: swapping from
        // Damiane to Kliff left the scan centred on her body for the rest of
        // the session, with the Nearby list empty while Kliff fought fifty
        // metres away. One sign settles it: the player actor walked. As
        // Kliff it is the body and walks with the player; as Damiane or
        // Oongka it never does. Two other signs were tried and each threw
        // her body away once: a crowd round the actor, because it stood in
        // one this session, and any movement at all, because it jumps on a
        // load. Forgetting the body puts the scan back on the actor, and a
        // swap back to Damiane or Oongka is caught by her body walking, or
        // by the barren clock.
        //
        // Unless the body is walking too. Summoning Kliff as a mercenary moves
        // the player actor, and LuxDragon's session of 22 September 2026 shows
        // what that costs: the actor walked, the scan left Damiane for Kliff,
        // her body took it back when Kliff wandered out of range, and it went
        // round again 36 times in half an hour, so looting followed whichever
        // of them the mod had last. A real swap parks the body it leaves; the
        // body being played never stands still for long while its owner is
        // moving. So the actor's walk only counts when the held body has
        // stopped.
        // What the game's own controller is driving, beside what the scan
        // chose. The swap acknowledgement's handler, +0xBFE330 in exe 2949,
        // reads the take-or-steal global +0x6D691B0, adds 0x30 and hands it
        // to the same accessor, so the object DAMIANE.md rule 3 reached, the
        // route object, is the user the accessor starts from, and the actor
        // it drives sits one hop past it. That makes it the one piece of
        // evidence that can separate a swap from a summon below; everywhere
        // else it is only logged, until a session confirms it names Damiane's
        // body while she is played.
        uint32_t ctlEid = 0;
        {
            static uint32_t s_ctlSaid = 0, s_ctlCentre = 0;
            static int s_ctlLines = 0;
            const uintptr_t ctlA = game::ControlledActor();
            if (ctlA) game::Eid(ctlA, &ctlEid);
            const uint32_t centre = g_bodyEid ? g_bodyEid : g_meEid;
            if ((ctlEid != s_ctlSaid || centre != s_ctlCentre) && s_ctlLines < 80)
            {
                ++s_ctlLines;
                s_ctlSaid = ctlEid; s_ctlCentre = centre;
                Vec3 cp{};
                const bool cpOk = ctlA && game::WorldPos(ctlA, &cp);
                LOG("[control] the game's controller drives %08X (tag %02X cat %02X)%s at %.1f %.1f %.1f; the scan is centred on %08X, "
                    "the player actor is %08X", ctlEid, ctlA ? game::TypeTag(ctlA) : 0, ctlA ? game::Cat2(ctlA) : 0,
                    ctlEid && ctlEid == centre ? ", the same" : ctlEid ? ", a different one" : "",
                    cpOk ? cp.x : 0.f, cpOk ? cp.y : 0.f, cpOk ? cp.z : 0.f, centre, g_meEid);
            }
        }
        if (g_bodyEid && g_bodyEid != g_meEid && apOk && g_actorMovedAt && now - g_actorMovedAt < 2000)
        {
            const Holder* held = nullptr;
            for (int i = 0; i < g_holderN; ++i) if (g_holders[i].eid == g_bodyEid) { held = &g_holders[i]; break; }
            const bool bodyWalks = held && held->movedAt && now - held->movedAt < 4000;
            // Both pairs, live. A companion following the one being played
            // walks too, so after a real swap to Kliff with Damiane in the
            // party the test above cannot tell the two cases apart, and
            // LuxDragon's first run of this build kept the scan on her. What
            // either body's bytes read in each case is the evidence a better
            // test needs, so it goes in the log every time.
            const uint8_t actorTag = g_me ? game::TypeTag(g_me) : 0, actorCat = g_me ? game::Cat2(g_me) : 0;
            const uint8_t bodyTag = held ? held->tagNow : 0, bodyCat = held ? held->catNow : 0;
            // Where walking cannot tell, the controller can. Kliff summoned
            // while Damiane is played, and Damiane following once Kliff is
            // swapped in, both walk the actor and the body together; the
            // controller names her body in the first and the actor in the
            // second. Anything else it says, or nothing, leaves the walking
            // rule to decide. If it turns out to name the actor even while
            // Damiane is played, the cost is the behaviour before 7a41fba, a
            // scan that follows a summoned mercenary, and not a lost body.
            const bool ctlSaysActor = ctlEid && ctlEid == g_meEid;
            const bool ctlSaysBody  = ctlEid && ctlEid == g_bodyEid;
            if (bodyWalks && ctlSaysActor)
            {
                LOG("[player] the player actor %08X walked with the body %08X walking too, and the game's controller "
                    "drives the actor, so the character was swapped (actor tag %02X cat %02X, body tag %02X cat %02X)",
                    g_meEid, g_bodyEid, actorTag, actorCat, bodyTag, bodyCat);
                ForgetBody("the game's controller drives the player actor");
            }
            else if (bodyWalks)
            {
                static DWORD s_saidAt = 0;
                if (!s_saidAt || now - s_saidAt > 30000)
                {
                    s_saidAt = now;
                    LOG("[player] the player actor %08X walked, but the body %08X is walking too, so this is a companion "
                        "of yours moving the actor and not a character swap; the scan stays on the body (actor tag %02X cat %02X, "
                        "body tag %02X cat %02X, controller %s)", g_meEid, g_bodyEid, actorTag, actorCat, bodyTag, bodyCat,
                        ctlSaysBody ? "on the body" : ctlEid ? "on neither" : "unread");
                }
            }
            else
            {
                LOG("[player] the player actor %08X walked, so it is the body being played, at %.1f %.1f %.1f; the character "
                    "was swapped (actor tag %02X cat %02X, body %08X tag %02X cat %02X)", g_meEid, ap.x, ap.y, ap.z,
                    actorTag, actorCat, g_bodyEid, bodyTag, bodyCat);
                ForgetBody("the player actor walked");
            }
        }
        const bool settling = now < s_holdUntil;
        (void)total;
        AutoStorePass(now, mp, cfg.scanRange);

        // Details for everything close enough to matter.
        float maxRange = std::max(std::max(cfg.lootRange, cfg.gatherRange), std::max(cfg.catchRange, cfg.corpseRange));
        if (cfg.autoArm) maxRange = std::max(maxRange, cfg.armRange > 0 ? cfg.armRange : cfg.gatherRange);
        maxRange = std::max(maxRange, 12.0f);
        int detailed = 0;
        for (Cand& k : list)
        {
            if (k.d > maxRange && detailed >= 24) break;
            // Either list can answer without filling: g_searched holds a raw
            // entity id for anything that never had an instance id, and
            // g_retiredEid holds the rest once they have been recognised once.
            if ((!g_debugLog || !WatchingBreak(k.eid, now)) &&
                (g_retiredEid.count(k.eid) || g_searched.count(k.eid))) { k.filled = true; k.banned = true; continue; }
            Fill(k);
            ++detailed;
            if (k.tid && k.db) { if (g_tidByEid.size() > 8192) g_tidByEid.clear(); g_tidByEid[k.eid] = k.tid; }
            else if (!k.db && k.node[0])
                if (const Item* y = NodeYield(k))
                { if (g_yieldByEid.size() > 8192) g_yieldByEid.clear(); g_yieldByEid[k.eid] = static_cast<uint16_t>(y->row); }
        }
        // Any instance id on two objects in this scan is shared from now on, and
        // it has to be known before the check below, or one sibling's retirement
        // retires them all.
        {
            std::unordered_map<uint32_t, uint32_t> firstEid;
            for (const Cand& k : list)
            {
                if (!k.filled || !k.iid) continue;
                const auto r = firstEid.emplace(k.iid, k.eid);
                if (r.second || r.first->second == k.eid || !g_sharedIid.insert(k.iid).second) continue;
                static int s_said = 0;
                if (s_said < 20)
                {
                    ++s_said;
                    LOG("[scan] %s: instance id %08X is on more than one object (%08X and %08X), so each is tracked by its own entity id",
                        Label(k), k.iid, r.first->second, k.eid);
                }
            }
        }
        // Now that Fill has read the instance id, Key() means what it says.
        // Anything already retired is noted by entity as well, so the next
        // scan takes the shortcut above instead of filling it again. Before
        // this the shortcut compared a raw entity id against a set keyed by
        // instance id, so it never matched for an object that had one and
        // every such object was filled again on every scan of the session.
        for (Cand& k : list)
            if (k.filled && !k.banned && g_searched.count(Key(k))) { k.banned = true; g_retiredEid.insert(k.eid); }
        // Note what is hanging off the player while it still is, so that it is
        // still recognisable in the seconds after it stops being. Filled above,
        // so the item row is read and can be held against the id later.
        for (const Cand& k : list)
            if (IsMine(k.parent))
            {
                OnMe& r = g_onMe[k.eid];
                r.when = now;
                if (k.tid) r.tid = k.tid;   // never trade a known row for an unread one
            }
        for (auto it = g_onMe.begin(); it != g_onMe.end(); )
        {
            if (now - it->second.when > kOnMeWindowMs) it = g_onMe.erase(it);
            else ++it;
        }
        // Container contents sit in one point; a bush comes as a data node plus an empty twin.
        //
        // A child's position is its parent's, so anything the game parents is
        // also "at one point" and is not a pile of anything. LuxDragon's
        // ginseng, 20 September 2026, issue #83: a ripe plot is six entities on
        // one spot, the planted seed, the plant parented to it, and four
        // harvestable Ginseng nodes parented to the plant, so every one of the
        // six counted the other five and all six were refused as storage. A
        // plant with three nodes stays under the threshold, which is why a ripe
        // four-node ginseng alone showed it. Relatives are not counted now,
        // siblings included.
        //
        // Measured before it was written: across 33 logs this test has fired 17
        // times and all 17 are that ginseng. It has never once refused real
        // storage contents in any log on hand, so it keeps its unparented case
        // rather than being taken out, and if a chest ever parents its contents
        // this will need the evidence that does not exist yet.
        std::unordered_map<uint32_t, size_t> byEid;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].eid) byEid.emplace(list[i].eid, i);
        // Two things are related when one is the other's ancestor or they share
        // one. Siblings matter as much as parents: LuxDragon's log on 1.6.35
        // had a plant whose four harvest nodes all hung off it, each saw the
        // other three at its point, and all four were still refused as storage
        // while the plants with three nodes went through. A parent that is
        // missing from this pass still counts, by its id. The hop limit is for
        // a parent loop in game data, not depth: the deepest chain seen is three.
        auto lineage = [&](size_t node, uint32_t (&out)[9]) {
            int n = 0;
            out[n++] = list[node].eid;
            uint32_t p = list[node].parent;
            for (int hop = 0; hop < 8 && p; ++hop)
            {
                out[n++] = p;
                auto it = byEid.find(p);
                if (it == byEid.end()) break;
                p = list[it->second].parent;
            }
            return n;
        };
        auto related = [&](size_t a, size_t b) {
            uint32_t la[9], lb[9];
            const int na = lineage(a, la), nb = lineage(b, lb);
            for (int x = 0; x < na; ++x)
                for (int y = 0; y < nb; ++y)
                    if (la[x] && la[x] == lb[y]) return true;
            return false;
        };
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (!list[i].filled) continue;
            int around = 0;
            for (size_t j = 0; j < list.size(); ++j)
            {
                if (i == j) continue;
                const float dx = list[j].pos.x - list[i].pos.x, dy = list[j].pos.y - list[i].pos.y, dz = list[j].pos.z - list[i].pos.z;
                const float dd = dx * dx + dy * dy + dz * dz;
                if (dd <= 0.0004f && !related(i, j))
                    ++around; // within 2 cm and no relation: the same point, as storage contents are
                if (!list[i].gather && !list[i].item && list[i].dead != 1 && list[i].inter && list[j].filled &&
                    (list[j].gather || list[j].item) && list[j].type == list[i].type && dd <= 0.25f) list[i].twin = true;
            }
            // around counts the others, so this is four or more at one point.
            // A loose item lying where the mod has just broken a vein is that
            // vein's spill and is exempt; anything else at one point is still
            // treated as storage.
            if (around >= 3 && !(list[i].item && NearOwnBreak(list[i].pos, now))) list[i].heap = true;
        }

        // Anything the mod broke a moment ago is being watched for what it paid,
        // and this is where a loose item on the ground is seen at all.
        if (!g_spillWatch.empty()) for (const Cand& k : list) if (k.filled) NoteSpill(k, now);
        // And what the player dropped by hand, before anything is decided.
        ClaimHandDrops(list, mp, now);

        // Decide, publish, and act.
        std::vector<Nearby> nearby;
        int lootable = 0, listed = 0;
        std::vector<Verdict> verdicts(list.size());
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (!list[i].filled) continue;
            verdicts[i] = Decide(list[i], cfg);
            if (verdicts[i].loot) ++lootable;
            // Anything the mod would take always gets a row, even on the rare
            // chance the ownership guess is wrong about it.
            if (verdicts[i].own && !verdicts[i].loot) continue;
            ++listed;
            if (nearby.size() < kNearbyRows)
            {
                Nearby n{}; n.eid = list[i].eid; n.dist = list[i].d; n.loot = verdicts[i].loot;
                strncpy(n.name, Label(list[i]), sizeof n.name - 1);
                strncpy(n.klass, list[i].db ? list[i].db->klass.c_str() : list[i].speciesClass ? list[i].speciesClass : (list[i].gather ? "gather node" : list[i].dead == 1 ? "corpse" : ""), sizeof n.klass - 1);
                if (verdicts[i].detail[0]) snprintf(n.verdict, sizeof n.verdict, "%s: %s", verdicts[i].why, verdicts[i].detail);
                else strncpy(n.verdict, verdicts[i].why, sizeof n.verdict - 1);
                n.value = list[i].db ? list[i].db->value : -1;
                if (list[i].db) strncpy(n.tags, list[i].db->tags.c_str(), sizeof n.tags - 1);
                nearby.push_back(n);
            }
        }

        // Say once per object why it was skipped, so a wrong verdict can be
        // read straight from the log without the verbose switch.
        const float diagRange = std::max(std::max(cfg.lootRange, cfg.gatherRange), std::max(cfg.catchRange, cfg.corpseRange));
        // A minute's worth at a time, rather than a total for the session.
        //
        // The old budget was 4,000 lines and nothing after them. In LuxDragon's
        // log of 14 September 2026 it ran out at 09:10:32 and the session went on
        // to 10:02:42, so fifty-two of its sixty-four minutes hold no verdict at
        // all, and the question it had been sent to answer was about something
        // that happened in the silent part. A session must never go permanently
        // blind, whatever else this costs.
        //
        // 120 a minute is two a second. The dedupe below is per object and per
        // reason, so a busy camp burns through a window and then falls quiet on
        // its own as the same things stop being news; it is new scenery that
        // costs, not standing still. An hour of that is about 1.8 MB, against
        // the 1.0 MB the old cap allowed in a session of any length.
        //
        // These lines are written whether or not the verbose switch is on, which
        // is the whole point of them, so the budget has to be a number somebody
        // would accept without having asked for a big log.
        static DWORD s_whyWindow = 0;
        static int   s_whyInWindow = 0, s_whySkipped = 0;
        if (now - s_whyWindow >= 60000)
        {
            // Say what was dropped rather than simply stopping. Going quiet
            // without a word is what made the log above unreadable: it looks
            // identical to nothing having happened.
            if (s_whySkipped)
                LOG("[why] and %d more in that minute, held back to keep the log a sensible size. "
                    "Each object is written once per reason, so these are ones that had not been seen before.",
                    s_whySkipped);
            s_whyWindow = now;
            s_whyInWindow = 0;
            s_whySkipped = 0;
        }
        for (size_t i = 0; i < list.size() && g_whyLines < kWhyCeiling; ++i)
        {
            const Cand& k = list[i];
            const Verdict& v = verdicts[i];
            if (!k.filled || v.loot || k.d > diagRange) continue;
            auto it = g_why.find(k.eid);
            if (it != g_why.end() && it->second == v.why) continue;
            // Out of budget: leave it unrecorded so it is written the moment
            // there is room, rather than marked seen and lost for good.
            if (s_whyInWindow >= kWhyPerMinute) { ++s_whySkipped; continue; }
            g_why[k.eid] = v.why;
            ++s_whyInWindow;
            ++g_whyLines;
            // "mine" is the back-reference to the player actor found in the
            // item's own fields. Printed so a log can say whether Damiane's
            // worn gear points at her, which decides whether the body can be
            // anchored on it outright instead of by counting worn items.
            LOG("[why] %08X %.1fm %s: %s%s%s | type %u tag %02X cat %02X/%02X dead %u parent %08X %s%s%s%s%s%s%s%s",
                k.eid, k.d, Label(k), v.why, v.detail[0] ? ": " : "", v.detail,
                k.tid, k.type, k.cat, k.cat2, k.dead, k.parent,
                k.inter ? "node " : "", k.item ? "item " : "", k.gather ? "gather " : "", k.ai ? "ai " : "",
                k.twin ? "twin " : "", k.heap ? "heap " : "", k.mine ? "mine " : "", k.node[0] ? k.node : "");
        }
        // Wind a well, if one is in reach and the switch is on.
        WellTick(list, cfg, now);

        // Per-entity memories grow with every object ever seen; a long session
        // sees hundreds of thousands. Forget the diagnostics wholesale and the
        // retry records once they are stale. (g_searched stays: a carcass must
        // never be searched twice, whatever the session length.)
        if (g_why.size() > 8192) g_why.clear();
        if (g_firstSeen.size() > 8192) g_firstSeen.clear();
        if (g_ownAns.size() > 4096) g_ownAns.clear();
        if (g_done.size() > 4096)
            for (auto it = g_done.begin(); it != g_done.end();)
                it = (now - it->second.when > 600000) ? g_done.erase(it) : std::next(it);

        int armedNow[32]; int armedN = 0;
        if (act && !settling)
        {
            const uint32_t route = events::RouteKnown() ? events::Route() : g_meRoute;
            // Arm empty nodes first: the game only fills a node's data when it
            // thinks the player can reach it; arming does that for us.
            if (cfg.autoArm && game::F().armFn)
            {
                const float armLim = cfg.armRange > 0 ? cfg.armRange : cfg.gatherRange;
                int armed = 0;
                for (Cand& k : list)
                {
                    if (!k.filled || k.item || k.gather || !k.inter) continue;
                    // A prefab the table vouches for as a loose item is a pick-up,
                    // and arming is what the game does to a mine or a herb patch.
                    // Forty-one arms across a hoard of gold bars, one of them at
                    // half a metre, and not one of them filled; the verdict sends
                    // the pick-up itself now. Leaving this in only spent arms on
                    // something that was never going to answer them, and it did
                    // so out to the arm range rather than the gather range the
                    // player set. LuxDragon, 13 September 2026.
                    if (k.nodeType && k.nodeType->tagged && k.nodeType->pickup) continue;
                    // Never ask the game to open a memory trigger, a puzzle mechanism or a
                    // fast-travel artifact. Refusing to loot one afterwards is too late.
                    if (OffLimits(k.node)) continue;
                    if (AttachedPart(k.node, k.nodeType)) continue;
                    // A well is seven entities and six of them never answer, so
                    // standing beside one meant seven arm calls every five seconds for
                    // as long as the player stood there. The seventh is the bucket and
                    // fills on its own once the player is close, so nothing is lost by
                    // leaving the whole thing alone.
                    if (IsMechanism(k.node)) continue;
                    // Arming one of these has never filled and never will.
                    if (StatePick(k.nodeType)) continue;
                    // Arming used to consult no looting switch whatsoever. It
                    // reached for a node whose kind the player had turned off,
                    // and the verdict that would have refused it never ran,
                    // because arming comes first. With every switch that could
                    // apply set to off, the mod still armed a damaging thorn
                    // 28 times in one session.
                    const GatherKind armKind = NodeKind(k);
                    if (GatherSwitchOff(armKind, cfg)) continue;
                    if (TreeUnsafe(armKind, k.d)) continue;
                    // And both of the questions the verdict asks, for the same
                    // reason arming consults the switches at all: nothing here
                    // has filled yet, so the verdict's own item-rule block is
                    // gated out and this is the only place the named yield of
                    // a tagged vein gets looked at before the mod reaches for
                    // it.
                    if (const Item* armYield = NodeYield(k))
                    {
                        if (!Rules::Decide(*armYield, cfg).loot) continue;
                    }
                    else if (TableYieldRefused(k, cfg) || KindRefused(armKind)) continue;
                    // Ore answers slowly and is therefore reached for sooner. Known from
                    // the prefab table, before anything is asked of the game.
                    const bool oreNode = k.nodeType && KindFromName(k.nodeType->kind) == GatherKind::Ore;
                    // Only ore the game itself tags counts as a vein. Every one of the 71
                    // ore rows is tagged now, but a table built by an older generator can
                    // still carry guesses, and a guess must not switch off with the veins
                    // or draw a vein's patience.
                    const bool vouched = oreNode && k.nodeType->tagged;
                    // Leaving the pickaxe work to the player means simply not asking: an
                    // unarmed vein holds nothing, so there is nothing to gather and the
                    // chunks they knock loose are picked up as ordinary items.
                    if (vouched && !cfg.gatherVeins) continue;
                    // A prefab that has turned down the long treatment several times over
                    // goes back to ordinary range and a single round. Still armed when the
                    // player is beside it, just not reached for across the field.
                    const bool wornOut = Unresponsive(k.nodeType);
                    const int  roundsAllowed = wornOut ? 1 : kArmRounds;
                    const float lim = (vouched && !wornOut)
                                    ? std::max(armLim, std::min(kArmRangeOre, cfg.scanRange)) : armLim;
                    if (IsMine(k.parent) || k.heap || k.d > lim) continue;
                    if (!cfg.armContainers && g_containers.count(k.eid)) continue;
                    // Same as the verdict: a classified gather node is not a
                    // container, whatever words its prefab path happens to hold.
                    if (k.node[0] && (!k.nodeType || k.nodeType->kind == "container") && !cfg.lootContainers &&
                        (IStr(k.node, "_chest") || IStr(k.node, "_box") || IStr(k.node, "dropset"))) continue;
                    if (k.node[0] && !k.nodeType && !cfg.lootFurniture && IStr(k.node, "furniture")) continue;
                    const uint64_t key = Key(k);
                    if (g_searched.count(key)) continue;
                    auto ar = g_armed.find(key);
                    if (ar != g_armed.end())
                    {
                        // Still empty after we armed it. Give the game two
                        // seconds, then count a failure; three failures and
                        // the node is not loot (a chest, a wardrobe).
                        const DWORD judgeMs = vouched ? kArmJudgeOreMs : kArmJudgeMs;
                        if (!ar->second.judged && now - ar->second.at > judgeMs)
                        {
                            ar->second.judged = true;
                            static int s_failLogs = 0;
                            if (++ar->second.fails >= kArmPerRound)
                            {
                                ar->second.fails = 0;
                                if (++ar->second.rounds >= roundsAllowed)
                                {
                                    g_searched.insert(key);
                                    if (s_failLogs < 30) { ++s_failLogs; LOG(
                                    "[arm] eid %08X %.1f m never filled after %d arms over %d round(s) (tag %02X cat2 %02X%s%s)",
                                    k.eid, k.d, kArmPerRound * roundsAllowed, roundsAllowed, k.type, k.cat2, k.node[0] ? " node " : "", k.node); }
                                    // Chalk it up against the prefab, not just this one node.
                                    if (k.nodeType)
                                    {
                                        PrefabArm& pa = g_prefabArm[k.nodeType];
                                        ++pa.exhausted;
                                        if (!pa.announced && pa.filled == 0 && pa.exhausted >= kUnresponsiveAfter)
                                        {
                                            pa.announced = true;
                                            LOG("[arm] %s has not answered arming on any of %d nodes; it keeps the ordinary range from here, and one node of it filling undoes that.",
                                    k.nodeType->prefab.c_str(), pa.exhausted);
                                        }
                                    }
                                    continue;
                                }
                                ar->second.restUntil = now + kArmRestMs;
                                if (cfg.debugLog) LOG(
                                    "[arm] eid %08X %.1f m nothing after %d arms; resting %lu s before another round",
                                    k.eid, k.d, kArmPerRound, static_cast<unsigned long>(kArmRestMs / 1000));
                            }
                        }
                        // Signed difference, so a rest survives the tick counter wrapping.
                        if (ar->second.restUntil && static_cast<LONG>(now - ar->second.restUntil) < 0) continue;
                        if (!ar->second.judged || now - ar->second.at < judgeMs + kArmRetryPadMs) continue; // wait, or cool down before re-arming
                    }
                    const uintptr_t g = game::CompByClass(game::Comps(k.ent), kCls_Gimmick);
                    if (!g) continue;
                    // What names this node will actually answer to, before asking.
                    ProbeTriggerMap(k, g);
                    ArmRec& rec = g_armed[key];
                    // The game arms with mode 1 when the player closes in and
                    // mode 0 when leaving; bushes answered 0, ore did not. Start
                    // with 1 and alternate on each retry.
                    rec.mode = 1;
                    rec.at = now; rec.judged = false;
                    static int s_armLogs = 0;
                    // The game's own 4th argument is an actor and differs on every
                    // call, which fits the node's own actor (the object that owns the
                    // gimmick component) rather than the player. Try that first, the
                    // player's actor second, and log which one a node answers to.
                    // Try 1: node actor with the game's own 3rd argument. Try 2: node
                    // actor with a zeroed buffer (what bushes accept). Try 3: the
                    // player's actor with the game's 3rd argument.
                    const uintptr_t nodeActor = game::Comps(k.ent);
                    const uintptr_t meActor   = game::Comps(g_me);
                    const int combo = kArmCombo[rec.fails % kArmPerRound];
                    const uintptr_t armCtx = (combo == 2) ? (meActor ? meActor : ArmContextNow()) : (nodeActor ? nodeActor : ArmContextNow());

                    // The id has to be one this node actually holds. Anything else misses the
                    // lookup and writes nothing, which is what every arm did until now: the
                    // mod passed 50875, and no node in the 2026-09-06 capture had it.
                    uint32_t ids[8];
                    const int nids = ReadTriggerIds(g, ids, 8);
                    // Its own ids in turn, so a node with two gets both tried across a round.
                    const uintptr_t armA3 = nids ? HoldId(ids[rec.fails % nids])
                                                 : ((combo == 1) ? 0 : hooks::ArmArg3());
                    rec.ctxKind = combo;
                    if (s_armLogs < 60) { ++s_armLogs; LOG("[arm] arming eid %08X %.1f m mode %d try %d combo %d ctx %llX id %u of %d own (tag %02X cat2 %02X%s%s)", k.eid, k.d, rec.mode, rec.fails + 1, combo, static_cast<unsigned long long>(armCtx), nids ? ids[rec.fails % nids] : 0u, nids, k.type, k.cat2, k.node[0] ? " node " : "", k.node); }
                    events::Arm(g, static_cast<uintptr_t>(rec.mode), armA3, armCtx);
                    if (armedN < 32) armedNow[armedN++] = k.eid;
                    if (cfg.debugLog) LOG("[arm] eid %08X %.1f m%s %s", k.eid, k.d,
                        vouched ? " (ore, reached for early)" : oreNode ? " (ore by name only, ordinary reach)" : "",
                        k.node[0] ? k.node : "");
                    if (++armed >= (cfg.perScan ? cfg.perScan : 8)) break;
                }
            }
            // A node that now carries data answered the arming: forget the record.
            for (Cand& k : list)
            {
                if (!k.filled || (!k.item && !k.gather)) continue;
                auto it = g_armed.find(Key(k));
                if (it == g_armed.end()) continue;
                static int s_okLogs = 0;
                if (s_okLogs < 40) { ++s_okLogs; LOG("[arm] eid %08X filled %lu ms after arming with combo %d (%s, type %u)", k.eid, static_cast<unsigned long>(now - it->second.at), it->second.ctxKind, k.gather ? "gather" : "item", k.tid); }
                // One node of a prefab answering clears any doubt about the
                // prefab, and the count of past refusals with it.
                if (k.nodeType)
                {
                    PrefabArm& pa = g_prefabArm[k.nodeType];
                    if (pa.filled == 0 && pa.announced)
                        LOG("[arm] %s answered after all; it gets the full reach again.", k.nodeType->prefab.c_str());
                    ++pa.filled;
                    pa.exhausted = 0;
                    pa.announced = false;
                }
                g_armed.erase(it);
            }

            int taken = 0;
            const int cap = burst ? (cfg.burstPerKey ? cfg.burstPerKey : 64) : (cfg.perScan ? cfg.perScan : 64);
            for (int pass = 0; pass < 2 && taken < cap; ++pass)
            {
                for (size_t i = 0; i < list.size() && taken < cap; ++i)
                {
                    Cand& k = list[i];
                    const Verdict& v = verdicts[i];
                    if (!k.filled || !v.loot) continue;
                    if ((pass == 0) != (v.act == Action::Search)) continue; // corpses first: they vanish first
                    // These hold an object back after its verdict has already
                    // passed, so without a line they are invisible: the log
                    // shows neither a skip nor a take. Debug only, and capped.
                    static int s_heldLogs = 0;
                    auto held = [&](const char* why) {
                        if (cfg.debugLog && s_heldLogs < 60) { ++s_heldLogs; LOG("[hold] %s %.1f m: %s", Label(k), k.d, why); }
                        return true;
                    };
                    bool justArmed = false;
                    for (int a = 0; a < armedN; ++a) if (armedNow[a] == k.eid) justArmed = true;
                    if (justArmed && held("armed this pass, waiting for it to fill")) continue;
                    if (v.act == Action::Gather && k.tid == 0 && AgeMs(k.eid, now) < 700 && held("still filling")) continue;
                    const uint64_t key = Key(k);
                    if (RecentlyDone(key, now, cfg.retryAfterMs) && held("sent to recently, waiting out the retry delay")) continue;
                    if (v.act != Action::Catch && SpotRecent(k.pos, k.tid, k.eid, now, cfg.retryAfterMs) && held("something of its kind was taken from this spot just now")) continue;
                    // Called where the mod has asked the game for something,
                    // whether the game took it or refused it. Never above the
                    // gates below: marking there told RecentlyDone the object
                    // had been sent to when nothing had been sent, and the
                    // object then sat out the whole retry delay. A vein first
                    // met further than 10 m away lost six seconds that way,
                    // every vein, from 1.6.26 to 1.6.33. Issue #81.
                    auto commit = [&] {
                        MarkDone(key, now);
                        if (v.act != Action::Catch) SpotMark(k.pos, k.tid, k.eid, now);
                    };
                    if (g_searched.count(key) && v.act != Action::Search) { if (cfg.debugLog) LOG("[loot] giving up on eid %08X after %d attempts", k.eid, kMaxTries); continue; }
                    if (v.act == Action::Search) g_searched.insert(key);
                    // Breaking an ore vein rather than gathering it, when asked. The mod's
                    // gather lifts the ore straight out of the node; striking it makes the
                    // game spill the contents on the ground through its own drop path and
                    // retire the node. The chunks are then ordinary ground items and get
                    // picked up as usual.
                    //
                    // The drop path pays the vein's base yield. It does not pay an equipped
                    // tool's bonus, whatever the older comment here said: nothing in the two
                    // events this drives carries a tool, and FINDINGS.md records that no
                    // tool query exists anywhere on the drop path. Issue #31.
                    // The table answers this for every node the game names: a
                    // vein carries a break impulse or a break projectile, the
                    // chunks it drops carry neither. NotAVein is the net under
                    // that, for a node the table does not know and for anything
                    // a later game patch changes.
                    if (v.pick)
                    {
                        const uintptr_t gc = game::CompByClass(game::Comps(k.ent), kCls_Gimmick);
                        const char* pn = gc ? mem::RttiShort(gc) : nullptr;
                        if (!gc || !pn || !strstr(pn, "GimmickActorComponent"))
                        {
                            held("its gimmick component no longer reads as one");
                            continue;
                        }
                        if (!events::DriveEvent(gc, kPickEvent, g_meEid, g_me, k.eid))
                        {
                            static int s_pickErr = 0;
                            if (s_pickErr < 8) { ++s_pickErr; LOG_ERR("[pick] eid %08X could not be queued", k.eid); }
                            commit();
                            continue;
                        }
                        commit();
                        // One pick per plant, the way a hand pick works: the
                        // game rolls for the yield and a miss uses the plant up
                        // just the same. LuxDragon, 17 September 2026: "if
                        // there were 10 interactions, but only 3 gave leaves...
                        // your mod found those 3".
                        g_searched.insert(key);
                        g_retiredEid.insert(k.eid);
                        ++taken;
                        // The pick is this mod's gather like any other, so what
                        // it pays is credited to it and auto-store can see it.
                        {
                            const Item* y = NodeYield(k);
                            g_pend.push_back({ now, Action::Gather, k.gtid, -1, false, std::string(), true, y ? y->row : -1 });
                            if (y && g_mineYields.size() < 256) g_mineYields.push_back({ now, static_cast<uint16_t>(y->row) });
                        }
                        if (cfg.debugLog)
                            LOG("[pick] drove the game's own pick at eid %08X %.1f m: %s", k.eid, k.d, k.node);
                        continue;
                    }
                    const bool breakIt = cfg.breakOre && v.act == Action::Gather && k.nodeType &&
                                         k.nodeType->tagged && KindFromName(k.nodeType->kind) == GatherKind::Ore &&
                                         k.nodeType->breaks && !NotAVein(k.node);
                    // One of a kind at a time while its answer is still coming.
                    if (breakIt && !NodeYield(k) && WatchingSpillFor(k.node, now) &&
                        held("waiting to see what the last one of these paid")) continue;
                    if (breakIt)
                        if (const char* unsafe = BreakUnsafe(k.d, now)) { held(unsafe); continue; }
                    if (breakIt)
                    {
                        // Drive the game's own state machine at the node: the swing
                        // landing, then the break. The transition is what spawns the
                        // ore, so the vein empties itself and then disappears, which
                        // the drop event on its own never made it do.
                        const uintptr_t gc = game::CompByClass(game::Comps(k.ent), kCls_Gimmick);
                        // A component the game has freed has lost its vtable
                        // name. Ask before driving it, not after the fault.
                        const char* gcName = gc ? mem::RttiShort(gc) : nullptr;
                        if (!gcName || !strstr(gcName, "GimmickActorComponent"))
                        {
                            held("its gimmick component no longer reads as one; the area is unloading");
                            continue;
                        }
                        if (!events::DriveBreak(gc, g_meEid, g_me, k.eid, k.pos.x, k.pos.y, k.pos.z))
                        {
                            // Its own budget: the shared hold log is spent on
                            // "still filling" long before a break failure would show.
                            static int s_brkErr = 0;
                            if (s_brkErr < 8) { ++s_brkErr; LOG_ERR("[break] eid %08X could not be queued", k.eid); }
                            commit();
                            continue;
                        }
                        // Once per vein and no more. The drop event spills what the
                        // node holds but does not consume the node, so the vein stays
                        // in the world and stays a candidate; without this it is
                        // struck again on every retry window and pays out every time.
                        // Ten of thirty veins were harvested two to four times over in
                        // the 20:26 session, which is duplication rather than mining.
                        // A vein the game respawns returns as a new entity, so retiring
                        // this one does not bar it for good.
                        g_searched.insert(key);
                        // Judged a second and a half from now: a vein will have
                        // dropped by then, and anything that has not is not one.
                        if (g_drivenBreaks.size() > 32) g_drivenBreaks.erase(g_drivenBreaks.begin());
                        DrivenBreak rec{ k.eid, key, now, "" };
                        snprintf(rec.node, sizeof rec.node, "%s", k.node);
                        g_drivenBreaks.push_back(rec);
                        // What falls out lands here. NearOwnBreak reads this so the
                        // spill is not mistaken for the contents of a container,
                        // and WatchSpill reads it to find out what this prefab
                        // actually holds when the table does not say.
                        BrokeMark(k.pos, now);
                        if (!NodeYield(k)) WatchSpill(k.node, k.pos, k.eid, now);
                        if (g_debugLog)
                        {
                            if (g_brokeWatch.size() > 64) g_brokeWatch.clear();
                            g_brokeWatch[k.eid] = now;
                        }
                    }
                    // A skin the mod does earns no knowledge of the creature,
                    // because the game raises that reward from the hand skin's own
                    // completion and the search alone never reaches it. Two hand
                    // skins rewarded against none of 24 mod skins, issue #70. So a
                    // carcass whose species the table names exactly carries the
                    // reward with it. Only exact: a species guessed from a shared
                    // word would teach the player about the wrong animal.
                    else if (!events::Send(v.act, k.eid, g_meEid, route, 0,
                                           v.act == Action::Search && k.speciesExact && k.species ? k.species->skin : 0,
                                           v.act == Action::Search && k.speciesExact && k.species ? k.species->key : 0))
                    { commit(); held("the game refused the event"); continue; }
                    commit();
                    ++taken;
                    {
                        const Item* y = v.act == Action::Gather ? NodeYield(k) : nullptr;
                        g_pend.push_back({ now, v.act, v.act == Action::Gather ? k.gtid : static_cast<uint16_t>(0), k.db ? k.db->row : -1, false,
                                           std::string(), true, y ? y->row : -1 });
                        if (y && !k.db && g_mineYields.size() < 256) g_mineYields.push_back({ now, static_cast<uint16_t>(y->row) });
                    }
                    if (k.db && k.db->row >= 0)
                    {
                        auto o = std::find_if(g_ownSends.begin(), g_ownSends.end(), [&](const OwnSend& e) { return e.eid == k.eid; });
                        if (o != g_ownSends.end()) { o->sentAt = now; o->goneAt = 0; o->missAt = 0; o->pos = k.pos; }
                        else
                        {
                            if (g_ownSends.size() >= 512) g_ownSends.erase(g_ownSends.begin());
                            g_ownSends.push_back({ static_cast<uint16_t>(k.db->row), k.eid, now, 0, k.pos, 0, 0.0f });
                        }
                    }
                    InterlockedIncrement(&g_session[static_cast<int>(v.act)]);
                    // Wide enough for a name and a distance. At 80 a long prefab
                    // path consumed the buffer and the distance was truncated away,
                    // which is how ten ore pickups were logged with no range at all.
                    char line[160];
                    if (breakIt) snprintf(line, sizeof line, "break %s (%.1f m)", Label(k), k.d);
                    else         snprintf(line, sizeof line, "%s %s (%.1f m)", events::ActionName(v.act), Label(k), k.d);
                    PushRecent(line);
                    // The table row that let this through, so a session can be
                    // read back to the rows rather than guessed at: which kind
                    // it was filed under and whether the game's tag, a name
                    // guess or a sighting put it there.
                    LOG("[loot] %s eid %08X type %u parent %08X cat %02X/%02X %s%s%s%s", line, k.eid, k.tid, k.parent, k.cat, k.cat2, k.key[0] ? k.key : "",
                        k.nodeType ? (" [" + k.nodeType->kind + "/" + (k.nodeType->tagged ? "vouched" : "name") + "]").c_str() : "",
                        k.node[0] ? " node " : "", k.node[0] ? k.node : "");
                    if (cfg.dupeProbe) DupeOpen(g_me, k.eid, k.iid, k.tid, k.pos, Label(k), k.node, now);
                }
            }
        }

        if (cfg.dupeProbe) DupeMature(g_me, now, mp, cfg.scanRange);

        QueryPerformanceCounter(&t1);
        std::lock_guard<std::mutex> lk(g_mu);
        // The actor manager keeps its entities in several arrays and hands out
        // a count and a capacity for each. A sweep that catches one being
        // resized sees a count of zero, or a count past the capacity, and skips
        // it, so a scan now and then comes back with nothing while the world is
        // plainly still there. Publishing that emptied the Nearby table for a
        // frame, which is the blinking people see.
        //
        // A sweep that finds nothing is therefore not believed straight away.
        // The last list that had something in it stays up, with the counts that
        // went with it, until two seconds of nothing agree that the world
        // really is empty, which is what loading a new area looks like.
        static DWORD s_lastGood = 0;
        const bool nothingFound = nearby.empty() && list.empty();
        if (!nothingFound || now - s_lastGood > 2000)
        {
            if (!nothingFound) s_lastGood = now;
            g_nearby.swap(nearby);
            g_status.candidates = static_cast<int>(list.size());
            g_status.listed = listed;
            g_status.lootable = lootable;
        }
        g_status.actorManager = true;
        g_status.playerFound = true;
        g_status.playerEid = g_meEid;
        g_status.settling = settling;
        snprintf(g_status.hold, sizeof g_status.hold, "%s", settling ? s_holdWhy : "");
        g_status.inventoryItems = game::InventoryCount();
        g_status.bagFull = g_bagFull;
        int bagUsed = 0, bagCap = 0;
        if (game::BagSlots(&bagUsed, &bagCap)) { g_status.bagUsed = bagUsed; g_status.bagSlots = bagCap; }
        else { g_status.bagUsed = g_status.bagSlots = 0; }
        g_status.learned = static_cast<int>(g_learn.size());
        g_status.lastScanMs = static_cast<float>((t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
        ++g_status.scans;
    }

    // -------------------------------------------------------------- worker ----
    static bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

    static DWORD WINAPI Worker(LPVOID)
    {
        Note("resolving game functions");
        const bool ok = game::ResolveAll();
        { std::lock_guard<std::mutex> lk(g_mu); g_status.resolved = ok; }
        if (!ok) { Note("required signatures missing; looting disabled"); LOG_ERR("[loot] required signatures missing; the loot engine is disabled this session"); return 0; }
        const bool hooked = hooks::Install();
        { std::lock_guard<std::mutex> lk(g_mu); g_status.hooked = hooked; g_status.pump = hooks::PumpName(); }
        if (!hooked) { Note("no game-thread pump; looting disabled"); return 0; }
        DropLearnedFile();
        Note("waiting for the world");
        LOG_OK("[loot] engine ready; pump: %s", hooks::PumpName());

        bool toggleWas = false, burstWas = false, ownedWas = false;
        bool menuWas = false, saidNoFrame = false;
        while (InterlockedCompareExchange(&g_running, 0, 0))
        {
            ml::Mod::ReportSecondCopy();
            ml::Mod::ReportOtherLootMod();
            // A copy: the render thread edits the live Config while the menu is up.
            Config cfg = Settings::Snapshot();
            RefreshKindRules(cfg);
            const State& st = State::Get();
            if (!st.Captures() && State::ForegroundIsOurs())
            {
                // Either the key or the pad shortcut, whichever the player set.
                // The edge is taken on the key itself and the modifier test only
                // decides whether that edge fires, so letting go of Ctrl while
                // F10 is still down does not toggle anything either.
                const bool tp = ml::hooks::PadChordHeld(cfg.padToggle);
                const bool t = KeyDown(cfg.keyToggle) || tp;
                if (t && !toggleWas && (tp || State::HotkeysFree(cfg.keyToggle))) { SetAuto(!cfg.enabled); cfg.enabled = !cfg.enabled; }
                toggleWas = t;
                const bool bp = ml::hooks::PadChordHeld(cfg.padBurst);
                const bool b = KeyDown(cfg.keyBurst) || bp;
                if (b && !burstWas && (bp || State::HotkeysFree(cfg.keyBurst))) RequestBurst();
                burstWas = b;
                // Unbound reads as nothing held, so an unset key never fires.
                const bool op = cfg.padOwned && ml::hooks::PadChordHeld(cfg.padOwned);
                const bool o = (cfg.keyOwned && KeyDown(cfg.keyOwned)) || op;
                if (o && !ownedWas && (op || State::HotkeysFree(cfg.keyOwned))) { SetLootOwned(!cfg.lootOwned); cfg.lootOwned = !cfg.lootOwned; }
                ownedWas = o;
                // The menu key is read on the render path, which runs only
                // once a frame has reached this mod. With no frame it is read
                // nowhere, and a press used to leave no trace at all: four
                // players reported a dead Insert on 25 September 2026 and the
                // one log sent had not a single [menu] line in three hours.
                // Watched here only until the menu has a frame, and said once.
                if (!saidNoFrame && !ml::hooks::MenuHasFrame())
                {
                    const bool mp = ml::hooks::PadChordHeld(cfg.padMenu);
                    const bool m = KeyDown(cfg.menuKey) || mp;
                    if (m && !menuWas && (mp || State::HotkeysFree(cfg.menuKey)))
                    {
                        saidNoFrame = true;
                        const char* who = ml::Mod::OverlayConflict();
                        LOG_ERR("[menu] %s was pressed, but the menu has not had a single frame to draw on "
                                "since the game started, so it cannot open. Looting is unaffected. %s%s",
                                mp ? "The menu chord" : Settings::KeyName(cfg.menuKey),
                                who ? who : "The [hook] lines near the top of this log say which mod holds the game's drawing calls.",
                                who ? " is loaded, and Character Creator 9's editor panel is known to do this: a text "
                                      "file called disable.txt containing the word overlay in bin64\\CharacterCreator "
                                      "switches that panel off." : "");
                    }
                    menuWas = m;
                }
            }
            if (InterlockedExchange(&g_forget, 0))
            {
                g_learn.clear();
                // What a prefab has refused is learned too, so the same button
                // clears it and every prefab gets its full reach back.
                g_prefabArm.clear();
                LOG("[learn] node yields forgotten, and every prefab has its arming reach back");
            }
            const bool burst = InterlockedExchange(&g_burst, 0) != 0;
            const bool want = cfg.enabled || burst || st.menuOpen;
            if (want && !g_scanDisabled) ScanGuarded(cfg, cfg.enabled || burst, burst);
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_status.sendAllowed = events::SendAllowed();
                g_status.descriptors = events::DescriptorsFound();
                g_status.ownerOracle = hooks::OwnerCaptured();
                g_status.routeKnown  = events::RouteKnown();
                g_status.itemTable   = game::ItemTableState();
                g_status.sent        = events::SentCount();
                g_status.faults      = mem::FaultCount();
                g_status.pumpTicks   = hooks::PumpTicks();
                if (g_status.playerFound) strncpy(g_status.note, cfg.enabled ? "looting" : "idle", sizeof g_status.note - 1);
            }
            const int sps = std::clamp(cfg.scansPerSec, 1, 30);
            const DWORD nap = want ? static_cast<DWORD>(1000 / sps) : 100;
            for (DWORD slept = 0; slept < nap;)
            {
                const DWORD step = std::min<DWORD>(nap - slept, 100);
                Sleep(step);
                slept += step;
                if (want) SampleFreePlay(GetTickCount());
            }
        }
        return 0;
    }

    void Start()
    {
        if (g_thread) return;
        InterlockedExchange(&g_running, 1);
        { std::lock_guard<std::mutex> lk(g_mu); g_status.started = true; }
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Stop()
    {
        InterlockedExchange(&g_running, 0);
        if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
        hooks::Remove();
    }

    // On the game thread. Besides draining events, it asks the game which bag
    // the scanned actor uses, four times a second, because as Damiane or Oongka
    // it is not the actor's own and only the game can say whose it is.
    void OnGameTick()
    {
        events::Drain();
        static DWORD s_holderAt = 0;
        const DWORD now = GetTickCount();
        if (now - s_holderAt < 250) return;
        s_holderAt = now;
        game::RefreshHolder(g_me);
    }

    Status GetStatus() { std::lock_guard<std::mutex> lk(g_mu); return g_status; }
    int CopyNearby(Nearby* out, int max)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const int n = std::min(max, static_cast<int>(g_nearby.size()));
        for (int i = 0; i < n; ++i) out[i] = g_nearby[i];
        return n;
    }
    int CopyRecent(Recent* out, int max)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const int n = std::min(max, static_cast<int>(g_recent.size()));
        for (int i = 0; i < n; ++i) out[i] = g_recent[i];
        return n;
    }
    long SessionCount(int action) { return (action >= 0 && action < 4) ? g_session[action] : 0; }
    void ForgetLearned() { InterlockedExchange(&g_forget, 1); }   // the worker owns the table; it clears it on its next pass
    // Both logged, because a report that auto-loot would not turn off came
    // with a log that could not say whether it had ever been on.
    void RequestBurst()
    {
        InterlockedExchange(&g_burst, 1);
        LOG("[loot] looting everything in range once, on the burst key");
        State::Get().Notify("Master Looter: looting everything in range", 1500);
    }
    void SetAuto(bool on)
    {
        { std::lock_guard<std::recursive_mutex> lk(Settings::Mutex()); Settings::Get().enabled = on; Settings::MarkDirty(); }
        LOG("[loot] auto-loot switched %s by its key", on ? "on" : "off");
        State::Get().Notify(on ? "Master Looter: auto-loot on" : "Master Looter: auto-loot off");
    }

    // Said plainly, because the cost of leaving this on is a bounty and the
    // player has to be able to tell at a glance which way they just flipped it.
    void SetLootOwned(bool on)
    {
        { std::lock_guard<std::recursive_mutex> lk(Settings::Mutex()); Settings::Get().lootOwned = on; Settings::MarkDirty(); }
        // Every remembered take-or-steal answer was given about the other case.
        g_ownAns.clear();
        State::Get().Notify(on ? "Master Looter: taking owned goods, the game calls this stealing"
                               : "Master Looter: leaving owned goods alone", 2500);
    }
}
