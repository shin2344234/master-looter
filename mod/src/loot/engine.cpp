#include "engine.h"

#include <algorithm>
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
#include "../core/itemdb.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../core/rules.h"
#include "../core/settings.h"
#include "../core/state.h"

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
    struct Verdict { bool loot = false; bool own = false; Action act = Action::Take; const char* why = ""; char detail[40] = ""; };

    // Memories. Keys: instance id when the node has one (survives respawns),
    // otherwise the entity id.
    static uint64_t Key(const Cand& c) { return c.iid ? (0x100000000ull | c.iid) : c.eid; }
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
    static std::unordered_map<uint32_t, DWORD> g_firstSeen; // eid -> when first listed
    static std::unordered_set<uint32_t>        g_containers;
    struct Spot { Vec3 p; uint16_t tid; DWORD when; uint32_t eid; };
    static std::vector<Spot> g_spots;                       // recently sent, by place and type
    static std::unordered_set<uint32_t> g_present;          // eids seen in the current scan
    struct Seen { uintptr_t ent; Vec3 pos; DWORD when; };
    static std::unordered_map<uint32_t, Seen>  g_seen;      // merges the game's partial lists

    static bool WasSeen(uint32_t eid) { return g_seen.count(eid) != 0; }
    static const char* LastVerdict(uint32_t eid)
    {
        auto it = g_why.find(eid);
        return it == g_why.end() ? nullptr : it->second;
    }
    static std::unordered_map<uint32_t, uint16_t> g_nodeType; // eid -> gather node type, from the last scans
    static std::unordered_map<uintptr_t, uint32_t> g_actorEid; // node actor -> eid, from the last scans
    static std::unordered_map<uint32_t, DWORD> g_slotProbeAt;  // eid -> last gimmick slot dump

    static uintptr_t g_me = 0;
    // Declared here, above the body helpers that read it.
    static uint32_t  g_meEid = 0, g_meRoute = 0;
    // When the scan last found nothing at all around the chosen actor.
    static DWORD g_barrenSince = 0;

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
        Vec3  pos;
        uint32_t route;
        int Score() const { return gear * 8 + kids; }
    };
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
                    if (e && (e == g_bodyEid || e == g_meEid)) continue;
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
    static void TouchHolder(uint32_t eid, const Vec3& at, DWORD now)
    {
        for (int i = 0; i < g_holderN; ++i)
            if (g_holders[i].eid == eid) { g_holders[i].seen = now; g_holders[i].pos = at; return; }
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
        if (!best || best->eid == incumbent->eid) { s_challenger = 0; return incumbent; }

        // Parts are not gear. Something that has never had a single child
        // classify as an item does not take the centre from something that
        // has, however many pieces it is built from. A steam engine enumerates
        // thirty-two parts and a dressed character seven items, and the score
        // already says the character wins; this is the backstop for when it
        // does not, because a big enough object can out-count seven items on
        // raw children alone.
        if (best->gear == 0 && incumbent->gear > 0) { s_challenger = 0; return incumbent; }
        // And its mirror. Something with gear takes the centre from something
        // without, whatever the counts say, after the same hold. In the same
        // log a prison wagon kept the centre for eighty seconds against the
        // real body because the body's score did not clear the margin.
        const bool gearOverParts = best->gear > 0 && incumbent->gear == 0;

        // Half again as much, and two more outright, or it is noise.
        const int mine = incumbent->Score(), theirs = best->Score();
        if (!gearOverParts && (theirs < mine + 2 || theirs * 2 < mine * 3)) { s_challenger = 0; return incumbent; }

        if (s_challenger != best->eid) { s_challenger = best->eid; s_challengeSince = now; s_challengeSaid = false; }
        if (now - s_challengeSince < kBodyHoldMs) return incumbent;
        // Both sides of the decision, once. The caller's line names the winner
        // and where it stands; this one says what it beat and by how much.
        if (!s_challengeSaid)
        {
            s_challengeSaid = true;
            LOG("[player] %08X (%d children, %d equipment, score %d) takes the centre from %08X (%d children, %d equipment, score %d)%s",
                best->eid, best->kids, best->gear, theirs, incumbent->eid, incumbent->kids, incumbent->gear, mine,
                gearOverParts ? ", on gear alone" : "");
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

    static std::unordered_map<uint16_t, uint16_t> g_learn;   // node type -> item row
    struct PendSend { DWORD at; Action act; uint16_t nodeType; int itemRow; bool counted = false; };
    static std::vector<PendSend> g_pend;
    static std::vector<std::pair<uint16_t, long long>> g_invPrev;
    static bool g_invPrevValid = false;

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
    static const Item* LearnedYield(uint16_t nodeType)
    {
        auto it = g_learn.find(nodeType);
        return it == g_learn.end() ? nullptr : ItemDb::ByRow(it->second);
    }

    // Diff the bag against the last scan and attribute every rise to a pending send.
    static void LearnFromInventory(DWORD now)
    {
        // What the player gathered or caught by hand counts as a pending send
        // too, so nodes get identified without the mod ever gathering them.
        static events::Seen seen[32];
        const int sn = events::DrainSeen(seen, 32);
        for (int i = 0; i < sn; ++i)
        {
            uint16_t nodeType = 0;
            if (seen[i].act == Action::Gather) { auto it = g_nodeType.find(seen[i].eid); if (it != g_nodeType.end()) nodeType = it->second; }
            const Item* known = nullptr;
            if (seen[i].act == Action::Gather && nodeType) known = LearnedYield(nodeType);
            if (known) continue; // nothing new to learn from it
            g_pend.push_back({ static_cast<DWORD>(seen[i].at), seen[i].act, nodeType, -1, false });
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
                if (why) { if (Settings::Get().debugLog) LOG("[learn] player %s eid %08X (node type %u), we had skipped it: %s", events::ActionName(seen[i].act), seen[i].eid, nodeType, why); }
                else if (WasSeen(seen[i].eid)) { if (Settings::Get().debugLog) LOG("[learn] player %s eid %08X (node type %u), the scan had it and did not act", events::ActionName(seen[i].act), seen[i].eid, nodeType); }
                else if (s_missLogs < 40) { ++s_missLogs; LOG("[learn] player %s eid %08X (node type %u), the scan never saw it", events::ActionName(seen[i].act), seen[i].eid, nodeType); }
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
            bool same = true;
            for (size_t i : unknown) if (g_pend[i].nodeType != nodeType) same = false;
            if (!same || rose.size() > 1) continue; // ambiguous: wait for a cleaner sample
            if (!g_learn.count(nodeType))
            {
                g_learn[nodeType] = type;
                const Item* it = ItemDb::ByRow(type);
                LOG("[learn] node type %u yields %s (%s)", nodeType, it ? it->Label() : "?", it ? it->klass.c_str() : "");
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
    enum class GatherKind { Unknown, Plant, Crop, Ore, Stone, Wood, Item, Furniture };
    // What an item counts as for the kind toggles. The classes come straight
    // from the item database (scripts/build_item_db.py): ore and jewel are
    // minerals from veins, stone from quarries, wood from trees and branches.
    // "Plant" means herbs, flowers and mushrooms; a crop (a vegetable, fruit or
    // grain) is food and has its own toggle, on the plant or lying loose.
    // `onGround`: an item lying in the world rather than a node's yield.
    static GatherKind KindFromName(const std::string& kind)
    {
        if (kind == "plant") return GatherKind::Plant;
        if (kind == "ore")   return GatherKind::Ore;
        if (kind == "stone") return GatherKind::Stone;
        if (kind == "wood")  return GatherKind::Wood;
        if (kind == "item")  return GatherKind::Item;
        return GatherKind::Unknown;
    }

    // What a gather node hands over, when anything says so. The prefab table
    // names the item for the nodes whose socket and item share a name; the
    // rest are known only by kind, and a bag diff may have caught one earlier
    // in this session.
    static const Item* NodeYield(const Cand& c)
    {
        if (c.nodeType && !c.nodeType->itemKey.empty())
            if (const Item* it = ItemDb::ByStringKey(c.nodeType->itemKey.c_str())) return it;
        return LearnedYield(c.gtid);
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
        // Ahead of the onGround line, so a fallen apple and one still on the
        // tree answer to the same switch. Before 1.3.1 both were ground items.
        if (k == "vegetable" || k == "fruit" || k == "grain") return GatherKind::Crop;
        if (onGround) return GatherKind::Item;
        if (k == "seed" || k == "alchemy-material" || y->HasTag("rare-gather")) return GatherKind::Plant;
        return GatherKind::Item;
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
        if (kind == GatherKind::Unknown || kind == GatherKind::Item)
        {
            const GatherKind byYield = KindOf(c.db ? c.db : NodeYield(c));
            if (byYield != GatherKind::Unknown) kind = byYield;
        }
        return kind;
    }

    // The switch that owns this kind, or null when it is on. Both the verdict
    // and the arm loop ask, and they must give the same answer: arming a node
    // the verdict will refuse is work done for nothing at best, and on a
    // damaging thorn it half-transitions the vine and leaves the volume that
    // hurts the player standing in thin air. Issue #41.
    static const char* GatherSwitchOff(GatherKind kind, const Config& cfg)
    {
        switch (kind)
        {
        case GatherKind::Plant:     return cfg.gatherPlants   ? nullptr : "plants off";
        case GatherKind::Crop:      return cfg.gatherCrops    ? nullptr : "crops off";
        case GatherKind::Ore:       return cfg.gatherOre      ? nullptr : "ore off";
        case GatherKind::Stone:     return cfg.gatherStone    ? nullptr : "stone off";
        case GatherKind::Wood:      return cfg.gatherWood     ? nullptr : "wood off";
        case GatherKind::Item:      return cfg.pickUpItems    ? nullptr : "pick up off";
        case GatherKind::Furniture: return cfg.lootFurniture  ? nullptr : "furniture off";
        default:                    return cfg.gatherUnknown  ? nullptr : "unidentified nodes off";
        }
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
    // Timings are milliseconds from the first transition of the captured run.
    // The repeats of D08DADE4 and EB0F048C are what the log showed, and the log
    // caps at three samples per id, so the real run may hold more of them. If a
    // replay stalls part way that is the first thing to suspect.
    static const WellStep kWellRun[] = {
        {     0, WellWinch,     0x92A049DE },
        {     9, WellWinchPart, 0xA4078B62 },
        {  1470, WellWinch,     0xA327105E },
        {  1479, WellWinch,     0xD08DADE4 },
        {  3013, WellWinch,     0xD08DADE4 },
        {  4553, WellWinch,     0xD08DADE4 },
        {  5106, WellWinch,     0xA327105E },
        {  5293, WellWinch,     0xEB0F048C },
        {  6828, WellWinch,     0xEB0F048C },
        {  8184, WellWinch,     0xA327105E },
        {  8238, WellWinch,     0xA809EFDF },
        {  8247, WellWinchPart, 0xF00346BD },
        {  8663, WellWinch,     0x0F7569ED },
        {  8673, WellWinchPart, 0xF00346BD },
        { 11115, WellBucket,    0x003ECC59 },
    };
    static constexpr int kWellRunSteps = static_cast<int>(sizeof kWellRun / sizeof kWellRun[0]);

    // What the mod actually drives, and why it is one step and not fifteen.
    //
    // Winding the winch works: the run above was driven back to back nineteen
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
    static DWORD AgeMs(uint32_t eid, DWORD now)
    {
        auto it = g_firstSeen.find(eid);
        if (it == g_firstSeen.end()) { g_firstSeen[eid] = now; return 0; }
        return now - it->second;
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

    static bool OffLimits(const char* node)
    {
        if (!node || !node[0]) return false;
        // "puzzle" earns its place: the game tags gimmick_puzzle_ice_wall_break,
        // _ice_block_break, _stone_wall_break and _pickaxe_break_point as
        // collect_mine, so they read as ordinary ore and the mod would open
        // them. Breaking the wall is the puzzle; solving it is the player's.
        // "woodthorn" is the damaging vine of issue #41. Arming one drives a
        // transition the node does not declare: the vine's visual goes and the
        // volume that hurts the player stays, which left the Duskwood pillar
        // puzzle unfinishable. It holds nothing, so nothing is lost by never
        // touching it, and this stands even when Unidentified nodes is on.
        static const char* kWords[] = { "visione", "quest", "artifact", "abyssruins", "mission", "puzzle", "woodthorn" };
        for (const char* w : kWords) if (IStr(node, w)) return true;
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
        if (k.item && k.parent) NoteHolderGear(k.parent, g_scanNow);
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
        const bool nameable = k.cat2 == 0x05 || k.cat2 == 0x09 || k.cat2 == 0x08;
        if (k.ai && !k.inter && (nameable || g_debugLog))
        {
            const uintptr_t aiComp = game::CompByClass(comps, kCls_Ai);
            const Species sp = FindSpecies(k.eid, k.ent, comps, status, aiComp, k.cat2, k.type);
            k.species = sp.row; k.speciesClass = sp.klass; k.speciesExact = sp.exact;
            // Only for the ones that stayed anonymous, which is the case worth
            // solving, and only with the debug log on.
            if (g_debugLog && !sp.exact) ProbeCreatureIdentity(k.eid, k.ent, comps, status, aiComp, k.cat2, sp.klass);
        }
        if (k.gather && k.gtid) { if (g_nodeType.size() > 4096) g_nodeType.clear(); g_nodeType[k.eid] = k.gtid; }
        if (g_actorEid.size() > 4096) g_actorEid.clear();
        g_actorEid[comps] = k.eid;
        // An empty node close by that will not fill: dump which of its gimmick
        // slots hold pointers, so a node the game fills elsewhere shows up.
        if (g_debugLog && inter && !k.item && !k.gather && k.cat2 == 0x00 && k.d < 4.0f)
        {
            static int s_dumps = 0;
            const DWORD nowp = GetTickCount();
            auto it = g_slotProbeAt.find(k.eid);
            if (s_dumps < 40 && (it == g_slotProbeAt.end() || nowp - it->second > 3000))
            {
                ++s_dumps; g_slotProbeAt[k.eid] = nowp;
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
        if (g_debugLog && k.gather && k.gtid && !k.nodeType && !LearnedYield(k.gtid)) ProbeNodeIdentity(k, inter, idata, gdata);
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
        if (c.item && c.parent && c.cat2 == 0x11) return skip("worn by someone");
        if (game::InventoryHas(c.iid)) return skip("already in your bag");
        if (c.node[0])
        {
            if (IStr(c.node, "visione") || IStr(c.node, "quest") || IStr(c.node, "artifact")) return skip("quest or memory trigger");
            if (IStr(c.node, "abyssruins")) return skip("fast-travel artifact");
            if (IStr(c.node, "mission")) return skip("mission object");
            if (AttachedPart(c.node, c.nodeType)) return skip("part of a creature or a mechanism");
            // Whatever arming refuses, the verdict refuses too. This was a
            // hand-copied list and it had drifted: "puzzle" was in OffLimits
            // and missing here. Five puzzle prefabs are tagged ore in the node
            // table (ice_block_break, two ice_wall_break, pickaxe_break_point,
            // stone_wall_break), and because OffLimits stops them being armed
            // they never fill, fall through to the ore branch below, and were
            // reaching the vein break. The mod was swinging at an ice wall.
            if (OffLimits(c.node)) return skip("puzzle or protected mechanism");
            // Kept in step with OffLimits(), which stops arming touching the same
            // things. The words above are the detail this one summarises.
            // These read the prefab path, so a gather node whose name happens to
            // carry one of the words is not a container: cd_box_mushroom_02 is a
            // plant. A node the table has already classified keeps its kind.
            if (!c.nodeType)
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
        const bool groundLizard = c.cat2 == 0x08 &&
                                  c.speciesExact && c.species && c.species->itemRow >= 0;
        const bool catchable = (smallGame || groundLizard) && !c.inter;
        const bool beastCorpse = c.dead == 1 && (c.cat2 == 0x0C || c.ai);
        if (c.dead == 1 && !beastCorpse) return skip("corpse: loot drops separately");
        if (!catchable && c.dead != 1 && !c.inter && c.ai) return skip("creature");
        if (!catchable && c.dead != 1 && !c.inter) return skip("no interaction node");

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
        case Action::Search: if (!cfg.lootCorpses) return skip("carcasses off"); break;
        case Action::Catch:
        {
            if (c.speciesClass)
            {
                const std::string cl = c.speciesClass;
                if (cl == "insect")                       { if (!cfg.catchInsects) return skip("insects off"); }
                else if (cl == "fish" || cl == "seafood") { if (!cfg.catchFish)    return skip("fish off"); }
                else                                      { if (!cfg.catchAnimals) return skip("small animals off"); }
                if (c.species)
                    if (const Item* it = ItemDb::ByRow(c.species->itemRow))
                    {
                        const Rules::Verdict r = Rules::Decide(*it, cfg);
                        if (!r.loot) { snprintf(v.detail, sizeof v.detail, "%s", r.detail.c_str()); v.loot = false; v.why = r.rule; return v; }
                    }
            }
            else if (c.cat2 == 0x05) { if (!cfg.catchFish || !cfg.catchInsects || !cfg.catchAnimals) return skip("unidentified: could be a fish, a flying insect or a bird"); }
            else                     { if (!cfg.catchInsects || !cfg.catchAnimals) return skip("unidentified: could be an insect or a small animal"); }
            break;
        }
        case Action::Gather:
        {
            if (const char* off = GatherSwitchOff(NodeKind(c), cfg)) return skip(off);
            break;
        }
        default:
        {
            if (!cfg.pickUpItems) return skip("pick up off");
            // Ore, stone and wood reach the ground as drops from broken nodes;
            // the same toggles cover the chunks. Furniture reaches it by being
            // smashed, and a table is furniture whether it is still standing or
            // lying in pieces, so it answers to the same switch either way.
            switch (KindOf(c.db, true))
            {
            case GatherKind::Plant: if (!cfg.gatherPlants) return skip("plants off"); break;
            case GatherKind::Crop:  if (!cfg.gatherCrops)  return skip("crops off"); break;
            case GatherKind::Ore:   if (!cfg.gatherOre)   return skip("ore off"); break;
            case GatherKind::Stone: if (!cfg.gatherStone) return skip("stone off"); break;
            case GatherKind::Wood:  if (!cfg.gatherWood)  return skip("wood off"); break;
            case GatherKind::Furniture: if (!cfg.lootFurniture) return skip("furniture off"); break;
            default: break;
            }
            break;
        }
        }
        const float lim = v.act == Action::Search ? cfg.corpseRange : v.act == Action::Catch ? cfg.catchRange
                        : v.act == Action::Gather ? cfg.gatherRange : cfg.lootRange;
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
            if (const Item* y = LearnedYield(c.gtid)) { snprintf(buf, sizeof buf, "%s node", y->Label()); return buf; }
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

    static void Scan(const Config& cfg, bool act, bool burst)
    {
        const DWORD now = GetTickCount();
        g_scanNow = now;
        g_debugLog = cfg.debugLog;
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

        // The game keeps its own pointer to the character being played, and
        // reading it beats every rule the engine used to guess with. See
        // game::LocalPlayer.
        if (const uintptr_t lp = game::LocalPlayer())
        {
            uint32_t lpEid = 0;
            if (game::Eid(lp, &lpEid) && (lp != g_me || lpEid != g_meEid))
            {
                static uint32_t s_said = 0;
                if (s_said != lpEid)
                {
                    s_said = lpEid;
                    LOG("[player] the game is playing %08X (tag %02X); scanning around that", lpEid, lpEid >> 24);
                }
                if (g_meEid && g_meEid != lpEid) ForgetBody("the game is playing someone else");
                g_me = lp; g_meEid = lpEid;
            }
            g_barrenSince = 0;
        }
        // Re-pick when the world around the current one is empty. Nothing in
        // range for several seconds while the player is standing in a field is
        // the signature of measuring from the wrong actor.
        // Only when the game's own pointer could not be followed.
        const bool reconsider = !g_me || (g_barrenSince && GetTickCount() - g_barrenSince > 4000);
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
            struct Pick { uintptr_t ent; uint32_t eid; Vec3 pos; int around; };   // not 'near': windows.h defines it
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
                if (tag == game::kTagPlayer && candN < 16 && ok) cand[candN++] = { e, eid, q, 0 };
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
                        if (!dup) cand[candN++] = { all[i].ent, all[i].eid, all[i].pos, 0 };
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
            if (allN < 50 && g_me)
            {
                static DWORD s_saidThin = 0;
                if (g_debugLog && now - s_saidThin > 5000)
                {
                    s_saidThin = now;
                    LOG("[player] only %d entities in this pass, too few to choose from; waiting for a fuller one", allN);
                }
                return;
            }
            const float lim = cfg.scanRange * cfg.scanRange;
            for (int i = 0; i < candN; ++i)
                for (int j = 0; j < worldN; ++j)
                {
                    const float dx = world[j].x - cand[i].pos.x, dy = world[j].y - cand[i].pos.y, dz = world[j].z - cand[i].pos.z;
                    if (dx * dx + dy * dy + dz * dz <= lim) ++cand[i].around;
                }
            int best = -1;
            for (int i = 0; i < candN; ++i) if (best < 0 || cand[i].around > cand[best].around) best = i;
            if (best >= 0)
            {
                const bool changed = g_meEid != cand[best].eid;
                if (g_meEid && g_meEid != cand[best].eid) ForgetBody("the player actor was re-picked");
                g_me = cand[best].ent; g_meEid = cand[best].eid;
                if (changed || g_debugLog)
                {
                    char line[240]; int w = 0;
                    for (int i = 0; i < candN && w < 200; ++i)
                        w += snprintf(line + w, sizeof line - w, " %08X:%d%s", cand[i].eid, cand[i].around, i == best ? "*" : "");
                    LOG("[player] %d candidates (player-tagged plus the biggest gear holders), world objects within %.0f m of each:%s (* is the one being scanned around)",
                        candN, cfg.scanRange, line);
                }
            }
            g_barrenSince = 0;
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
            if (body && (g_barrenSince || g_bodyEid == body->eid))
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
        game::InventoryRefresh(g_me, !g_pend.empty());
        if (g_debugLog) game::DumpInventoryShape(g_me, g_bagFull);
        LearnFromInventory(now);
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
        ForEachEntity(mgr, [&](uintptr_t e) {
            uint32_t eid = 0;
            if (!game::Eid(e, &eid)) return true;
            ++tagCount[eid >> 24];
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
            if (g_bodyEid && eid == g_bodyEid) TouchHolder(eid, q, now);
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
        // The game hands out partial lists; keep objects seen in the last second.
        for (auto it = g_seen.begin(); it != g_seen.end();)
        {
            if (now - it->second.when > 3000) { it = g_seen.erase(it); continue; }
            bool have = false;
            for (const Cand& c : list) if (c.eid == it->first) { have = true; break; }
            if (!have && now - it->second.when <= 1000 && list.size() < 256)
            {
                uint32_t eid2 = 0;
                if (game::Eid(it->second.ent, &eid2) && eid2 == it->first)
                {
                    const float dx = it->second.pos.x - mp.x, dy = it->second.pos.y - mp.y, dz = it->second.pos.z - mp.z;
                    Cand k; k.ent = it->second.ent; k.eid = eid2; k.pos = it->second.pos;
                    k.d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (k.d <= cfg.scanRange) { k.route = game::Route(k.ent); k.type = game::TypeTag(k.ent); k.parent = game::ParentEid(k.ent); list.push_back(k); }
                }
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
        if (inRange == 0 && total > 8) { if (!g_barrenSince) g_barrenSince = now; }
        else g_barrenSince = 0;
        const bool settling = now < s_holdUntil;
        (void)total;

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
            // Now that Fill has read the instance id, Key() means what it says.
            // Anything already retired is noted by entity as well, so the next
            // scan takes the shortcut above instead of filling it again. Before
            // this the shortcut compared a raw entity id against a set keyed by
            // instance id, so it never matched for an object that had one and
            // every such object was filled again on every scan of the session.
            if (g_searched.count(Key(k))) { k.banned = true; g_retiredEid.insert(k.eid); }
        }
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
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (!list[i].filled) continue;
            int around = 0;
            for (size_t j = 0; j < list.size(); ++j)
            {
                if (i == j) continue;
                const float dx = list[j].pos.x - list[i].pos.x, dy = list[j].pos.y - list[i].pos.y, dz = list[j].pos.z - list[i].pos.z;
                const float dd = dx * dx + dy * dy + dz * dz;
                if (dd <= 0.0004f) ++around; // within 2 cm: the same point, as storage contents are
                if (!list[i].gather && !list[i].item && list[i].dead != 1 && list[i].inter && list[j].filled &&
                    (list[j].gather || list[j].item) && list[j].type == list[i].type && dd <= 0.25f) list[i].twin = true;
            }
            // around counts the others, so this is four or more at one point.
            // A loose item lying where the mod has just broken a vein is that
            // vein's spill and is exempt; anything else at one point is still
            // treated as storage.
            if (around >= 3 && !(list[i].item && NearOwnBreak(list[i].pos, now))) list[i].heap = true;
        }

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
        for (size_t i = 0; i < list.size() && g_whyLines < 4000; ++i)
        {
            const Cand& k = list[i];
            const Verdict& v = verdicts[i];
            if (!k.filled || v.loot || k.d > diagRange) continue;
            auto it = g_why.find(k.eid);
            if (it != g_why.end() && it->second == v.why) continue;
            g_why[k.eid] = v.why;
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
                    // Arming used to consult no looting switch whatsoever. It
                    // reached for a node whose kind the player had turned off,
                    // and the verdict that would have refused it never ran,
                    // because arming comes first. With every switch that could
                    // apply set to off, the mod still armed a damaging thorn
                    // 28 times in one session.
                    if (GatherSwitchOff(NodeKind(k), cfg)) continue;
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
                    if (k.node[0] && !k.nodeType && !cfg.lootContainers && (IStr(k.node, "_chest") || IStr(k.node, "_box") || IStr(k.node, "dropset"))) continue;
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
                    MarkDone(key, now);
                    if (v.act != Action::Catch) SpotMark(k.pos, k.tid, k.eid, now);
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
                    const bool breakIt = cfg.breakOre && v.act == Action::Gather && k.nodeType &&
                                         k.nodeType->tagged && KindFromName(k.nodeType->kind) == GatherKind::Ore;
                    if (breakIt)
                    {
                        // Drive the game's own state machine at the node: the swing
                        // landing, then the break. The transition is what spawns the
                        // ore, so the vein empties itself and then disappears, which
                        // the drop event on its own never made it do.
                        const uintptr_t gc = game::CompByClass(game::Comps(k.ent), kCls_Gimmick);
                        if (!gc || !events::DriveBreak(gc, g_meEid, g_me, k.eid, k.pos.x, k.pos.y, k.pos.z))
                        {
                            // Its own budget: the shared hold log is spent on
                            // "still filling" long before a break failure would show.
                            static int s_brkErr = 0;
                            if (s_brkErr < 8) { ++s_brkErr; LOG_ERR("[break] eid %08X could not be queued", k.eid); }
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
                        // What falls out lands here. NearOwnBreak reads this so the
                        // spill is not mistaken for the contents of a container.
                        BrokeMark(k.pos, now);
                        if (g_debugLog)
                        {
                            if (g_brokeWatch.size() > 64) g_brokeWatch.clear();
                            g_brokeWatch[k.eid] = now;
                        }
                    }
                    else if (!events::Send(v.act, k.eid, g_meEid, route, 0)) { held("the game refused the event"); continue; }
                    ++taken;
                    g_pend.push_back({ now, v.act, v.act == Action::Gather ? k.gtid : static_cast<uint16_t>(0), k.db ? k.db->row : -1, false });
                    InterlockedIncrement(&g_session[static_cast<int>(v.act)]);
                    // Wide enough for a name and a distance. At 80 a long prefab
                    // path consumed the buffer and the distance was truncated away,
                    // which is how ten ore pickups were logged with no range at all.
                    char line[160];
                    if (breakIt) snprintf(line, sizeof line, "break %s (%.1f m)", Label(k), k.d);
                    else         snprintf(line, sizeof line, "%s %s (%.1f m)", events::ActionName(v.act), Label(k), k.d);
                    PushRecent(line);
                    LOG("[loot] %s eid %08X type %u %s%s%s", line, k.eid, k.tid, k.key[0] ? k.key : "", k.node[0] ? " node " : "", k.node[0] ? k.node : "");
                }
            }
        }

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
        while (InterlockedCompareExchange(&g_running, 0, 0))
        {
            // A copy: the render thread edits the live Config while the menu is up.
            Config cfg = Settings::Snapshot();
            const State& st = State::Get();
            if (!st.Captures() && State::ForegroundIsOurs())
            {
                // Either the key or the pad shortcut, whichever the player set.
                const bool t = KeyDown(cfg.keyToggle) || ml::hooks::PadChordHeld(cfg.padToggle);
                if (t && !toggleWas) { SetAuto(!cfg.enabled); cfg.enabled = !cfg.enabled; }
                toggleWas = t;
                const bool b = KeyDown(cfg.keyBurst) || ml::hooks::PadChordHeld(cfg.padBurst);
                if (b && !burstWas) { InterlockedExchange(&g_burst, 1); State::Get().Notify("Master Looter: looting everything in range", 1500); }
                burstWas = b;
                // Unbound reads as nothing held, so an unset key never fires.
                const bool o = (cfg.keyOwned && KeyDown(cfg.keyOwned)) ||
                               (cfg.padOwned && ml::hooks::PadChordHeld(cfg.padOwned));
                if (o && !ownedWas) { SetLootOwned(!cfg.lootOwned); cfg.lootOwned = !cfg.lootOwned; }
                ownedWas = o;
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
            Sleep(want ? static_cast<DWORD>(1000 / sps) : 100);
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

    void OnGameTick() { events::Drain(); }

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
    void RequestBurst() { InterlockedExchange(&g_burst, 1); State::Get().Notify("Master Looter: looting everything in range", 1500); }
    void SetAuto(bool on)
    {
        { std::lock_guard<std::recursive_mutex> lk(Settings::Mutex()); Settings::Get().enabled = on; Settings::MarkDirty(); }
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
