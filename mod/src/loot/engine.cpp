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
        uint16_t tid = 0;
        uint8_t  type = 0xFF, cat = 0, cat2 = 0, dead = 0, locked = 0, gkind = 0;
        bool inter = false, item = false, gather = false, ai = false, noIid = false;
        bool twin = false, heap = false, mine = false, filled = false, banned = false;
        Vec3 pos;
        float d = 0;
        char key[64] = "";   // engine string key from the live table
        char node[64] = "";  // gimmick node name
        const Item* db = nullptr;
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

    static Species FindSpecies(uint32_t eid, uintptr_t ent, uintptr_t actor, uintptr_t status, uintptr_t ai, uint8_t cat2)
    {
        Species sp;
        if (!CreatureDb::Loaded()) return sp;
        auto hit = g_speciesByEid.find(eid);
        if (hit != g_speciesByEid.end()) return hit->second;
        if (g_speciesMiss.count(eid)) return sp;
        static int s_dumped05 = 0, s_dumped09 = 0;
        int& dumped = cat2 == 0x05 ? s_dumped05 : s_dumped09;
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
            if (s_hits < 30) { ++s_hits; LOG("[species] %08X (byte %02X) is %s%s%s: word '%s' in \"%s\" (trust %d)", eid, cat2, sp.klass, sp.row ? (sp.exact ? ", " : ", e.g. ") : "", sp.row ? sp.row->name.c_str() : "", sp.word.c_str(), sp.from.c_str(), sp.trust); }
            return sp;
        }
        if (dump)
        {
            ++dumped;
            uintptr_t vt = 0, ti = 0; mem::ReadPtr(ent, &vt); mem::ReadPtr(ent + kOff_Ent_TypeInfo, &ti);
            LOG("[species] no match for %08X (byte %02X, vtable +0x%llX typeinfo %llX); strings seen: %s", eid, cat2,
                static_cast<unsigned long long>(mem::Rva(vt)), static_cast<unsigned long long>(ti), probe[0] ? probe : "(none)");
        }
        if (g_speciesMiss.size() > 4096) g_speciesMiss.clear();
        g_speciesMiss.insert(eid);
        return sp;
    }

    struct Verdict { bool loot = false; Action act = Action::Take; const char* why = ""; char detail[40] = ""; };

    // Memories. Keys: instance id when the node has one (survives respawns),
    // otherwise the entity id.
    static uint64_t Key(const Cand& c) { return c.iid ? (0x100000000ull | c.iid) : c.eid; }
    struct Done { DWORD when; int tries; };
    static std::unordered_map<uint64_t, Done>  g_done;      // recently sent
    static std::unordered_set<uint64_t>        g_searched;  // never again this session
    struct ArmRec { DWORD at; int fails; bool judged; int mode; int ctxKind; };
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
    static std::unordered_map<uint32_t, uint16_t> g_nodeType; // eid -> gather node type, from the last scans
    static std::unordered_map<uintptr_t, uint32_t> g_actorEid; // node actor -> eid, from the last scans
    static std::unordered_map<uint32_t, DWORD> g_slotProbeAt;  // eid -> last gimmick slot dump

    static uintptr_t g_me = 0;
    static uint32_t  g_meEid = 0, g_meRoute = 0;

    // --- what a gather node yields --------------------------------------------
    // Gather nodes carry a type number the static tables do not explain. The
    // bag tells us instead: after a gather, whichever item count rose is what
    // that node type yields. Learned pairs persist in MasterLooter.learned.tsv.
    static std::unordered_map<uint16_t, uint16_t> g_learn;   // node type -> item row
    struct PendSend { DWORD at; Action act; uint16_t nodeType; int itemRow; };
    static std::vector<PendSend> g_pend;
    static std::vector<std::pair<uint16_t, long long>> g_invPrev;
    static bool g_invPrevValid = false;

    static void SaveLearned();
    static void LoadLearned()
    {
        FILE* f = _wfopen(Paths::File(L"MasterLooter.learned.tsv").c_str(), L"rb");
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            unsigned node = 0, row = 0; char key[96] = "";
            if (sscanf(line, "%u\t%u\t%95[^\t\r\n]", &node, &row, key) < 2 || !node || node >= 65536 || row >= 65536) continue;
            // The string key survives a game patch; the row id may not.
            if (key[0])
                if (const Item* byKey = ItemDb::ByStringKey(key))
                    if (byKey->row >= 0)
                    {
                        if (byKey->row != static_cast<int>(row)) LOG("[learn] node %u: %s moved from row %u to %d", node, key, row, byKey->row);
                        row = static_cast<unsigned>(byKey->row);
                    }
            const Item* it = ItemDb::ByRow(static_cast<int>(row));
            if (IsCreatureItem(it))
            { LOG("[learn] dropping node %u -> %s: a creature cannot be a node yield", node, it->Label()); continue; }
            g_learn[static_cast<uint16_t>(node)] = static_cast<uint16_t>(row);
        }
        fclose(f);
        LOG("[learn] %d node yields loaded", static_cast<int>(g_learn.size()));
        SaveLearned(); // rewrite without anything dropped
    }
    static void SaveLearned()
    {
        FILE* f = _wfopen(Paths::File(L"MasterLooter.learned.tsv").c_str(), L"wb");
        if (!f) return;
        fputs("node_type\titem_row\titem_key\titem_name\n", f);
        for (const auto& kv : g_learn)
        {
            const Item* it = ItemDb::ByRow(kv.second);
            fprintf(f, "%u\t%u\t%s\t%s\n", kv.first, kv.second, it ? it->stringKey.c_str() : "", it ? it->name.c_str() : "");
        }
        fclose(f);
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
            g_pend.push_back({ static_cast<DWORD>(seen[i].at), seen[i].act, nodeType, -1 });
            if (Settings::Get().debugLog) LOG("[learn] player %s eid %08X (node type %u)", events::ActionName(seen[i].act), seen[i].eid, nodeType);
        }
        static uint16_t types[2048]; static long long qty[2048];
        const int n = game::InventoryTypes(types, qty, 2048);
        std::vector<std::pair<uint16_t, long long>> cur(n);
        for (int i = 0; i < n; ++i) cur[i] = { types[i], qty[i] };
        std::vector<uint16_t> rose;
        if (g_invPrevValid)
        {
            size_t j = 0;
            for (const auto& e : cur)
            {
                while (j < g_invPrev.size() && g_invPrev[j].first < e.first) ++j;
                const long long before = (j < g_invPrev.size() && g_invPrev[j].first == e.first) ? g_invPrev[j].second : 0;
                if (e.second > before) rose.push_back(e.first);
            }
        }
        g_invPrev.swap(cur);
        g_invPrevValid = true;
        // Expire stale sends.
        g_pend.erase(std::remove_if(g_pend.begin(), g_pend.end(), [now](const PendSend& p) { return now - p.at > 4000; }), g_pend.end());
        if (rose.empty() || g_pend.empty()) return;
        for (uint16_t type : rose)
        {
            // A send whose item we already knew explains the rise.
            auto known = std::find_if(g_pend.begin(), g_pend.end(), [type](const PendSend& p) { return p.itemRow == type; });
            if (known != g_pend.end()) { g_pend.erase(known); continue; }
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
                SaveLearned();
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
    enum class GatherKind { Unknown, Plant, Ore, Stone, Wood, Item };
    // What an item counts as for the kind toggles. The classes come straight
    // from the item database (scripts/build_item_db.py): ore and jewel are
    // minerals from veins, stone from quarries, wood from trees and branches.
    // "Plant" means herbs, flowers and mushrooms. Crops (a vegetable, fruit or
    // grain) are food and follow the Ground items toggle and their class rule,
    // whether still on the plant or lying loose. `onGround`: an item lying in
    // the world rather than a node's yield.
    static GatherKind KindOf(const Item* y, bool onGround = false)
    {
        if (!y) return GatherKind::Unknown;
        const std::string& k = y->klass;
        if (k == "wood"  || y->HasTag("wood"))  return GatherKind::Wood;
        if (k == "stone" || y->HasTag("stone")) return GatherKind::Stone;
        if (k == "ore" || k == "jewel" || y->HasTag("ore") || y->HasTag("mineral")) return GatherKind::Ore;
        if (k == "herb") return GatherKind::Plant;
        if (onGround) return GatherKind::Item;
        if (k == "vegetable" || k == "fruit" || k == "grain") return GatherKind::Item;
        if (k == "seed" || k == "alchemy-material" || y->HasTag("rare-gather")) return GatherKind::Plant;
        return GatherKind::Item;
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

    // Reads components, node data and names for one candidate.
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
        if (gdata)
        {
            uint16_t t = 0; if (mem::Read16(gdata, &t)) k.tid = t;
            mem::Read8(gdata + 5, &k.gkind);
            if (k.gkind == 0x04 && k.tid == 52920) g_containers.insert(k.eid); // the well bucket
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
            game::NodeName(inter, k.node, sizeof k.node);
        }
        // Live creatures: which species, from the CharacterInfo row they point at.
        if (k.ai && !k.inter && k.type == 0x06 && (k.cat2 == 0x05 || k.cat2 == 0x09))
        {
            const Species sp = FindSpecies(k.eid, k.ent, comps, status, game::CompByClass(comps, kCls_Ai), k.cat2);
            k.species = sp.row; k.speciesClass = sp.klass; k.speciesExact = sp.exact;
        }
        if (k.gather && k.tid) { if (g_nodeType.size() > 4096) g_nodeType.clear(); g_nodeType[k.eid] = k.tid; }
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
        if (k.tid)
        {
            if (!game::ItemKeyForType(k.tid, k.key, sizeof k.key)) game::GimmickKeyForType(k.tid, k.key, sizeof k.key);
            // Prefer the live key string; fall back to the row mapping only when verified.
            k.db = k.key[0] ? ItemDb::ByStringKey(k.key) : nullptr;
            if (!k.db && game::ItemTableState() == 1 && k.item) k.db = ItemDb::ByRow(k.tid);
        }
    }

    static Verdict Decide(const Cand& c, const Config& cfg)
    {
        Verdict v;
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
        if (c.parent && c.parent == g_meEid) return skip("worn or carried by you");
        if (c.item && c.parent && c.cat2 == 0x11) return skip("worn by someone");
        if (game::InventoryHas(c.iid)) return skip("already in your bag");
        if (c.node[0])
        {
            if (IStr(c.node, "visione") || IStr(c.node, "quest") || IStr(c.node, "artifact")) return skip("quest or memory trigger");
            if (IStr(c.node, "abyssruins")) return skip("fast-travel artifact");
            if (IStr(c.node, "mission")) return skip("mission object");
            const bool container = IStr(c.node, "_chest") || IStr(c.node, "_box") || IStr(c.node, "dropset");
            const bool furniture = IStr(c.node, "furniture");
            if (container && !cfg.lootContainers) return skip("container (off)");
            if (furniture && !cfg.lootFurniture)  return skip("furniture node (off)");
        }
        if (c.tid == 52920 || g_containers.count(c.eid)) return skip("mechanism part");
        if (c.heap) return skip("stack at one point (storage contents)");
        // (A pointer to the player inside the object used to mean "yours"; the
        // parent and bag checks above cover that, and arming can plant such a
        // pointer in a node we just touched.)

        const bool catchable = (c.cat2 == 0x09 || c.cat2 == 0x05) && c.type == 0x06 && !c.inter;
        const bool beastCorpse = c.dead == 1 && (c.cat2 == 0x0C || c.ai);
        if (c.dead == 1 && !beastCorpse) return skip("corpse: loot drops separately");
        if (!catchable && c.dead != 1 && !c.inter && c.ai) return skip("creature");
        if (!catchable && c.dead != 1 && !c.inter) return skip("no interaction node");

        if (beastCorpse)     v.act = Action::Search;
        else if (catchable)  v.act = Action::Catch;
        else if (c.gather)   v.act = Action::Gather;
        else if (c.item)     v.act = Action::Take;
        else return skip("not ready (node empty)");

        // Item rules from the database. A live key that our table knows gets the
        // full class/tag/item verdict; a node whose yield has been learned gets
        // the same verdict on the yield; unknown names fall back to name checks.
        if ((v.act == Action::Take || v.act == Action::Gather) && c.tid)
        {
            const Item* ruled = c.db ? c.db : (v.act == Action::Gather ? LearnedYield(c.tid) : nullptr);
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
            const GatherKind kind = KindOf(c.db ? c.db : LearnedYield(c.tid));
            switch (kind)
            {
            case GatherKind::Plant:   if (!cfg.gatherPlants)  return skip("plants off"); break;
            case GatherKind::Ore:     if (!cfg.gatherOre)     return skip("ore off"); break;
            case GatherKind::Stone:   if (!cfg.gatherStone)   return skip("stone off"); break;
            case GatherKind::Wood:    if (!cfg.gatherWood)    return skip("wood off"); break;
            case GatherKind::Item:    if (!cfg.pickUpItems)   return skip("pick up off"); break;
            default:                  if (!cfg.gatherUnknown) return skip("unidentified nodes off"); break;
            }
            break;
        }
        default:
        {
            if (!cfg.pickUpItems) return skip("pick up off");
            // Ore, stone and wood reach the ground as drops from broken nodes;
            // the same toggles cover the chunks.
            switch (KindOf(c.db, true))
            {
            case GatherKind::Plant: if (!cfg.gatherPlants) return skip("plants off"); break;
            case GatherKind::Ore:   if (!cfg.gatherOre)   return skip("ore off"); break;
            case GatherKind::Stone: if (!cfg.gatherStone) return skip("stone off"); break;
            case GatherKind::Wood:  if (!cfg.gatherWood)  return skip("wood off"); break;
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
            const int steal = hooks::WouldSteal(g_me, c.ent);
            if (steal == 1)  return skip("owned by someone (theft)");
            if (steal == -1) return skip("owner unknown yet");
        }
        v.loot = true;
        v.why = events::ActionName(v.act);
        return v;
    }

    static const char* Label(const Cand& c)
    {
        if (c.db) return c.db->Label();
        if (c.gather && c.tid)
        {
            static char buf[80];
            if (const Item* y = LearnedYield(c.tid)) snprintf(buf, sizeof buf, "%s node", y->Label());
            else snprintf(buf, sizeof buf, "node type %u", c.tid);
            return buf;
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
    static void Scan(const Config& cfg, bool act, bool burst)
    {
        const DWORD now = GetTickCount();
        g_debugLog = cfg.debugLog;
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);

        const uintptr_t mgr = game::ActorManager();
        if (!mgr) { std::lock_guard<std::mutex> lk(g_mu); g_status.actorManager = false; g_status.playerFound = false; return; }

        // Player: remembered between scans and re-validated by its id tag.
        uint32_t id = 0;
        if (g_me && (!game::Eid(g_me, &id) || id != g_meEid || (id >> 24) != game::kTagPlayer)) g_me = 0;
        if (!g_me)
        {
            ForEachEntity(mgr, [&](uintptr_t e) {
                uint32_t eid = 0;
                if (game::Eid(e, &eid) && (eid >> 24) == game::kTagPlayer) { g_me = e; g_meEid = eid; return false; }
                return true;
            });
        }
        Vec3 mp;
        if (!g_me || !game::WorldPos(g_me, &mp))
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status.actorManager = true; g_status.playerFound = false;
            return;
        }
        g_meRoute = game::Route(g_me);
        game::InventoryRefresh(g_me, !g_pend.empty());
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
        ForEachEntity(mgr, [&](uintptr_t e) {
            uint32_t eid = 0;
            if (!game::Eid(e, &eid) || (eid >> 24) != game::kTagWorld) return true;
            for (const Cand& c : list) if (c.eid == eid) return true; // one entity sits in several lists
            ++total;
            Vec3 q;
            if (!game::WorldPos(e, &q)) return true;
            const float dx = q.x - mp.x, dy = q.y - mp.y, dz = q.z - mp.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > cfg.scanRange) return true;
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
                if (now - s_holdLogAt > 5000) { s_holdLogAt = now; LOG("[scan] paused: %s", why); }
            }
        }
        s_lastPos = mp; s_lastMe = g_me; s_havePrev = true;
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
            if (g_searched.count(k.eid)) { k.filled = true; k.banned = true; continue; }
            Fill(k);
            ++detailed;
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
            if (around >= 3) list[i].heap = true;
        }

        // Decide, publish, and act.
        std::vector<Nearby> nearby;
        int lootable = 0;
        std::vector<Verdict> verdicts(list.size());
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (!list[i].filled) continue;
            verdicts[i] = Decide(list[i], cfg);
            if (verdicts[i].loot) ++lootable;
            if (nearby.size() < 48)
            {
                Nearby n{}; n.eid = list[i].eid; n.dist = list[i].d; n.loot = verdicts[i].loot;
                strncpy(n.name, Label(list[i]), sizeof n.name - 1);
                strncpy(n.klass, list[i].db ? list[i].db->klass.c_str() : list[i].speciesClass ? list[i].speciesClass : (list[i].gather ? "gather node" : list[i].dead == 1 ? "corpse" : ""), sizeof n.klass - 1);
                if (verdicts[i].detail[0]) snprintf(n.verdict, sizeof n.verdict, "%s: %s", verdicts[i].why, verdicts[i].detail);
                else strncpy(n.verdict, verdicts[i].why, sizeof n.verdict - 1);
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
            LOG("[why] %08X %.1fm %s: %s%s%s | type %u tag %02X cat %02X/%02X dead %u parent %08X %s%s%s%s%s%s%s",
                k.eid, k.d, Label(k), v.why, v.detail[0] ? ": " : "", v.detail,
                k.tid, k.type, k.cat, k.cat2, k.dead, k.parent,
                k.inter ? "node " : "", k.item ? "item " : "", k.gather ? "gather " : "", k.ai ? "ai " : "",
                k.twin ? "twin " : "", k.heap ? "heap " : "", k.node[0] ? k.node : "");
        }
        // Per-entity memories grow with every object ever seen; a long session
        // sees hundreds of thousands. Forget the diagnostics wholesale and the
        // retry records once they are stale. (g_searched stays: a carcass must
        // never be searched twice, whatever the session length.)
        if (g_why.size() > 8192) g_why.clear();
        if (g_firstSeen.size() > 8192) g_firstSeen.clear();
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
                    if (k.parent == g_meEid || k.heap || k.d > armLim) continue;
                    if (!cfg.armContainers && g_containers.count(k.eid)) continue;
                    if (k.node[0] && !cfg.lootContainers && (IStr(k.node, "_chest") || IStr(k.node, "_box") || IStr(k.node, "dropset"))) continue;
                    if (k.node[0] && !cfg.lootFurniture && IStr(k.node, "furniture")) continue;
                    const uint64_t key = Key(k);
                    if (g_searched.count(key)) continue;
                    auto ar = g_armed.find(key);
                    if (ar != g_armed.end())
                    {
                        // Still empty after we armed it. Give the game two
                        // seconds, then count a failure; three failures and
                        // the node is not loot (a chest, a wardrobe).
                        if (!ar->second.judged && now - ar->second.at > 2000)
                        {
                            ar->second.judged = true;
                            static int s_failLogs = 0;
                            if (++ar->second.fails >= 3) { g_searched.insert(key); if (s_failLogs < 30) { ++s_failLogs; LOG("[arm] eid %08X %.1f m never filled after 3 arms (tag %02X cat2 %02X%s%s)", k.eid, k.d, k.type, k.cat2, k.node[0] ? " node " : "", k.node); } continue; }
                        }
                        if (!ar->second.judged || now - ar->second.at < 5000) continue; // wait, or cool down before re-arming
                    }
                    const uintptr_t g = game::CompByClass(game::Comps(k.ent), kCls_Gimmick);
                    if (!g) continue;
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
                    const uintptr_t gameA3    = hooks::ArmArg3();
                    const int combo = rec.fails % 3;
                    const uintptr_t armCtx = (combo == 2) ? (meActor ? meActor : ArmContextNow()) : (nodeActor ? nodeActor : ArmContextNow());
                    const uintptr_t armA3  = (combo == 1) ? 0 : gameA3;
                    rec.ctxKind = combo;
                    if (s_armLogs < 60) { ++s_armLogs; LOG("[arm] arming eid %08X %.1f m mode %d try %d combo %d ctx %llX a3 %llX (tag %02X cat2 %02X%s%s)", k.eid, k.d, rec.mode, rec.fails + 1, combo, static_cast<unsigned long long>(armCtx), static_cast<unsigned long long>(armA3), k.type, k.cat2, k.node[0] ? " node " : "", k.node); }
                    events::Arm(g, static_cast<uintptr_t>(rec.mode), armA3, armCtx);
                    if (armedN < 32) armedNow[armedN++] = k.eid;
                    if (cfg.debugLog) LOG("[arm] eid %08X %.1f m %s", k.eid, k.d, k.node[0] ? k.node : "");
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
                    bool justArmed = false;
                    for (int a = 0; a < armedN; ++a) if (armedNow[a] == k.eid) justArmed = true;
                    if (justArmed) continue;
                    if (v.act == Action::Gather && k.tid == 0 && AgeMs(k.eid, now) < 700) continue; // let the node finish filling
                    const uint64_t key = Key(k);
                    if (RecentlyDone(key, now, cfg.retryAfterMs)) continue;
                    if (v.act != Action::Catch && SpotRecent(k.pos, k.tid, k.eid, now, cfg.retryAfterMs)) continue;
                    MarkDone(key, now);
                    if (v.act != Action::Catch) SpotMark(k.pos, k.tid, k.eid, now);
                    if (g_searched.count(key) && v.act != Action::Search) { if (cfg.debugLog) LOG("[loot] giving up on eid %08X after %d attempts", k.eid, kMaxTries); continue; }
                    if (v.act == Action::Search) g_searched.insert(key);
                    if (!events::Send(v.act, k.eid, g_meEid, route, 0)) continue;
                    ++taken;
                    g_pend.push_back({ now, v.act, v.act == Action::Gather ? k.tid : static_cast<uint16_t>(0), k.db ? k.db->row : -1 });
                    InterlockedIncrement(&g_session[static_cast<int>(v.act)]);
                    char line[80];
                    snprintf(line, sizeof line, "%s %s (%.1f m)", events::ActionName(v.act), Label(k), k.d);
                    PushRecent(line);
                    LOG("[loot] %s eid %08X type %u %s%s%s", line, k.eid, k.tid, k.key[0] ? k.key : "", k.node[0] ? " node " : "", k.node[0] ? k.node : "");
                }
            }
        }

        QueryPerformanceCounter(&t1);
        std::lock_guard<std::mutex> lk(g_mu);
        g_nearby.swap(nearby);
        g_status.actorManager = true;
        g_status.playerFound = true;
        g_status.playerEid = g_meEid;
        g_status.candidates = static_cast<int>(list.size());
        g_status.lootable = lootable;
        g_status.settling = settling;
        snprintf(g_status.hold, sizeof g_status.hold, "%s", settling ? s_holdWhy : "");
        g_status.inventoryItems = game::InventoryCount();
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
        LoadLearned();
        Note("waiting for the world");
        LOG_OK("[loot] engine ready; pump: %s", hooks::PumpName());

        bool toggleWas = false, burstWas = false;
        while (InterlockedCompareExchange(&g_running, 0, 0))
        {
            // A copy: the render thread edits the live Config while the menu is up.
            Config cfg = Settings::Snapshot();
            const State& st = State::Get();
            if (!st.Captures() && State::ForegroundIsOurs())
            {
                const bool t = KeyDown(cfg.keyToggle);
                if (t && !toggleWas) { SetAuto(!cfg.enabled); cfg.enabled = !cfg.enabled; }
                toggleWas = t;
                const bool b = KeyDown(cfg.keyBurst);
                if (b && !burstWas) { InterlockedExchange(&g_burst, 1); State::Get().Notify("Master Looter: looting everything in range", 1500); }
                burstWas = b;
            }
            if (InterlockedExchange(&g_forget, 0)) { g_learn.clear(); SaveLearned(); LOG("[learn] node yields forgotten"); }
            const bool burst = InterlockedExchange(&g_burst, 0) != 0;
            const bool want = cfg.enabled || burst || st.menuOpen;
            if (want) Scan(cfg, cfg.enabled || burst, burst);
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
}
