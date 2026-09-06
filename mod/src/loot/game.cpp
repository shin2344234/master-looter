#include "game.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "mem.h"
#include "signatures.h"
#include "../core/itemdb.h"
#include "../core/log.h"

namespace ml::game
{
    using namespace ml::sig;

    // ------------------------------------------------------------ resolve ----
    static Fns      g_f;
    static SigResult g_sigs[16];
    static int       g_sigN = 0;
    static bool      g_requiredOk = false;

    static uintptr_t Scan(const char* name, const char* pattern, bool required)
    {
        size_t hits = 0;
        const uintptr_t a = mem::FindUnique(pattern, &hits);
        if (g_sigN < 16) g_sigs[g_sigN++] = { name, a, hits, required };
        if (a) LOG("[sig] %-14s +0x%llX", name, static_cast<unsigned long long>(mem::Rva(a)));
        else   LOG_ERR("[sig] %-14s %s (%zu hits)%s", name, hits ? "AMBIGUOUS" : "not found", hits, required ? " - required" : "");
        return a;
    }

    // Table resolvers are clones; the one we want references its table-name
    // string with `lea r8,[rip+..]` and has the 16-bit-key prologue above it.
    struct TableHunt { const char* name; uintptr_t fn; };
    static bool TableVisit(uintptr_t hit, void* ctx)
    {
        auto* h = static_cast<TableHunt*>(ctx);
        const uintptr_t str = mem::RipAt(hit, 7);
        char buf[32];
        if (!mem::ReadCString(str, buf, sizeof buf) || strcmp(buf, h->name) != 0) return false;
        for (uintptr_t p = hit; p + kMax_LeaToPrologue > hit && p > mem::Game().base; --p)
            if (mem::MatchAt(p, kSig_TableResolver16)) { h->fn = p; return true; }
        return false;
    }
    static uintptr_t TableGlobal(const char* name)
    {
        TableHunt h{ name, 0 };
        mem::FindIf(kSig_LeaR8Rip, TableVisit, &h);
        if (!h.fn) return 0;
        const uintptr_t g = mem::RipAt(h.fn + kOff_TableResolver_MovGlobal, 7);
        return mem::InImage(g) ? g : 0;
    }

    bool ResolveAll()
    {
        g_sigN = 0;
        g_f = Fns{};
        const uintptr_t tls = Scan("tls+desc", kSig_TlsDesc, true);
        if (tls) { g_f.tlsInit = tls; g_f.descLookup = tls + kOff_TlsDesc_DescLookup; }
        g_f.allocEvent = Scan("alloc_event", kSig_AllocEvent, true);
        g_f.enqueue    = Scan("enqueue", kSig_Enqueue, true);
        const uintptr_t dq = Scan("desc_mask+queue", kSig_DescMaskQueue, true);
        if (dq)
        {
            g_f.descMask = mem::RipAt(dq + kOff_DescMask_Mov, 7);
            g_f.queue    = mem::RipAt(dq + kOff_Queue_Mov, 7);
            if (!mem::InImage(g_f.descMask) || !mem::InImage(g_f.queue))
            {
                LOG_ERR("[sig] DESC_MASK or queue resolved outside the image; sending disabled");
                g_f.descMask = g_f.queue = 0;
            }
            else LOG("[sig] DESC_MASK +0x%llX queue +0x%llX", static_cast<unsigned long long>(mem::Rva(g_f.descMask)),
                     static_cast<unsigned long long>(mem::Rva(g_f.queue)));
        }
        g_f.moveUpdate   = Scan("move_update", kSig_MoveUpdate, false);
        g_f.areaSweepHit = Scan("area_sweep", kSig_AreaSweep, false);
        g_f.ownCheck     = Scan("own_check", kSig_OwnCheck, false);
        g_f.armFn        = Scan("node_arm", kSig_ArmDispatch, false);
        g_f.itemTableGlobal    = TableGlobal(kStr_ItemInfoTable);
        g_f.gimmickTableGlobal = TableGlobal(kStr_GimmickInfoTable);
        if (g_sigN < 16) g_sigs[g_sigN++] = { "iteminfo table", g_f.itemTableGlobal, g_f.itemTableGlobal ? 1u : 0u, false };
        if (g_sigN < 16) g_sigs[g_sigN++] = { "gimmickinfo table", g_f.gimmickTableGlobal, g_f.gimmickTableGlobal ? 1u : 0u, false };
        LOG("[sig] iteminfo global %s, gimmickinfo global %s",
            g_f.itemTableGlobal ? "found" : "missing", g_f.gimmickTableGlobal ? "found" : "missing");

        g_requiredOk = g_f.tlsInit && g_f.allocEvent && g_f.enqueue && g_f.descMask && g_f.queue &&
                       (g_f.moveUpdate || g_f.areaSweepHit);
        return g_requiredOk;
    }

    const Fns& F() { return g_f; }
    int SigCount() { return g_sigN; }
    const SigResult& Sig(int i) { return g_sigs[i]; }
    bool RequiredOk() { return g_requiredOk; }

    // ------------------------------------------------------ actor manager ----
    static uintptr_t g_mgrVt[4];
    static int       g_mgrVtN = 0;
    static bool      g_mgrVtDone = false;
    static uintptr_t g_mgrSlot = 0;
    static DWORD     g_mgrNextTry = 0;

    uintptr_t ActorManager()
    {
        if (!g_mgrSlot)
        {
            const DWORD now = GetTickCount();
            if (g_mgrNextTry && static_cast<LONG>(now - g_mgrNextTry) < 0) return 0;
            g_mgrNextTry = now + 3000;
            if (!g_mgrVtDone)
            {
                g_mgrVtDone = true;
                g_mgrVtN = mem::FindVtablesByName(kRtti_ActorManager, g_mgrVt, 4);
                LOG("[mgr] ClientActorManager vtables: %d%s", g_mgrVtN,
                    g_mgrVtN ? "" : " (class not found in image)");
            }
            if (!g_mgrVtN) return 0;
            long cand = 0;
            g_mgrSlot = mem::FindGlobalHoldingVtable(g_mgrVt, g_mgrVtN, &cand);
            if (!g_mgrSlot) { static bool told = false; if (!told) { told = true; LOG("[mgr] not in memory yet (%ld pointers checked); retrying", cand); } return 0; }
            LOG_OK("[mgr] ClientActorManager global +0x%llX", static_cast<unsigned long long>(mem::Rva(g_mgrSlot)));
        }
        uintptr_t p = 0;
        if (!mem::ReadPtr(g_mgrSlot, &p)) return 0;
        return mem::Readable(p, kOff_Mgr_ListsEnd) ? p : 0;
    }
    bool ActorManagerFound() { return g_mgrSlot != 0; }

    // ----------------------------------------------------------- entities ----
    bool Eid(uintptr_t e, uint32_t* out) { return mem::Read32(e + kOff_Ent_Eid, out); }
    uint32_t Route(uintptr_t e) { uint32_t r = 0; mem::Read32(e + kOff_Ent_Route, &r); return r; }
    uint8_t TypeTag(uintptr_t e)
    {
        const uintptr_t ti = mem::Deref(e, kOff_Ent_TypeInfo);
        uint8_t t = 0xFF;
        if (!ti || !mem::Read8(ti + 1, &t)) return 0xFF;
        return t;
    }
    uintptr_t Comps(uintptr_t e) { return mem::Deref(e, kOff_Ent_Comps); }

    // Component slots are fixed but may move between patches; the class name
    // never does. The first successful RTTI match per class caches the slot
    // and vtable so later lookups are one pointer compare.
    struct SlotCache { const char* cls; int off; uintptr_t vt; };
    static SlotCache g_slots[3] = { { kCls_Status, -1, 0 }, { kCls_Gimmick, -1, 0 }, { kCls_Ai, -1, 0 } };

    uintptr_t CompByClass(uintptr_t comps, const char* cls)
    {
        if (!comps) return 0;
        SlotCache* sc = nullptr;
        for (auto& s : g_slots) if (strcmp(s.cls, cls) == 0) { sc = &s; break; }
        if (sc && sc->off >= 0)
        {
            const uintptr_t c = mem::Deref(comps, static_cast<unsigned>(sc->off));
            if (!c) return 0;                       // slot empty: this actor lacks the component
            uintptr_t vt = 0;
            if (mem::ReadPtr(c, &vt) && vt == sc->vt) return c;
        }
        if (!mem::Readable(comps, kComps_SlotsEnd)) return 0;
        for (unsigned off = 0; off < kComps_SlotsEnd; off += 8)
        {
            const uintptr_t c = mem::Deref(comps, off);
            if (!c) continue;
            const char* n = mem::RttiName(c);
            if (!n || !strstr(n, cls)) continue;
            if (sc) { uintptr_t vt = 0; if (mem::ReadPtr(c, &vt)) { sc->off = static_cast<int>(off); sc->vt = vt; } }
            return c;
        }
        return 0;
    }

    uintptr_t Transform(uintptr_t comps) { return comps ? mem::Deref(comps, kOff_Comps_Transform) : 0; }

    bool WorldPos(uintptr_t e, Vec3* out)
    {
        const uintptr_t tf = Transform(Comps(e));
        if (!tf || !mem::Readable(tf, kOff_Tf_ParentPos + 12)) return false;
        float v[3], pw[3];
        uint32_t parent = 0;
        if (!mem::ReadF32x3(tf + kOff_Tf_Pos, v) || !mem::Read32(tf + kOff_Tf_ParentEid, &parent)) return false;
        if (parent != 0xFFFFFFFF && parent != 0 && mem::ReadF32x3(tf + kOff_Tf_ParentPos, pw))
        {
            if (std::isfinite(pw[0]) && std::isfinite(pw[1]) && std::isfinite(pw[2]))
            { v[0] += pw[0]; v[1] += pw[1]; v[2] += pw[2]; }
        }
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
        out->x = v[0]; out->y = v[1]; out->z = v[2];
        return true;
    }

    uint32_t ParentEid(uintptr_t e)
    {
        const uintptr_t tf = Transform(Comps(e));
        uint32_t p = 0;
        if (!tf || !mem::Read32(tf + kOff_Tf_ParentEid, &p)) return 0;
        return p == 0xFFFFFFFF ? 0 : p;
    }

    // ---------------------------------------------------------- inventory ----
    static uint32_t g_inv[2048];
    static int      g_invN = 0;
    static DWORD    g_invAt = 0;
    static int      g_invCap = 0;       // slots across the buckets we can read, 0 when unknown
    static std::vector<std::pair<int, int>> g_invEach;   // per bucket: slots, occupied
    static unsigned g_slotStride = 0;   // 0xC0 (Trinity) or 0xC8 (CDLoot); probed on the live data
    static std::vector<std::pair<uint16_t, long long>> g_qty;

    // The two references disagree on the slot stride. Count slots that look
    // valid (type id inside the item table or the empty marker) under each
    // and keep the one that fits.
    static unsigned ProbeStride(uintptr_t slots, uint16_t sn)
    {
        const uint32_t tableN = ItemTableCount();
        const unsigned cands[2] = { 0xC0, 0xC8 };
        int best = -1; unsigned bestStride = 0xC8;
        for (unsigned st : cands)
        {
            if (!mem::Readable(slots, static_cast<size_t>(sn) * st)) continue;
            int ok = 0;
            for (uint16_t i = 0; i < sn && i < 64; ++i)
            {
                uint16_t type = 0;
                if (!mem::Read16(slots + static_cast<uintptr_t>(i) * st + kOff_Slot_TypeId, &type)) break;
                if (type == 0xFFFF || (tableN && type < tableN)) ++ok;
            }
            if (ok > best) { best = ok; bestStride = st; }
        }
        LOG("[inv] slot stride 0x%X (%d of %u slots plausible)", bestStride, best, sn < 64 ? sn : 64);
        return bestStride;
    }

    void InventoryRefresh(uintptr_t me, bool force)
    {
        const DWORD now = GetTickCount();
        if (!me || (!force && g_invN && now - g_invAt < 500)) return;
        g_invAt = now;
        int n = 0, cap = 0;
        std::vector<std::pair<int, int>> each;   // per bucket: slots, occupied
        std::vector<std::pair<uint16_t, long long>> qty;
        const uintptr_t comps  = Comps(me);
        const uintptr_t holder = comps ? mem::Deref(comps, kOff_Comps_InvHolder) : 0;
        if (!holder) { g_invN = 0; return; }
        uintptr_t barr = 0; uint32_t bn = 0;
        if (!mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) || !mem::Read32(holder + kOff_Inv_BucketN, &bn) || bn > 64) { g_invN = 0; return; }
        for (uint32_t b = 0; b < bn && n < 2048; ++b)
        {
            uintptr_t bk = 0, slots = 0; uint16_t sn = 0;
            if (!mem::ReadPtr(barr + 8ull * b, &bk)) continue;
            if (!mem::ReadPtr(bk + kOff_Bucket_Slots, &slots) || !mem::Read16(bk + kOff_Bucket_SlotN, &sn) || sn > 4096) continue;
            if (!g_slotStride && sn >= 8) g_slotStride = ProbeStride(slots, sn);
            const unsigned stride = g_slotStride ? g_slotStride : kInv_SlotStride;
            if (!mem::Readable(slots, static_cast<size_t>(sn) * stride)) continue;
            cap += sn;
            int hereN = 0;
            for (uint16_t i = 0; i < sn && n < 2048; ++i)
            {
                const uintptr_t s = slots + static_cast<uintptr_t>(i) * stride;
                uint16_t type = 0; uint32_t iid = 0;
                if (!mem::Read16(s + kOff_Slot_TypeId, &type) || type == 0xFFFF || type == 0) continue;
                if (!mem::Read32(s, &iid) || !iid || iid == 0xFFFFFFFF) continue;
                g_inv[n++] = iid; ++hereN;
                uint64_t q = 0;
                long long count = (mem::Read64(s + 0x10, &q) && q > 0 && q < 100000000ull) ? static_cast<long long>(q) : 1;
                qty.emplace_back(type, count);
            }
            each.emplace_back(static_cast<int>(sn), hereN);
        }
        g_invN = n;
        g_invEach = each;
        // The holder is not one bag: 18 buckets and 26,280 slots on 2760, which
        // is every store the player owns rather than what they are carrying.
        // Until one of them is known to be the bag, there is no capacity to
        // report, and the total is only written to the log to be looked at.
        g_invCap = 0;
        static bool s_capLogged = false;
        if (!s_capLogged && cap)
        {
            s_capLogged = true;
            char line[600]; int w = snprintf(line, sizeof line, "[inv] %u buckets, %d slots, %d holding something. Per bucket:", bn, cap, n);
            for (size_t i = 0; i < each.size() && w < 520; ++i)
                w += snprintf(line + w, sizeof line - w, " %u:%d/%d", static_cast<unsigned>(i), each[i].second, each[i].first);
            LOG("%s", line);
        }
        std::sort(qty.begin(), qty.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::pair<uint16_t, long long>> merged;
        for (const auto& e : qty)
        {
            if (!merged.empty() && merged.back().first == e.first) merged.back().second += e.second;
            else merged.push_back(e);
        }
        g_qty.swap(merged);
    }
    int InventoryTypes(uint16_t* types, long long* qty, int max)
    {
        const int n = static_cast<int>(g_qty.size()) < max ? static_cast<int>(g_qty.size()) : max;
        for (int i = 0; i < n; ++i) { types[i] = g_qty[i].first; qty[i] = g_qty[i].second; }
        return n;
    }
    bool InventoryHas(uint32_t iid)
    {
        if (!iid) return false;
        for (int i = 0; i < g_invN; ++i) if (g_inv[i] == iid) return true;
        return false;
    }
    int InventoryCount() { return g_invN; }
    int InventoryCapacity() { return g_invCap; }

    // ------------------------------------------------- inventory shape ----
    // Where the bag's limit is kept is not known: every bucket is allocated
    // 1,460 slots whatever it holds, so nothing there is a capacity. A limit
    // has to be a field that stays put while the count moves, so the holder
    // and the two buckets in use are sampled repeatedly with the count beside
    // them, and only values small enough to be a slot limit are printed.
    // Debug logging only, a dozen samples a session.
    void DumpInventoryShape(uintptr_t me, bool bagFull)
    {
        // Samples are only worth taking when the count has moved: a limit is
        // the field that does not move with it. One at the start, then one per
        // change, so filling and emptying the bag inside a session is enough.
        static int   s_dumps = 0, s_lastN = -1;
        static DWORD s_last = 0;
        const DWORD now = GetTickCount();
        if (s_dumps >= 24) return;
        if (s_dumps && g_invN == s_lastN) return;
        if (s_last && now - s_last < 1500) return;
        s_lastN = g_invN;
        const uintptr_t comps  = Comps(me);
        const uintptr_t holder = comps ? mem::Deref(comps, kOff_Comps_InvHolder) : 0;
        if (!holder) return;
        s_last = now; ++s_dumps;

        char line[1400];
        int w = snprintf(line, sizeof line, "[shape] %d held%s", g_invN, bagFull ? ", bag reads as full" : "");
        for (size_t i = 0; i < g_invEach.size() && w < 300; ++i)
            if (g_invEach[i].second) w += snprintf(line + w, sizeof line - w, " b%u=%d", static_cast<unsigned>(i), g_invEach[i].second);
        w += snprintf(line + w, sizeof line - w, " | holder:");
        for (unsigned off = 0; off < 0x200 && w < 1200; off += 4)
        {
            uint32_t v = 0;
            if (!mem::Read32(holder + off, &v) || !v || v > 4096) continue;
            w += snprintf(line + w, sizeof line - w, " +%X=%u", off, v);
        }
        LOG("%s", line);

        uintptr_t barr = 0; uint32_t bn = 0;
        if (!mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) || !mem::Read32(holder + kOff_Inv_BucketN, &bn) || bn > 64) return;
        for (uint32_t b = 0; b < bn && b < 18; ++b)
        {
            if (b < g_invEach.size() && !g_invEach[b].second) continue;   // only the ones holding something
            uintptr_t bk = 0;
            if (!mem::ReadPtr(barr + 8ull * b, &bk)) continue;
            w = snprintf(line, sizeof line, "[shape]   bucket %u (%d held):", b, b < g_invEach.size() ? g_invEach[b].second : -1);
            for (unsigned off = 0; off < 0x80 && w < 1200; off += 4)
            {
                uint32_t v = 0;
                if (!mem::Read32(bk + off, &v) || !v || v > 4096) continue;
                w += snprintf(line + w, sizeof line - w, " +%X=%u", off, v);
            }
            LOG("%s", line);
        }
    }


    // ------------------------------------------------------------- tables ----
    static int      g_tableState = 0;
    static unsigned g_defsOff = kOff_Table_DefsA;

    static uintptr_t DefFor(uintptr_t global, uint32_t row, unsigned defsOff)
    {
        uintptr_t table = 0, defs = 0, def = 0; uint32_t count = 0;
        if (!global || !mem::ReadPtr(global, &table)) return 0;
        if (!mem::Read32(table + kOff_Table_Count, &count) || !count || count > 0x40000 || row >= count) return 0;
        if (!mem::ReadPtr(table + defsOff, &defs)) return 0;
        if (!mem::ReadPtr(defs + 8ull * row, &def)) return 0;
        return def;
    }
    static bool KeyAt(uintptr_t global, uint32_t row, unsigned defsOff, char* out, size_t n)
    {
        const uintptr_t def = DefFor(global, row, defsOff);
        return def && mem::ReadEngineString(def + kOff_Def_StringKey, out, n) && strlen(out) >= 2;
    }

    uint32_t ItemTableCount()
    {
        uintptr_t table = 0; uint32_t count = 0;
        if (!g_f.itemTableGlobal || !mem::ReadPtr(g_f.itemTableGlobal, &table)) return 0;
        if (!mem::Read32(table + kOff_Table_Count, &count)) return 0;
        return count;
    }

    int ProbeItemTable()
    {
        if (g_tableState != 0) return g_tableState;
        if (!g_f.itemTableGlobal || !ItemDb::Loaded()) { g_tableState = -1; return g_tableState; }
        const uint32_t count = ItemTableCount();
        if (!count) return 0; // table not built yet; try again later
        const int dbN = ItemDb::Count();
        const int sample[] = { 0, 1, 2, 10, 100, 500, 1000, 2500, 4000, dbN - 1 };
        const unsigned offs[] = { kOff_Table_DefsA, kOff_Table_DefsB };
        int bestOff = -1, bestMatch = -1, bestReadable = -1;
        for (unsigned off : offs)
        {
            int match = 0, readable = 0;
            for (int row : sample)
            {
                if (row < 0 || static_cast<uint32_t>(row) >= count) continue;
                char key[96];
                if (!KeyAt(g_f.itemTableGlobal, static_cast<uint32_t>(row), off, key, sizeof key)) continue;
                ++readable;
                const Item* it = ItemDb::ByRow(row);
                if (it && it->stringKey == key) ++match;
            }
            if (match > bestMatch) { bestMatch = match; bestOff = static_cast<int>(off); bestReadable = readable; }
        }
        if (bestReadable <= 0) { g_tableState = -1; LOG_ERR("[table] iteminfo: no readable string keys at +0x50 or +0x58; item names unavailable"); return g_tableState; }
        g_defsOff = static_cast<unsigned>(bestOff);
        if (bestMatch >= 8) { g_tableState = 1; LOG_OK("[table] iteminfo: %u rows, defs at +0x%X, row ids match our database (%d/%d samples)", count, g_defsOff, bestMatch, bestReadable); }
        else { g_tableState = 2; LOG("[table] iteminfo: %u rows, defs at +0x%X, row ids differ from our database (%d/%d); matching by name", count, g_defsOff, bestMatch, bestReadable); }
        return g_tableState;
    }
    int ItemTableState() { return g_tableState; }

    bool ItemKeyForType(uint16_t typeId, char* out, size_t n)
    {
        if (!typeId || g_tableState <= 0) return false;
        return KeyAt(g_f.itemTableGlobal, typeId, g_defsOff, out, n);
    }
    bool GimmickKeyForType(uint16_t typeId, char* out, size_t n)
    {
        if (!typeId || !g_f.gimmickTableGlobal || g_tableState <= 0) return false;
        return KeyAt(g_f.gimmickTableGlobal, typeId, g_defsOff, out, n);
    }
    bool NodeName(uintptr_t gimmick, char* out, size_t n)
    {
        if (!gimmick) return false;
        return mem::ReadEngineString(gimmick + kOff_Gimmick_NodeName, out, n) && strlen(out) >= 4;
    }

    bool NodePrefab(uintptr_t gimmick, char* out, size_t n)
    {
        if (!gimmick) return false;
        const uintptr_t a = mem::Deref(gimmick, kOff_Gimmick_Prefab);
        if (a && mem::ReadEngineString(a + kOff_Prefab_Path, out, n) && strstr(out, ".prefab")) return true;
        const uintptr_t b = mem::Deref(gimmick, kOff_Gimmick_PrefabAlt);
        if (b && mem::ReadEngineString(b + kOff_PrefabAlt_Path, out, n) && strstr(out, ".prefab")) return true;
        out[0] = 0;
        return false;
    }

    // ------------------------------------------------------- table sweep ----
    // The engine reaches every static table through the same three
    // instructions, so one scan lists them all with their row counts. Used to
    // ask what an unknown id could be a row of.
    static TableRef g_tables[192];
    static int      g_tableN = -1;

    static void NameTable(uintptr_t resolver, char* out, size_t n)
    {
        out[0] = '\0';
        // The miss path formats the table's own name; it is the first
        // lower-case identifier the function points at.
        for (uintptr_t p = resolver; p < resolver + 0x180; ++p)
        {
            if (!mem::MatchAt(p, "4C 8D 05") && !mem::MatchAt(p, "48 8D 15") && !mem::MatchAt(p, "48 8D 0D")) continue;
            char buf[32];
            if (!mem::ReadCString(mem::RipAt(p, 7), buf, sizeof buf)) continue;
            size_t len = strlen(buf);
            if (len < 4 || len > 30) continue;
            bool ok = true;
            for (size_t i = 0; i < len; ++i)
                if (!((buf[i] >= 'a' && buf[i] <= 'z') || (buf[i] >= '0' && buf[i] <= '9') || buf[i] == '_')) { ok = false; break; }
            if (!ok) continue;
            strncpy(out, buf, n - 1); out[n - 1] = '\0';
            return;
        }
    }

    static bool CollectTable(uintptr_t hit, void*)
    {
        if (g_tableN >= static_cast<int>(sizeof g_tables / sizeof g_tables[0])) return true;
        const uintptr_t g = mem::RipAt(hit + kOff_TableIndex_MovGlobal, 7);
        if (!mem::InImage(g)) return false;
        for (int i = 0; i < g_tableN; ++i) if (g_tables[i].global == g) return false;
        uintptr_t table = 0; uint32_t count = 0;
        if (!mem::ReadPtr(g, &table) || !mem::Read32(table + kOff_Table_Count, &count) || !count || count > 0x40000) return false;
        TableRef& t = g_tables[g_tableN++];
        t.global = g; t.count = count;
        NameTable(hit, t.name, sizeof t.name);
        return false;
    }

    int EnumTables(const TableRef** out)
    {
        if (g_tableN < 0)
        {
            g_tableN = 0;
            mem::FindIf(kSig_TableIndex, CollectTable, nullptr);
            LOG("[table] %d static tables found in the image", g_tableN);
        }
        if (out) *out = g_tables;
        return g_tableN;
    }

    bool KeyInTable(uintptr_t global, uint32_t row, char* out, size_t n)
    {
        for (unsigned off : { kOff_Table_DefsA, kOff_Table_DefsB })
            if (KeyAt(global, row, off, out, n)) return true;
        return false;
    }
}
