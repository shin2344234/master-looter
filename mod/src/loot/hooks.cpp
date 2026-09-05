#include "hooks.h"

#include <Windows.h>
#include <MinHook.h>

#include "engine.h"
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
    static void*     g_targets[4] = {};
    static int       g_targetN = 0;

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
        InterlockedIncrement(&g_ownCalls);
        if (!InterlockedCompareExchange(&g_ownCaptured, 0, 0) && ctx && tag)
        {
            // The second argument should be the player entity: check its id tag.
            uint32_t eid = 0;
            const bool playerArg = mem::Read32(reinterpret_cast<uintptr_t>(me) + 0x60, &eid) && (eid >> 24) == game::kTagPlayer;
            if (playerArg || g_ownCalls > 30)
            {
                g_ownCtx = ctx; g_ownTag = tag;
                InterlockedExchange(&g_ownCaptured, 1);
                LOG_OK("[owner] oracle context captured (%s)", playerArg ? "player argument confirmed" : "after 30 calls, unconfirmed");
            }
        }
        return oOwn(ctx, me, target, tag, a5, a6);
    }

    static uint64_t hkArm(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
    {
        InterlockedIncrement(&g_armCalls);
        InterlockedExchange(&g_armMode, static_cast<LONG>(a2 & 0xFF));
        return oArm(a1, a2, a3, a4, a5, a6, a7, a8);
    }

    static bool Hook(const char* what, uintptr_t target, void* detour, void** original)
    {
        if (!target) return false;
        void* t = reinterpret_cast<void*>(target);
        const MH_STATUS c = MH_CreateHook(t, detour, original);
        if (c != MH_OK) { LOG_ERR("[hook] %s: create failed (%s)", what, MH_StatusToString(c)); return false; }
        const MH_STATUS e = MH_EnableHook(t);
        if (e != MH_OK) { LOG_ERR("[hook] %s: enable failed (%s)", what, MH_StatusToString(e)); MH_RemoveHook(t); return false; }
        if (g_targetN < 4) g_targets[g_targetN++] = t;
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
        for (int i = 0; i < g_targetN; ++i) { MH_DisableHook(g_targets[i]); MH_RemoveHook(g_targets[i]); }
        g_targetN = 0;
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

    int WouldSteal(uintptr_t me, uintptr_t target)
    {
        if (!oOwn || !OwnerCaptured() || !me || !target) return -1;
        uint64_t r = 0;
        __try { r = oOwn(g_ownCtx, reinterpret_cast<void*>(me), reinterpret_cast<void*>(target), g_ownTag, 7, 0); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return (r & 0xFF) ? 1 : 0;
    }
}
