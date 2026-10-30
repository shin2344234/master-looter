#include "game.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "farhook.h"
#include "mem.h"
#include "signatures.h"
#include "../core/itemdb.h"
#include "../core/log.h"
#include "../core/settings.h"

namespace ml::game
{
    using namespace ml::sig;

    // ------------------------------------------------------------ resolve ----
    static Fns      g_f;
    static SigResult g_sigs[16];
    static int       g_sigN = 0;
    static bool      g_requiredOk = false;

    static void OwnerOf(uintptr_t dest, char* owner, size_t n)
    {
        strncpy_s(owner, n, "an unknown module", _TRUNCATE);
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(dest), &mod) && mod)
        {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mod, path, MAX_PATH))
            {
                const char* base = strrchr(path, '\\');
                strncpy_s(owner, n, base ? base + 1 : path, _TRUNCATE);
            }
        }
    }

    // A pattern that starts at a function entry finds nothing once another mod
    // has put its jump there. Trinity.asi writes a five-byte E9 over
    // move_update and own_check; CDAutoLoot writes mov rax, imm64; jmp rax over
    // the event queue, own_check and node arming, twelve bytes plus two or
    // three of its own. The event queue is required, so where CDAutoLoot loads
    // first the whole engine used to switch itself off. Each form is tried
    // with the bytes it can cover wildcarded and the rest of the pattern
    // intact, and a match only counts when it is unique and the jump there
    // leaves the game module.
    static uintptr_t FindUnderPatch(const char* pattern, char* owner, size_t ownerN)
    {
        std::vector<std::string> t;
        for (const char* p = pattern; *p; )
        {
            while (*p == ' ') ++p;
            const char* q = p;
            while (*q && *q != ' ') ++q;
            if (q > p) t.emplace_back(p, q);
            p = q;
        }
        struct Form { const char* head; unsigned headN, cover, minTail; bool rel; };
        static const Form forms[] = {
            { "E9 ?? ?? ?? ??", 5, 8, 8, true },
            { "48 B8 ?? ?? ?? ?? ?? ?? ?? ?? FF E0", 12, 16, 6, false },
            { "FF 25 00 00 00 00 ?? ?? ?? ?? ?? ?? ?? ??", 14, 16, 6, false },
        };
        for (const Form& f : forms)
        {
            if (t.size() < f.cover + f.minTail) continue;
            std::string v = f.head;
            for (unsigned i = f.headN; i < f.cover; ++i) v += " ??";
            for (size_t i = f.cover; i < t.size(); ++i) { v += ' '; v += t[i]; }
            size_t hits = 0;
            const uintptr_t j = mem::FindUnique(v.c_str(), &hits);
            if (!j) continue;
            const uintptr_t dest = f.rel ? mem::RipAt(j, 5) : farhook::AbsJumpTarget(j);
            if (!dest || mem::InImage(dest)) continue;
            OwnerOf(dest, owner, ownerN);
            return j;
        }
        return 0;
    }

    uintptr_t FindEntry(const char* pattern, size_t* hits)
    {
        uintptr_t a = mem::FindUnique(pattern, hits);
        char owner[MAX_PATH] = "";
        if (!a && !*hits && (a = FindUnderPatch(pattern, owner, sizeof owner)) != 0)
        {
            *hits = 1;
            LOG("[sig] +0x%llX found under a jump %s put over its entry", static_cast<unsigned long long>(mem::Rva(a)), owner);
        }
        return a;
    }

    static uintptr_t Scan(const char* name, const char* pattern, bool required)
    {
        size_t hits = 0;
        uintptr_t a = mem::FindUnique(pattern, &hits);
        char owner[MAX_PATH] = "";
        if (!a && !hits) a = FindUnderPatch(pattern, owner, sizeof owner);
        if (g_sigN < 16) g_sigs[g_sigN++] = { name, a, a ? 1u : hits, required };
        if (a && owner[0])
            LOG("[sig] %-14s +0x%llX, under a jump %s put over its entry; hooking on top of it",
                name, static_cast<unsigned long long>(mem::Rva(a)), owner);
        else if (a) LOG("[sig] %-14s +0x%llX", name, static_cast<unsigned long long>(mem::Rva(a)));
        else   LOG_ERR("[sig] %-14s %s (%zu hits)%s", name, hits ? "AMBIGUOUS" : "not found", hits, required ? " - required" : "");
        return a;
    }

    // A function another mod has already detoured starts with that mod's jump,
    // so a pattern that begins at the entry finds nothing. kaster21's log on 19
    // September 2026 had own_check and move_update both at 0 hits with
    // Trinity.asi loaded, and without own_check no node, body or dropped item
    // is ever asked about, so nothing but catches was looted. The second
    // pattern carries the jump in place of the bytes it covers, and a match
    // only counts when the jump leaves the game module.
    static uintptr_t ScanJumped(const char* name, const char* pattern, const char* jumped)
    {
        const uintptr_t a = Scan(name, pattern, false);
        if (a) return a;
        size_t hits = 0;
        const uintptr_t j = mem::FindUnique(jumped, &hits);
        if (!j) return 0;
        const uintptr_t dest = mem::RipAt(j, 5);
        if (mem::InImage(dest)) { LOG_ERR("[sig] %-14s under a jump that stays in the game, so not taken", name); return 0; }
        char owner[MAX_PATH];
        OwnerOf(dest, owner, sizeof owner);
        for (int i = g_sigN - 1; i >= 0; --i)
            if (g_sigs[i].name == name) { g_sigs[i].addr = j; g_sigs[i].hits = hits; break; }
        LOG("[sig] %-14s +0x%llX, under a jump %s put over its entry; hooking on top of it",
            name, static_cast<unsigned long long>(mem::Rva(j)), owner);
        return j;
    }

    // Where a function is named by a call to it and its entry may be anyone's.
    // The body check past the patched bytes is what keeps a stray match from
    // being taken for it.
    static uintptr_t ScanFromCaller(const char* name, const char* caller, unsigned callAt, const char* body, unsigned bodyAt)
    {
        size_t hits = 0;
        const uintptr_t site = mem::FindUnique(caller, &hits);
        if (!site) return 0;
        const uintptr_t fn = mem::RipAt(site + callAt, 5);
        if (!mem::InImage(fn) || !mem::MatchAt(fn + bodyAt, body))
        {
            LOG_ERR("[sig] %-14s its call site names +0x%llX, which is not the function it should be; not taken",
                    name, static_cast<unsigned long long>(mem::Rva(fn)));
            return 0;
        }
        uint8_t e[12] = {};
        for (unsigned i = 0; i < sizeof e; ++i) mem::Read8(fn + i, &e[i]);
        for (int i = g_sigN - 1; i >= 0; --i)
            if (g_sigs[i].name && strcmp(g_sigs[i].name, name) == 0) { g_sigs[i].addr = fn; g_sigs[i].hits = 1; break; }
        LOG("[sig] %-14s +0x%llX, found from its caller; its entry reads %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X, "
            "so another mod has patched it and this one will stack on the patch",
            name, static_cast<unsigned long long>(mem::Rva(fn)), e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11]);
        return fn;
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
        g_f.moveUpdate   = ScanJumped("move_update", kSig_MoveUpdate, kSig_MoveUpdateJumped);
        g_f.areaSweepHit = Scan("area_sweep", kSig_AreaSweep, false);
        g_f.ownCheck     = ScanJumped("own_check", kSig_OwnCheck, kSig_OwnCheckJumped);
        if (!g_f.ownCheck)
            g_f.ownCheck = ScanFromCaller("own_check", kSig_OwnCheckCaller, kOff_OwnCheckCaller_Call, kSig_OwnCheckBody, kOff_OwnCheckBody);
        g_f.armFn        = Scan("node_arm", kSig_ArmDispatch, false);
        {
            // The two displacements come out of the accessor itself: the first
            // mov rdi, [rcx+disp8] and the first mov r14, [rax+disp32] in it.
            size_t hits = 0;
            const uintptr_t site = mem::FindUnique(kSig_ControlSite, &hits);
            const uintptr_t g = site ? mem::RipAt(site, 7) : 0;
            const uintptr_t acc = site ? mem::RipAt(site + kOff_ControlSite_Call, 5) : 0;
            unsigned ctl = 0, act = 0;
            for (unsigned i = 0; acc && i < 0x180 && (!ctl || !act); ++i)
            {
                uint8_t d8 = 0; uint32_t d32 = 0;
                if (!ctl && mem::MatchAt(acc + i, "48 8B 79") && mem::Read8(acc + i + 3, &d8)) ctl = d8;
                else if (ctl && !act && mem::MatchAt(acc + i, "4C 8B B0") && mem::Read32(acc + i + 3, &d32) && d32 < 0x1000) act = d32;
            }
            if (g && mem::InImage(g) && ctl && act)
            {
                g_f.controlGlobal = g; g_f.ctlController = ctl; g_f.ctlActor = act;
                LOG("[sig] controller     global +0x%llX, accessor +0x%llX, controller at +0x%X, its actor at +0x%X",
                    static_cast<unsigned long long>(mem::Rva(g)), static_cast<unsigned long long>(mem::Rva(acc)), ctl, act);
            }
            else LOG("[sig] controller     not resolved (%zu site hits, controller +0x%X, actor +0x%X); the scan does not use it",
                     hits, ctl, act);
        }
        g_f.stateDriver  = Scan("gimmick_driver", kSig_StateDriver, false);
        g_f.invHolder    = Scan("inv_holder", kSig_InvHolder, false);
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

    uint32_t NameId(const char* name)
    {
        if (!name) return 0;
        const unsigned char* k = reinterpret_cast<const unsigned char*>(name);
        size_t len = 0; while (k[len]) ++len;
        auto rot = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
        uint32_t a, b, c;
        a = b = c = static_cast<uint32_t>(len) + 0xDEBA1DCDu;
        size_t i = 0;
        while (len - i > 12)
        {
            a += k[i] | (k[i+1] << 8) | (k[i+2] << 16) | (k[i+3] << 24);
            b += k[i+4] | (k[i+5] << 8) | (k[i+6] << 16) | (k[i+7] << 24);
            c += k[i+8] | (k[i+9] << 8) | (k[i+10] << 16) | (k[i+11] << 24);
            a -= c; a ^= rot(c, 4);  c += b;
            b -= a; b ^= rot(a, 6);  a += c;
            c -= b; c ^= rot(b, 8);  b += a;
            a -= c; a ^= rot(c, 16); c += b;
            b -= a; b ^= rot(a, 19); a += c;
            c -= b; c ^= rot(b, 4);  b += a;
            i += 12;
        }
        unsigned char t[12] = {};
        for (size_t j = i; j < len; ++j) t[j - i] = k[j];
        if (len - i)
        {
            a += t[0] | (t[1] << 8) | (t[2] << 16) | (t[3] << 24);
            b += t[4] | (t[5] << 8) | (t[6] << 16) | (t[7] << 24);
            c += t[8] | (t[9] << 8) | (t[10] << 16) | (t[11] << 24);
            c ^= b; c -= rot(b, 14);
            a ^= c; a -= rot(c, 11);
            b ^= a; b -= rot(a, 25);
            c ^= b; c -= rot(b, 16);
            a ^= c; a -= rot(c, 4);
            b ^= a; b -= rot(a, 14);
            c ^= b; c -= rot(b, 24);
        }
        return c;
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
            // There is more than one of these in memory and the first found is
            // not necessarily the live one. Playing as Damiane the mod latched
            // a manager holding four entities while the world had hundreds, so
            // the scan measured from an actor 1700 m from anything and the
            // Nearby list stayed empty. Take the one actually holding a world.
            uintptr_t slots[16]; const int n = mem::FindGlobalsHoldingVtable(g_mgrVt, g_mgrVtN, slots, 16);
            if (!n) { static bool told = false; if (!told) { told = true; LOG("[mgr] not in memory yet; retrying"); } return 0; }
            int bestN = -1;
            char line[240]; int w = 0;
            for (int i = 0; i < n; ++i)
            {
                uintptr_t inst = 0;
                int held = 0;
                if (mem::ReadPtr(slots[i], &inst) && inst && mem::Readable(inst, kOff_Mgr_ListsEnd))
                    for (unsigned off = kOff_Mgr_ListsBegin; off + 16 <= kOff_Mgr_ListsEnd; off += 8)
                    {
                        uint32_t count = 0, cap = 0; uintptr_t arr = 0;
                        if (mem::Read32(inst + off, &count) && mem::Read32(inst + off + 4, &cap) &&
                            mem::ReadPtr(inst + off + 8, &arr) && count && cap && count <= cap && cap <= 0x10000)
                            held += static_cast<int>(count);
                    }
                if (w < 200) w += snprintf(line + w, sizeof line - w, " +%llX:%d",
                                           static_cast<unsigned long long>(mem::Rva(slots[i])), held);
                if (bestN < 0 || held > bestN) { bestN = held; g_mgrSlot = slots[i]; }
            }
            LOG_OK("[mgr] %d ClientActorManager globals, entities in each:%s; using +0x%llX", n, w ? line : " none",
                   static_cast<unsigned long long>(mem::Rva(g_mgrSlot)));
        }
        // The counts above are taken the moment the globals are found, which is
        // before the world exists: both candidates read zero and the pick was a
        // toss-up. Re-check once, a little later, when there is something to
        // count.
        static bool s_recheck = false;
        static DWORD s_recheckAt = GetTickCount() + 20000;
        if (!s_recheck && static_cast<LONG>(GetTickCount() - s_recheckAt) >= 0)
        {
            s_recheck = true;
            g_mgrSlot = 0; g_mgrNextTry = 0;
            LOG("[mgr] re-checking which manager holds the world now that one is loaded");
            return 0;
        }
        uintptr_t p = 0;
        if (!mem::ReadPtr(g_mgrSlot, &p)) return 0;
        return mem::Readable(p, kOff_Mgr_ListsEnd) ? p : 0;
    }
    bool ActorManagerFound() { return g_mgrSlot != 0; }

    uintptr_t ControlledActor()
    {
        if (!g_f.controlGlobal) return 0;
        const uintptr_t svcs = mem::Deref(g_f.controlGlobal, 0);
        const uintptr_t svc  = svcs ? mem::Deref(svcs, kOff_ControlSite_Svc) : 0;
        const uintptr_t ctl  = svc ? mem::Deref(svc, g_f.ctlController) : 0;
        const uintptr_t a    = ctl ? mem::Deref(ctl, g_f.ctlActor) : 0;
        if (!a || !mem::Readable(a, 0x100)) return 0;
        uint32_t eid = 0;
        return Eid(a, &eid) && eid ? a : 0;
    }
    uintptr_t ActorManagerSlot() { return g_mgrSlot; }

    // ----------------------------------------------------------- entities ----
    bool Eid(uintptr_t e, uint32_t* out) { return mem::Read32(e + kOff_Ent_Eid, out); }

    // The answer GetInventoryHolder last gave, and for which actor. Written on
    // the game thread and read by the worker, so the pair sits under one lock:
    // a holder read against the wrong actor is a bag of somebody else's.
    // g_holderKnown separates "the game said this actor has no bag", which is
    // an answer and stays 0, from "the game has not been asked yet", which
    // falls back to the actor's own holder.
    static SRWLOCK   g_holderLock = SRWLOCK_INIT;
    static uintptr_t g_holderFor = 0, g_holderIs = 0;
    static bool      g_holderKnown = false;
    static uintptr_t g_invHolderRead = 0;

    static uintptr_t OwnHolder(uintptr_t actor)
    {
        const uintptr_t comps = actor ? Comps(actor) : 0;
        return comps ? mem::Deref(comps, kOff_Comps_InvHolder) : 0;
    }

    // False when the call faulted, which is no answer at all rather than an
    // answer of no bag.
    static bool CallInvHolder(uintptr_t fn, uintptr_t actor, uintptr_t* out)
    {
        __try { *out = reinterpret_cast<uintptr_t (*)(uintptr_t)>(fn)(actor); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return false; }
    }

    void RefreshHolder(uintptr_t actor)
    {
        const uintptr_t fn = g_f.invHolder;
        if (!fn || !actor || !mem::Readable(actor, 0x100)) return;
        uintptr_t h = 0;
        if (!CallInvHolder(fn, actor, &h)) return;
        if (h && !mem::Readable(h, 0x40)) return;   // an answer that does not read is no answer
        const uintptr_t use = h;
        AcquireSRWLockExclusive(&g_holderLock);
        const bool changed = g_holderFor != actor || g_holderIs != use || !g_holderKnown;
        g_holderFor = actor; g_holderIs = use; g_holderKnown = true;
        ReleaseSRWLockExclusive(&g_holderLock);
        if (!changed || !use) return;
        const uintptr_t own = OwnHolder(actor);
        uint32_t who = 0, whose = 0;
        Eid(actor, &who);
        uintptr_t owner = 0;
        if (mem::ReadPtr(use + 8, &owner) && owner) Eid(owner, &whose);
        static uint32_t s_said = 0;
        if (use != own && s_said != who)
        {
            s_said = who;
            LOG("[inv] %08X keeps no bag of its own: the game hands it the bag of %08X, which is the one read from now on",
                who, whose);
        }
    }

    uintptr_t Holder(uintptr_t actor)
    {
        if (!actor) return 0;
        AcquireSRWLockShared(&g_holderLock);
        const bool known = g_holderKnown && g_holderFor == actor;
        const uintptr_t h = known ? g_holderIs : 0;
        ReleaseSRWLockShared(&g_holderLock);
        return known ? h : OwnHolder(actor);
    }

    void ForgetHolder()
    {
        AcquireSRWLockExclusive(&g_holderLock);
        g_holderFor = 0; g_holderIs = 0; g_holderKnown = false;
        ReleaseSRWLockExclusive(&g_holderLock);
    }

    uintptr_t LastInventoryHolder() { return g_invHolderRead; }

    uint32_t Route(uintptr_t e) { uint32_t r = 0; mem::Read32(e + kOff_Ent_Route, &r); return r; }
    uint8_t TypeTag(uintptr_t e)
    {
        const uintptr_t ti = mem::Deref(e, kOff_Ent_TypeInfo);
        uint8_t t = 0xFF;
        if (!ti || !mem::Read8(ti + 1, &t)) return 0xFF;
        return t;
    }
    uintptr_t Comps(uintptr_t e) { return mem::Deref(e, kOff_Ent_Comps); }

    // The status component's second category byte, read straight off an
    // entity. Two derefs and a slot-cache hit, so it can be asked of every
    // parented object the enumeration hands over, not only the ones in range.
    uint8_t Cat2(uintptr_t e)
    {
        const uintptr_t status = CompByClass(Comps(e), kCls_Status);
        uint8_t c = 0;
        if (!status || !mem::Read8(status + kOff_Status_Cat2, &c)) return 0;
        return c;
    }

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
    static int      g_bagUsed = 0, g_bagCap = 0;        // the carried bag, 0 when unreadable
    static unsigned g_slotStride = 0;   // 0xC0 (Trinity) or 0xC8 (CDLoot); probed on the live data
    static std::vector<std::pair<uint16_t, long long>> g_qty;
    // What the carried bag alone holds. Kept apart because the total above
    // spans every storage the player owns, and a storage mod moving a stack
    // from the bag into one of them takes it out of one bucket and puts it in
    // another in two steps; a read that falls between them counts it twice.
    static std::vector<std::pair<uint16_t, long long>> g_bagQty;
    static bool g_bagQtyOk = false;

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
        g_bagQtyOk = false;
        std::vector<std::pair<uint16_t, long long>> bagQty;
        bool bagRead = false;
        int n = 0, cap = 0;
        std::vector<std::pair<int, int>> each;   // per bucket: slots, occupied
        std::vector<std::pair<uint16_t, long long>> qty;
        const uintptr_t holder = Holder(me);
        g_invHolderRead = holder;
        if (!holder) { g_invN = 0; g_qty.clear(); g_bagQty.clear(); return; }
        uintptr_t barr = 0; uint32_t bn = 0;
        if (!mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) || !mem::Read32(holder + kOff_Inv_BucketN, &bn) || bn > 64)
        { g_invN = 0; g_invHolderRead = 0; g_qty.clear(); g_bagQty.clear(); return; }
        for (uint32_t b = 0; b < bn && n < 2048; ++b)
        {
            uintptr_t bk = 0, slots = 0; uint16_t sn = 0;
            if (!mem::ReadPtr(barr + 8ull * b, &bk)) continue;
            if (!mem::ReadPtr(bk + kOff_Bucket_Slots, &slots) || !mem::Read16(bk + kOff_Bucket_SlotN, &sn) || sn > 4096) continue;
            if (b == kInv_BagBucket)
            {
                uint16_t used = 0, cap = 0;
                if (mem::Read16(bk + kOff_Bucket_Used, &used) && mem::Read16(bk + kOff_Bucket_Cap, &cap) &&
                    cap >= 8 && cap <= 4096 && used <= cap + 512)
                {
                    // Re-read every refresh rather than kept, so expanding the
                    // bag is picked up within half a second. A change is worth
                    // a line: it is also the confirmation that this field is
                    // the limit and not something that merely looks like it.
                    if (!g_bagCap) LOG("[inv] carried bag holds %u slots, %u in use", cap, used);
                    else if (cap != g_bagCap) LOG("[inv] carried bag limit changed from %d to %u slots", g_bagCap, cap);
                    g_bagUsed = used; g_bagCap = cap;
                }
                else g_bagUsed = g_bagCap = 0;
            }
            if (!g_slotStride && sn >= 8) g_slotStride = ProbeStride(slots, sn);
            const unsigned stride = g_slotStride ? g_slotStride : kInv_SlotStride;
            if (!mem::Readable(slots, static_cast<size_t>(sn) * stride)) continue;
            if (b == kInv_BagBucket) bagRead = true;
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
                if (b == kInv_BagBucket) bagQty.emplace_back(type, count);
            }
            each.emplace_back(static_cast<int>(sn), hereN);
        }
        g_invN = n;
        // Issue #32: once, the instance ids and type counts, debug only, so a
        // discard request's payload can be matched against something known.
        {
            static bool s_dumped = false;
            if (!s_dumped && g_invN > 0 && Settings::Get().debugLog)
            {
                s_dumped = true;
                char line[1200]; int w = 0;
                for (int i = 0; i < g_invN && i < 48 && w < 1100; ++i) w += snprintf(line + w, sizeof line - w, " %08X", g_inv[i]);
                LOG("[invdump] %d instance ids (first 48):%s", g_invN, line);
                w = 0;
                for (size_t i = 0; i < g_qty.size() && i < 60 && w < 1100; ++i) w += snprintf(line + w, sizeof line - w, " %u x%lld", g_qty[i].first, g_qty[i].second);
                LOG("[invdump] %d item types with counts:%s", static_cast<int>(g_qty.size()), line);
            }
        }
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
        std::sort(bagQty.begin(), bagQty.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        merged.clear();
        for (const auto& e : bagQty)
        {
            if (!merged.empty() && merged.back().first == e.first) merged.back().second += e.second;
            else merged.push_back(e);
        }
        g_bagQty.swap(merged);
        g_bagQtyOk = bagRead;
    }
    int BagTypes(uint16_t* types, long long* qty, int max)
    {
        if (!g_bagQtyOk) return -1;
        const int n = static_cast<int>(g_bagQty.size()) < max ? static_cast<int>(g_bagQty.size()) : max;
        for (int i = 0; i < n; ++i) { types[i] = g_bagQty[i].first; qty[i] = g_bagQty[i].second; }
        return n;
    }
    int InventoryTypes(uint16_t* types, long long* qty, int max)
    {
        const int n = static_cast<int>(g_qty.size()) < max ? static_cast<int>(g_qty.size()) : max;
        for (int i = 0; i < n; ++i) { types[i] = g_qty[i].first; qty[i] = g_qty[i].second; }
        return n;
    }
    uintptr_t HolderNow(uintptr_t actor)
    {
        uintptr_t h = 0;
        if (!g_f.invHolder || !actor || !mem::Readable(actor, 0x100)) return 0;
        if (!CallInvHolder(g_f.invHolder, actor, &h)) return 0;
        return h && mem::Readable(h, 0x40) ? h : 0;
    }

    int InventoryEntries(uintptr_t me, InvEntry* out, int max, uintptr_t* holderOut)
    {
        const uintptr_t holder = Holder(me);
        if (holderOut) *holderOut = holder;
        return HolderEntries(holder, out, max);
    }

    int HolderEntries(uintptr_t holder, InvEntry* out, int max)
    {
        int n = 0;
        if (!holder || !out || max <= 0) return 0;
        uintptr_t barr = 0; uint32_t bn = 0;
        if (!mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) || !mem::Read32(holder + kOff_Inv_BucketN, &bn) || bn > 64) return 0;
        const unsigned stride = g_slotStride ? g_slotStride : kInv_SlotStride;
        for (uint32_t b = 0; b < bn && n < max; ++b)
        {
            uintptr_t bk = 0, slots = 0; uint16_t sn = 0;
            if (!mem::ReadPtr(barr + 8ull * b, &bk)) continue;
            if (!mem::ReadPtr(bk + kOff_Bucket_Slots, &slots) || !mem::Read16(bk + kOff_Bucket_SlotN, &sn) || sn > 4096) continue;
            if (!mem::Readable(slots, static_cast<size_t>(sn) * stride)) continue;
            for (uint16_t i = 0; i < sn && n < max; ++i)
            {
                const uintptr_t s = slots + static_cast<uintptr_t>(i) * stride;
                uint16_t type = 0; uint32_t iid = 0;
                if (!mem::Read16(s + kOff_Slot_TypeId, &type) || type == 0xFFFF || type == 0) continue;
                if (!mem::Read32(s, &iid) || !iid || iid == 0xFFFFFFFF) continue;
                uint64_t q = 0;
                const long long count = (mem::Read64(s + 0x10, &q) && q > 0 && q < 100000000ull) ? static_cast<long long>(q) : 1;
                out[n++] = { iid, type, static_cast<int>(b), static_cast<int>(i), count, s };
            }
        }
        return n;
    }

    int InventoryBuckets(uintptr_t me, BucketInfo* out, int max)
    {
        return HolderBuckets(Holder(me), out, max);
    }

    int HolderBuckets(uintptr_t holder, BucketInfo* out, int max)
    {
        int n = 0;
        if (!holder || !out || max <= 0) return 0;
        uintptr_t barr = 0; uint32_t bn = 0;
        if (!mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) || !mem::Read32(holder + kOff_Inv_BucketN, &bn) || bn > 64) return 0;
        for (uint32_t b = 0; b < bn && n < max; ++b)
        {
            BucketInfo bi = {};
            if (!mem::ReadPtr(barr + 8ull * b, &bi.addr)) { bi.type = 0xFFFF; out[n++] = bi; continue; }
            mem::Read16(bi.addr + 0x10, &bi.type);
            mem::Read16(bi.addr + kOff_Bucket_SlotN, &bi.slotN);
            mem::Read16(bi.addr + 0x0C, &bi.capC);
            mem::Read16(bi.addr + kOff_Bucket_Used, &bi.used);
            mem::Read16(bi.addr + kOff_Bucket_Cap, &bi.cap);
            mem::Read32(bi.addr + 0x28, &bi.exclN);
            out[n++] = bi;
        }
        return n;
    }

    static uintptr_t InvTypeTableGlobal()
    {
        static uintptr_t g = 0; static DWORD lastTry = 0;
        if (g) return g;
        const DWORD now = GetTickCount();
        if (lastTry && now - lastTry < 10000) return 0;
        lastTry = now;
        const TableRef* t = nullptr;
        const int n = EnumTables(&t);
        char k0[40], k1[40];
        for (int i = 0; i < n; ++i)
        {
            if (t[i].count < 8 || t[i].count > 64) continue;
            if (!KeyInTable(t[i].global, 0, k0, sizeof k0) || _stricmp(k0, "Money") != 0) continue;
            if (!KeyInTable(t[i].global, 1, k1, sizeof k1) || _stricmp(k1, "Character") != 0) continue;
            g = t[i].global;
            LOG("[table] inventory types: %u rows, global +0x%llX", t[i].count, static_cast<unsigned long long>(mem::Rva(g)));
            break;
        }
        if (!g) LOG_ERR("[table] no inventory-type table among %d; nothing can be deleted from the inventory", n);
        return g;
    }
    static uintptr_t InvTypeTable()
    {
        uintptr_t table = 0;
        const uintptr_t g = InvTypeTableGlobal();
        if (!g || !mem::ReadPtr(g, &table)) return 0;
        return table;
    }
    int InvTypeRows(InvTypeRow* out, int max, uint32_t* countOut)
    {
        const uintptr_t table = InvTypeTable();
        uint32_t count = 0; uintptr_t defs = 0;
        if (countOut) *countOut = 0;
        if (!table || !mem::Read32(table + kOff_Table_Count, &count) || !count || count > 0x4000) return 0;
        if (countOut) *countOut = count;
        if (!mem::ReadPtr(table + kOff_Table_DefsA, &defs)) return 0;
        int n = 0;
        for (uint32_t r = 0; r < count && n < max; ++r)
        {
            InvTypeRow row = { 0xFFFF, 0xFFFF, "" };
            uintptr_t def = 0;
            if (mem::ReadPtr(defs + 8ull * r, &def) && def)
            {
                mem::Read16(def + kOff_Def_Key16, &row.id); row.idx = static_cast<uint16_t>(r);
                if (!mem::ReadEngineString(def + kOff_Def_StringKey, row.name, sizeof row.name)) row.name[0] = 0;
            }
            else strcpy_s(row.name, "(unloaded)");
            out[n++] = row;
        }
        return n;
    }
    // The lookup at 0x8752A40 on 2760: bucket = key % [table+0x68], each
    // bucket 0x100 bytes holding a count then {key u32, index u32} pairs from
    // +8; the index goes into the node array at [table+0x80], and the node
    // must carry the key at +4. Result is the u16 at +6. The nodes are not the
    // defs. On 1.0.0.3392 every 16-bit-keyed lookup the ItemInfo loader calls
    // (categoryinfo's at RVA +0x172EE20) still reads exactly this shape.
    static uint16_t InvTypeHashLookup(uintptr_t table, uint16_t key)
    {
        uint32_t flag = 0, bc = 0; uintptr_t buckets = 0, rows = 0;
        if (!table || !mem::Read32(table + 0x6C, &flag) || !flag || !mem::Read32(table + 0x68, &bc) || !bc) return 0xFFFF;
        if (!mem::ReadPtr(table + 0x78, &buckets) || !mem::ReadPtr(table + 0x80, &rows)) return 0xFFFF;
        const uintptr_t b = buckets + static_cast<uintptr_t>(key % bc) * 0x100;
        uint32_t n = 0;
        if (!mem::Read32(b, &n)) return 0xFFFF;
        for (uint32_t i = 0; i < n && i < 31; ++i)
        {
            uint32_t k = 0, idx = 0;
            if (!mem::Read32(b + 8 + 8ull * i, &k) || k != key) continue;
            if (!mem::Read32(b + 0xC + 8ull * i, &idx)) continue;
            uintptr_t row = 0; uint16_t rk = 0, rv = 0;
            if (!mem::ReadPtr(rows + 8ull * idx, &row) || !mem::Read16(row + 4, &rk) || rk != key) continue;
            if (mem::Read16(row + 6, &rv)) return rv;
        }
        return 0xFFFF;
    }
    // Behind it, the defs themselves: since 1.0.0.3392 an InventoryInfo def
    // holds its _key as the u16 at +8, and the type a bucket names is the
    // def's row (Money is key 1 at row 0, the bag key 2 at row 1). Used only
    // when the map gives no answer, and the log says when that happens.
    static uint16_t InvTypeDefLookup(uintptr_t table, uint16_t key)
    {
        uint32_t count = 0; uintptr_t defs = 0;
        if (!table || !mem::Read32(table + kOff_Table_Count, &count) || !count || count > 0x4000) return 0xFFFF;
        if (!mem::ReadPtr(table + kOff_Table_DefsA, &defs)) return 0xFFFF;
        for (uint32_t r = 0; r < count; ++r)
        {
            uintptr_t def = 0; uint16_t k = 0;
            if (mem::ReadPtr(defs + 8ull * r, &def) && def && mem::Read16(def + kOff_Def_Key16, &k) && k == key)
                return static_cast<uint16_t>(r);
        }
        return 0xFFFF;
    }
    uint16_t InvTypeLookup(uint16_t key)
    {
        const uintptr_t table = InvTypeTable();
        const uint16_t v = InvTypeHashLookup(table, key);
        if (v != 0xFFFF) return v;
        const uint16_t d = InvTypeDefLookup(table, key);
        static int s_said = 0;
        if (d != 0xFFFF && s_said < 4)
        {
            ++s_said;
            LOG("[table] inventory key %u: the game's lookup gave nothing, its def says type %u", key, d);
        }
        return d;
    }
    int InvTypeKeysFor(uint16_t value, uint16_t* out, int max)
    {
        const uintptr_t table = InvTypeTable();
        uint32_t flag = 0, bc = 0; uintptr_t buckets = 0, rows = 0; int n = 0;
        if (table && mem::Read32(table + 0x6C, &flag) && flag && mem::Read32(table + 0x68, &bc) && bc && bc <= 0x10000 &&
            mem::ReadPtr(table + 0x78, &buckets) && mem::ReadPtr(table + 0x80, &rows))
            for (uint32_t bi = 0; bi < bc && n < max; ++bi)
            {
                const uintptr_t b = buckets + static_cast<uintptr_t>(bi) * 0x100;
                uint32_t cnt = 0;
                if (!mem::Read32(b, &cnt)) continue;
                for (uint32_t i = 0; i < cnt && i < 31 && n < max; ++i)
                {
                    uint32_t k = 0, idx = 0;
                    if (!mem::Read32(b + 8 + 8ull * i, &k) || !mem::Read32(b + 0xC + 8ull * i, &idx)) continue;
                    uintptr_t row = 0; uint16_t rk = 0, rv = 0;
                    if (!mem::ReadPtr(rows + 8ull * idx, &row) || !mem::Read16(row + 4, &rk) || !mem::Read16(row + 6, &rv)) continue;
                    if (rk == static_cast<uint16_t>(k) && rv == value) out[n++] = rk;
                }
            }
        if (n) return n;
        uint32_t count = 0; uintptr_t defs = 0, def = 0; uint16_t k = 0;
        if (!max || !table || !mem::Read32(table + kOff_Table_Count, &count) || value >= count) return 0;
        if (!mem::ReadPtr(table + kOff_Table_DefsA, &defs) || !mem::ReadPtr(defs + 8ull * value, &def) || !def) return 0;
        if (!mem::Read16(def + kOff_Def_Key16, &k)) return 0;
        out[0] = k;
        return 1;
    }

    bool InventoryHas(uint32_t iid)
    {
        if (!iid) return false;
        for (int i = 0; i < g_invN; ++i) if (g_inv[i] == iid) return true;
        return false;
    }
    int InventoryCount() { return g_invN; }
    int InventoryCapacity() { return g_invCap; }

    bool BagSlots(int* used, int* cap)
    {
        if (!g_bagCap) return false;
        if (used) *used = g_bagUsed;
        if (cap)  *cap  = g_bagCap;
        return true;
    }

    // ------------------------------------------------- inventory shape ----
    // Where the bag's limit is kept is not known. It is not a slot count: all
    // 18 buckets are allocated 1,460 slots whatever they hold, and nothing in
    // the bucket that is the bag moves when its contents do. That leaves a
    // carried weight against a limit, which is how this studio's other games
    // work, so the search is now for a field that tracks what is carried and a
    // constant beside it.
    //
    // Rather than dump everything, each sample says what CHANGED since the one
    // before, which is a short line and exactly the question being asked. The
    // first sample writes the whole holder once so the constants are on record
    // too. Values are shown as an integer and as a float, since a weight is
    // likely to be one. Debug logging only.
    static uint32_t g_shapePrev[0x80];      // holder words, 0x200 bytes
    static uint32_t g_shapePrevB0[0x40];    // bucket 0 words, 0x100 bytes
    static bool     g_shapeHave = false;

    static int AppendVal(char* out, int w, int cap, unsigned off, uint32_t was, uint32_t now)
    {
        float fw = 0, fn = 0;
        memcpy(&fw, &was, 4); memcpy(&fn, &now, 4);
        auto plausible = [](float f) { const float a = f < 0 ? -f : f; return a > 0.001f && a < 1.0e7f; };
        if (plausible(fw) || plausible(fn))
            return w + snprintf(out + w, cap - w, " +%X=%u->%u(%.3f->%.3f)", off, was, now, fw, fn);
        return w + snprintf(out + w, cap - w, " +%X=%u->%u", off, was, now);
    }

    void DumpInventoryShape(uintptr_t me, bool bagFull)
    {
        // Samples are only worth taking when the count has moved: a limit is
        // the field that does not move with it.
        static int   s_dumps = 0, s_lastN = -1;
        static DWORD s_last = 0;
        const DWORD now = GetTickCount();
        if (s_dumps >= 24) return;
        if (s_dumps && g_invN == s_lastN) return;
        if (s_last && now - s_last < 1500) return;
        const uintptr_t holder = Holder(me);
        if (!holder) return;
        s_lastN = g_invN;
        s_last = now; ++s_dumps;

        uintptr_t barr = 0, bk0 = 0; uint32_t bn = 0;
        if (mem::ReadPtr(holder + kOff_Inv_Buckets, &barr) && mem::Read32(holder + kOff_Inv_BucketN, &bn) && bn)
            mem::ReadPtr(barr, &bk0);

        uint32_t cur[0x80] = {}, cur0[0x40] = {};
        for (unsigned i = 0; i < 0x80; ++i) mem::Read32(holder + i * 4, &cur[i]);
        if (bk0) for (unsigned i = 0; i < 0x40; ++i) mem::Read32(bk0 + i * 4, &cur0[i]);

        char line[1500];
        int w = snprintf(line, sizeof line, "[shape] %d held%s", g_invN, bagFull ? ", bag reads as full" : "");
        for (size_t i = 0; i < g_invEach.size() && w < 260; ++i)
            if (g_invEach[i].second) w += snprintf(line + w, sizeof line - w, " b%u=%d", static_cast<unsigned>(i), g_invEach[i].second);

        if (!g_shapeHave)
        {
            LOG("%s | first sample, whole holder follows", line);
            for (unsigned base = 0; base < 0x80; base += 32)
            {
                w = snprintf(line, sizeof line, "[shape]   holder +%03X:", base * 4);
                for (unsigned i = base; i < base + 32 && i < 0x80; ++i)
                    if (cur[i]) w += snprintf(line + w, sizeof line - w, " +%X=%u", i * 4, cur[i]);
                LOG("%s", line);
            }
            w = snprintf(line, sizeof line, "[shape]   bucket0:");
            for (unsigned i = 0; i < 0x40; ++i)
                if (cur0[i]) w += snprintf(line + w, sizeof line - w, " +%X=%u", i * 4, cur0[i]);
            LOG("%s", line);
        }
        else
        {
            w += snprintf(line + w, sizeof line - w, " | holder changed:");
            int nch = 0;
            for (unsigned i = 0; i < 0x80 && w < 1300; ++i)
                if (cur[i] != g_shapePrev[i]) { w = AppendVal(line, w, sizeof line, i * 4, g_shapePrev[i], cur[i]); ++nch; }
            if (!nch) w += snprintf(line + w, sizeof line - w, " nothing");
            w += snprintf(line + w, sizeof line - w, " | bucket0 changed:");
            nch = 0;
            for (unsigned i = 0; i < 0x40 && w < 1450; ++i)
                if (cur0[i] != g_shapePrevB0[i]) { w = AppendVal(line, w, sizeof line, i * 4, g_shapePrevB0[i], cur0[i]); ++nch; }
            if (!nch) w += snprintf(line + w, sizeof line - w, " nothing");
            LOG("%s", line);
        }
        memcpy(g_shapePrev, cur, sizeof cur);
        memcpy(g_shapePrevB0, cur0, sizeof cur0);
        g_shapeHave = true;
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
    static bool KeyAt(uintptr_t global, uint32_t row, unsigned defsOff, char* out, size_t n,
                      unsigned keyOff = kOff_Def_StringKey)
    {
        const uintptr_t def = DefFor(global, row, defsOff);
        return def && mem::ReadEngineString(def + keyOff, out, n) && strlen(out) >= 2;
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
        const int dbN = ItemDb::RowCount();
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
        return KeyAt(g_f.gimmickTableGlobal, typeId, g_defsOff, out, n, kOff_GimmickDef_StringKey);
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

    uintptr_t CharacterInfoTable(uint32_t* rowsOut)
    {
        static uintptr_t g_chr = 0;
        static uint32_t  g_chrRows = 0;
        static bool      g_looked = false;
        if (!g_looked)
        {
            g_looked = true;
            const TableRef* t = nullptr;
            const int n = EnumTables(&t);
            for (int i = 0; i < n; ++i)
            {
                if (_stricmp(t[i].name, "characterinfo") != 0) continue;
                if (t[i].count <= g_chrRows) continue;
                g_chr = t[i].global; g_chrRows = t[i].count;
            }
            if (g_chr) LOG("[table] characterinfo: %u rows, used to name creatures", g_chrRows);
            else       LOG_ERR("[table] no characterinfo table; creatures cannot be named");
        }
        if (rowsOut) *rowsOut = g_chrRows;
        return g_chr;
    }

    bool KeyInTable(uintptr_t global, uint32_t row, char* out, size_t n)
    {
        for (unsigned off : { kOff_Table_DefsA, kOff_Table_DefsB })
            if (KeyAt(global, row, off, out, n)) return true;
        return false;
    }

    int RowByKey(const char* table, const char* key)
    {
        const TableRef* t = nullptr;
        const int n = EnumTables(&t);
        for (int i = 0; i < n; ++i)
        {
            if (_stricmp(t[i].name, table) != 0) continue;
            char buf[96];
            for (uint32_t r = 0; r < t[i].count && r < 0x10000; ++r)
                if (KeyInTable(t[i].global, r, buf, sizeof buf) && strcmp(buf, key) == 0) return static_cast<int>(r);
        }
        return -1;
    }
}
