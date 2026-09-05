#include "hooks.h"

#include <Windows.h>

#include "engine.h"
#include "farhook.h"
#include "events.h"
#include "game.h"
#include "mem.h"
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

    static uint64_t hkOwn(void* ctx, void* me, void* target, void* tag, uint64_t a5, uint64_t a6)
    {
        const LONG n = InterlockedIncrement(&g_ownCalls);
        const uint64_t r = oOwn(ctx, me, target, tag, a5, a6);
        uint32_t eid = 0, teid = 0;
        const bool playerArg = mem::Read32(reinterpret_cast<uintptr_t>(me) + 0x60, &eid) && (eid >> 24) == game::kTagPlayer;
        mem::Read32(reinterpret_cast<uintptr_t>(target) + 0x60, &teid);
        if (ctx && tag && (playerArg || n > 30))
        {
            // Follow the game's latest context: it is what the game itself is
            // using right now, so it cannot go stale on us.
            const bool changed = ctx != g_ownCtx || tag != g_ownTag;
            g_ownCtx = ctx; g_ownTag = tag;
            if (!InterlockedCompareExchange(&g_ownCaptured, 0, 0))
            {
                InterlockedExchange(&g_ownCaptured, 1);
                LOG_OK("[owner] oracle context captured (%s)", playerArg ? "player argument confirmed" : "after 30 calls, unconfirmed");
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
        if (n <= 6)
        {
            const char* cls = mem::RttiShort(static_cast<uintptr_t>(a1));
            LOG("[arm] game armed %s mode %u eid %08X (call %ld)", cls ? cls : "?", static_cast<unsigned>(a2 & 0xFF), static_cast<uint32_t>(a4), n);
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
    long ArmCalls() { return g_armCalls; }

    static uint64_t CallOracle(uintptr_t me, uintptr_t target, bool* boom)
    {
        *boom = false;
        __try { return oOwn(g_ownCtx, reinterpret_cast<void*>(me), reinterpret_cast<void*>(target), g_ownTag, 7, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { *boom = true; return 0; }
    }

    int WouldSteal(uintptr_t me, uintptr_t target)
    {
        if (!oOwn || !OwnerCaptured() || !me || !target) return -1;
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
