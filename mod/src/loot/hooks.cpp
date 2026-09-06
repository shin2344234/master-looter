#include "hooks.h"

#include <Windows.h>

#include "engine.h"
#include "farhook.h"
#include "events.h"
#include "game.h"
#include "mem.h"
#include "signatures.h"
#include "../core/log.h"

namespace ml::loot::hooks
{
    // Unknown-arity targets are declared with eight integer arguments so any
    // stack arguments the game passes are copied through to the original.
    typedef uint64_t (*Fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    typedef void     (*FnEnqueue)(void* q, void* ev, void* desc, uint64_t z);
    typedef uint64_t (*FnOwn)(void* ctx, void* me, void* target, void* tag, uint64_t a5, uint64_t a6);

    static Fn8       oMove = nullptr,  oArea = nullptr, oArm = nullptr;
    static FnEnqueue oEnq  = nullptr;
    static FnOwn     oOwn  = nullptr;

    static const char* g_pump = "none";
    static volatile LONG g_pumpTicks = 0, g_lastPumpAt = 0;
    static volatile LONG g_ownCalls = 0, g_armCalls = 0, g_armMode = -1;
    static volatile LONG64 g_armCtx = 0, g_armA3 = 0;
    static ArmSeen g_armSeen[64]; static volatile LONG g_armSeenN = 0;
    static volatile LONG g_ownCaptured = 0;
    static void* g_ownCtx = nullptr;
    static void* g_ownTag = nullptr;
    static bool  g_enqueuePump = false;

    static void Pump()
    {
        InterlockedIncrement(&g_pumpTicks);
        InterlockedExchange(&g_lastPumpAt, static_cast<LONG>(GetTickCount()));
        events::OnGameThreadTick();
        loot::OnGameTick();
    }

    static uint64_t hkMove(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
    {
        const uint64_t r = oMove(a1, a2, a3, a4, a5, a6, a7, a8);
        Pump();
        return r;
    }

    static uint64_t hkArea(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
    {
        // Runs thousands of times a second; pump at most once every 8 ms.
        static volatile LONG s_last = 0;
        const LONG now = static_cast<LONG>(GetTickCount());
        if (now - InterlockedCompareExchange(&s_last, 0, 0) >= 8) { InterlockedExchange(&s_last, now); Pump(); }
        return oArea(a1, a2, a3, a4, a5, a6, a7, a8);
    }

    static void hkEnqueue(void* q, void* ev, void* desc, uint64_t z)
    {
        events::SpyEnqueue(reinterpret_cast<uintptr_t>(ev));
        oEnq(q, ev, desc, z);
        // Fallback pump: when no tick hook is installed, or the tick has gone
        // quiet (menus, mounts), drain from the game's own event traffic. Only
        // a thread that carries player events is treated as the game thread.
        const LONG now = static_cast<LONG>(GetTickCount());
        const bool quiet = now - InterlockedCompareExchange(&g_lastPumpAt, 0, 0) > 1500;
        if (!g_enqueuePump && !quiet) return;
        uint32_t who = 0;
        if (!mem::Read32(reinterpret_cast<uintptr_t>(ev) + 0x50, &who) || (who >> 24) != game::kTagPlayer) return;
        if (events::OnGameThread() || !events::SelfTested() || g_enqueuePump) Pump();
    }

    static DWORD g_startedAt = GetTickCount();

    static uint64_t hkOwn(void* ctx, void* me, void* target, void* tag, uint64_t a5, uint64_t a6)
    {
        const LONG n = InterlockedIncrement(&g_ownCalls);
        const uint64_t r = oOwn(ctx, me, target, tag, a5, a6);
        uint32_t eid = 0, teid = 0;
        const bool playerArg = mem::Read32(reinterpret_cast<uintptr_t>(me) + 0x60, &eid) && (eid >> 24) == game::kTagPlayer;
        mem::Read32(reinterpret_cast<uintptr_t>(target) + 0x60, &teid);
        // Why capture waits is worth knowing: a call with a null context or
        // tag does not count, and neither does one that is not about the
        // player until thirty have gone by.
        static int s_early = 0;
        if (!InterlockedCompareExchange(&g_ownCaptured, 0, 0) && s_early < 12)
        {
            ++s_early;
            LOG("[owner] call %ld before capture: ctx %s, tag %s, subject is %s", n,
                ctx ? "set" : "null", tag ? "set" : "null", playerArg ? "the player" : "someone else");
        }

        // Does the context the game passed match what the chain from the actor
        // gives? If it does, a later build can work the context out at load and
        // stop making everyone wait. Observation only: nothing here uses it.
        if (ctx && me)
        {
            constexpr unsigned kCtxHolder = 0x68, kCtxField = 0x120;
            const uintptr_t holder = mem::Deref(reinterpret_cast<uintptr_t>(me), kCtxHolder);
            const uintptr_t derived = holder ? mem::Deref(holder, kCtxField) : 0;
            static int s_cmp = 0;
            if (s_cmp < 6)
            {
                ++s_cmp;
                LOG("[owner] context chain check: game passed %p, actor+0x68+0x120 gives %p, %s",
                    ctx, reinterpret_cast<void*>(derived),
                    derived == reinterpret_cast<uintptr_t>(ctx) ? "MATCH" : "different");
            }
        }

        if (ctx && tag && (playerArg || n > 30))
        {
            // Follow the game's latest context: it is what the game itself is
            // using right now, so it cannot go stale on us.
            const bool changed = ctx != g_ownCtx || tag != g_ownTag;
            g_ownCtx = ctx; g_ownTag = tag;
            if (!InterlockedCompareExchange(&g_ownCaptured, 0, 0))
            {
                InterlockedExchange(&g_ownCaptured, 1);
                LOG_OK("[owner] ownership check armed after %ld call(s) and %lu ms (%s). Until now nothing could be looted.",
                       n, static_cast<unsigned long>(GetTickCount() - g_startedAt),
                       playerArg ? "player argument confirmed" : "after 30 calls, unconfirmed");
            }
            else if (changed) { static int s_chg = 0; if (s_chg < 10) { ++s_chg; LOG("[owner] oracle context changed (call %ld)", n); } }
        }
        // The game's own verdicts, for comparison with ours in the log.
        static int s_logged = 0;
        if (s_logged < 40) { ++s_logged; LOG("[owner] game asked: target %08X a5 %llu -> %s (%llu)", teid, static_cast<unsigned long long>(a5), (r & 0xFF) ? "STEAL" : "take", static_cast<unsigned long long>(r)); }
        return r;
    }

    static uint64_t hkArm(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
    {
        const LONG n = InterlockedIncrement(&g_armCalls);
        InterlockedExchange(&g_armMode, static_cast<LONG>(a2 & 0xFF));
        // a4 looks like a pointer in every call seen (0x1_5018AF80 style), most
        // likely the interacting actor or an interaction context; it decides
        // things like "does this actor carry a pickaxe". Keep the latest.
        if (mem::Plausible(static_cast<uintptr_t>(a4))) InterlockedExchange64(&g_armCtx, static_cast<LONG64>(a4));
        if (mem::Plausible(static_cast<uintptr_t>(a3))) InterlockedExchange64(&g_armA3, static_cast<LONG64>(a3));
        if (n <= 8)
        {
            const char* cls = mem::RttiShort(static_cast<uintptr_t>(a1));
            const char* c3  = mem::Plausible(static_cast<uintptr_t>(a3)) ? mem::RttiShort(static_cast<uintptr_t>(a3)) : nullptr;
            const char* c4  = mem::Plausible(static_cast<uintptr_t>(a4)) ? mem::RttiShort(static_cast<uintptr_t>(a4)) : nullptr;
            uintptr_t owner = 0; mem::ReadPtr(static_cast<uintptr_t>(a1) + 0x08, &owner);
            LOG("[arm] game armed %s mode %u a3 %llX (%s) a4 %llX (%s) owner %llX%s (call %ld)", cls ? cls : "?", static_cast<unsigned>(a2 & 0xFF),
                static_cast<unsigned long long>(a3), c3 ? c3 : "no class", static_cast<unsigned long long>(a4), c4 ? c4 : "not an object",
                static_cast<unsigned long long>(owner), owner == static_cast<uintptr_t>(a4) ? " = a4" : "", n);
        }
        {
            const LONG k = InterlockedCompareExchange(&g_armSeenN, 0, 0);
            if (k < 64) { g_armSeen[k] = { static_cast<uintptr_t>(a4), static_cast<uintptr_t>(a3), static_cast<int>(a2 & 0xFF), GetTickCount() }; InterlockedExchange(&g_armSeenN, k + 1); }
        }
        return oArm(a1, a2, a3, a4, a5, a6, a7, a8);
    }

    static bool Hook(const char* what, uintptr_t target, void* detour, void** original)
    {
        if (!target) return false;
        char why[96];
        if (!farhook::Install(what, target, detour, original, why, sizeof why))
        {
            LOG_ERR("[hook] %s: %s", what, why);
            return false;
        }
        LOG("[hook] %s hooked at +0x%llX", what, static_cast<unsigned long long>(mem::Rva(target)));
        return true;
    }

    bool Install()
    {
        const game::Fns& f = game::F();
        if (Hook("movement tick", f.moveUpdate, reinterpret_cast<void*>(&hkMove), reinterpret_cast<void**>(&oMove)))
            g_pump = "movement tick";
        else if (Hook("scene sweep", f.areaSweepHit, reinterpret_cast<void*>(&hkArea), reinterpret_cast<void**>(&oArea)))
            g_pump = "scene sweep";
        const bool enq = Hook("event queue", f.enqueue, reinterpret_cast<void*>(&hkEnqueue), reinterpret_cast<void**>(&oEnq));
        if (!oMove && !oArea)
        {
            if (!enq) { LOG_ERR("[hook] no game-thread pump available; looting disabled"); return false; }
            g_enqueuePump = true;
            g_pump = "event queue";
        }
        Hook("ownership oracle", f.ownCheck, reinterpret_cast<void*>(&hkOwn), reinterpret_cast<void**>(&oOwn));
        Hook("node arming", f.armFn, reinterpret_cast<void*>(&hkArm), reinterpret_cast<void**>(&oArm));
        return true;
    }

    void Remove()
    {
        farhook::RemoveAll();
        oMove = oArea = oArm = nullptr; oEnq = nullptr; oOwn = nullptr;
        g_pump = "none";
    }

    const char* PumpName() { return g_pump; }
    long PumpTicks() { return g_pumpTicks; }
    bool OwnerCaptured() { return InterlockedCompareExchange(&g_ownCaptured, 0, 0) != 0; }
    long OwnerCalls() { return g_ownCalls; }
    int  ArmMode() { const LONG m = InterlockedCompareExchange(&g_armMode, 0, 0); return m < 0 ? 0 : static_cast<int>(m); }
    bool ArmObserved() { return InterlockedCompareExchange(&g_armMode, 0, 0) >= 0; }
    uintptr_t ArmContext() { return static_cast<uintptr_t>(InterlockedCompareExchange64(&g_armCtx, 0, 0)); }
    uintptr_t ArmArg3()    { return static_cast<uintptr_t>(InterlockedCompareExchange64(&g_armA3, 0, 0)); }
    int DrainArmSeen(ArmSeen* out, int max)
    {
        const LONG n = InterlockedExchange(&g_armSeenN, 0);
        const int k = n < max ? static_cast<int>(n) : max;
        for (int i = 0; i < k; ++i) out[i] = g_armSeen[i];
        return k;
    }
    long ArmCalls() { return g_armCalls; }

    // The fourth argument of the ownership call, read out of the game's own
    // code rather than waited for. Every call site that builds the context the
    // way we do passes the same pointer, so any one of them will do.
    // The fourth argument of the ownership call, read out of the game's own
    // code rather than waited for. Every call site that builds the context the
    // way we do passes the same pointer, so any one of them will do. The
    // instruction pair the pattern matches is common, so each match is checked
    // for actually calling the routine we mean.
    struct TagHunt { uintptr_t ownFn; void* tag; };

    static bool TagVisit(uintptr_t hit, void* ctxv)
    {
        auto* h = static_cast<TagHunt*>(ctxv);
        const uintptr_t call = hit + ml::sig::kOff_OwnCallSite_Call;
        if (mem::RipAt(call, 5) != h->ownFn) return false;
        for (uintptr_t p = call; p + ml::sig::kMax_OwnCallSite_Back > call && p > mem::Game().base; --p)
        {
            if (!mem::MatchAt(p, "4C 8D 0D")) continue;
            const uintptr_t tag = mem::RipAt(p, 7);
            if (!mem::InImage(tag)) continue;
            h->tag = reinterpret_cast<void*>(tag);
            LOG("[owner] ownership tag found at +0x%llX, from the call site at +0x%llX",
                static_cast<unsigned long long>(mem::Rva(tag)), static_cast<unsigned long long>(mem::Rva(call)));
            return true;
        }
        return false;
    }

    static void* FindOwnTag(uintptr_t ownFn)
    {
        if (!ownFn) return nullptr;
        TagHunt h{ ownFn, nullptr };
        mem::FindIf(ml::sig::kSig_OwnCallSite, TagVisit, &h);
        if (!h.tag) LOG_ERR("[owner] could not find the ownership tag in the game's code; the mod will wait for the game to ask instead.");
        return h.tag;
    }

    static uint64_t CallOracle(uintptr_t me, uintptr_t target, bool* boom)
    {
        *boom = false;
        __try { return oOwn(g_ownCtx, reinterpret_cast<void*>(me), reinterpret_cast<void*>(target), g_ownTag, 7, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *boom = true; return 0; }
    }

    // The game only runs its ownership check when the player goes near
    // something it can be asked about, which on a fresh load took 87 seconds
    // here. Waiting for that meant the mod took nothing at all until then, and
    // told the player their wild flowers belonged to someone. Both halves of
    // the call can be worked out instead: the context is the same chain off the
    // player that the game itself walks, confirmed against the game's own
    // argument, and the tag is a fixed pointer read out of its code.
    static bool ArmFromPlayer(uintptr_t me)
    {
        if (g_ownCtx && g_ownTag) return true;
        if (!me) return false;
        static void* s_tag = nullptr;
        static bool  s_looked = false;
        if (!s_looked) { s_looked = true; s_tag = FindOwnTag(game::F().ownCheck); }
        if (!s_tag) return false;
        const uintptr_t holder = mem::Deref(me, ml::sig::kOff_Own_CtxHolder);
        const uintptr_t ctx = holder ? mem::Deref(holder, ml::sig::kOff_Own_CtxField) : 0;
        if (!ctx) return false;
        g_ownCtx = reinterpret_cast<void*>(ctx);
        g_ownTag = s_tag;
        InterlockedExchange(&g_ownCaptured, 1);
        LOG_OK("[owner] ownership check armed from the player, without waiting for the game to ask.");
        return true;
    }

    int WouldSteal(uintptr_t me, uintptr_t target)
    {
        if (!oOwn || !me || !target) return -1;
        if (!OwnerCaptured() && !ArmFromPlayer(me)) return -1;
        bool boom = false;
        const uint64_t r = CallOracle(me, target, &boom);
        if (boom) return -1;
        static int s_logged = 0;
        if (s_logged < 40)
        {
            ++s_logged;
            uint32_t teid = 0; mem::Read32(target + 0x60, &teid);
            LOG("[owner] we asked:   target %08X -> %s (%llu)", teid, (r & 0xFF) ? "STEAL" : "take", static_cast<unsigned long long>(r));
        }
        return (r & 0xFF) ? 1 : 0;
    }
}
