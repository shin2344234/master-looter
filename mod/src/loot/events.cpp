#include "events.h"

#include <Windows.h>
#include <cstring>

#include "game.h"
#include "mem.h"
#include "signatures.h"
#include "../core/log.h"

namespace ml::events
{
    using namespace ml::sig;

    typedef void  (__fastcall *FnTlsInit)();
    typedef void* (__fastcall *FnDescLookup)(uint64_t zero, uint32_t descId, uint32_t mask);
    typedef void* (__fastcall *FnAllocEvent)(uint64_t zero, uint32_t sizeCode);
    typedef void  (__fastcall *FnEnqueue)(void* queueObj, void* ev, void* desc, uint64_t zero);
    typedef void  (__fastcall *FnArm)(void* gimmick, uintptr_t mode, void* scratch, uintptr_t eid);

    const char* ActionName(Action a)
    {
        switch (a)
        {
        case Action::Search: return "search corpse";
        case Action::Catch:  return "catch";
        case Action::Gather: return "gather";
        default:             return "pick up";
        }
    }

    // Descriptor ids are assigned by the engine and drift between builds; the
    // class names are source symbols and do not. Ids are re-derived at self test.
    struct Desc { uint16_t id; uint16_t size; uintptr_t ptr; const char* cls; };
    static Desc g_desc[3] = {
        { 0x07E8, 7,    0, kDesc_Search },
        { 0x0809, 0x0D, 0, kDesc_PickUp },
        { 0x0800, 8,    0, kDesc_Catch  },
    };
    static int Row(Action a) { return a == Action::Search ? 0 : a == Action::Catch ? 2 : 1; }

    static volatile LONG g_gameTid = 0;
    static bool g_selfTested = false, g_sendAllowed = false;
    static int  g_descFound = 0;
    static volatile LONG g_route = 0, g_routeKnown = 0, g_sendTid = 0, g_sent = 0, g_draining = 0;
    static Seen g_seen[32]; static volatile LONG g_seenN = 0;

    struct PendAct { Action act; uint32_t eid, player, route; uint8_t mode; };
    struct PendArm { uintptr_t node, mode, arg3, ctx; };
    static PendAct g_pendAct[64]; static int g_pendActN = 0;
    static PendArm g_pendArm[32]; static int g_pendArmN = 0;
    static CRITICAL_SECTION g_cs;
    static LONG g_csReady = 0;
    static void Lock()   { if (InterlockedCompareExchange(&g_csReady, 1, 0) == 0) { InitializeCriticalSection(&g_cs); InterlockedExchange(&g_csReady, 2); } while (InterlockedCompareExchange(&g_csReady, 2, 2) != 2) Sleep(0); EnterCriticalSection(&g_cs); }
    static void Unlock() { LeaveCriticalSection(&g_cs); }

    bool OnGameThread()
    {
        const LONG t = InterlockedCompareExchange(&g_gameTid, 0, 0);
        return t != 0 && static_cast<DWORD>(t) == GetCurrentThreadId();
    }
    bool SendAllowed() { return g_sendAllowed; }
    int  DescriptorsFound() { return g_descFound; }
    bool SelfTested() { return g_selfTested; }
    bool RouteKnown() { return InterlockedCompareExchange(&g_routeKnown, 0, 0) != 0; }
    uint32_t Route() { return static_cast<uint32_t>(InterlockedCompareExchange(&g_route, 0, 0)); }
    long SentCount() { return g_sent; }
    long QueuedCount() { return g_pendActN; }

    // Asks the game for each descriptor id and matches the class name.
    static void ResolveDescriptors(uint32_t mask)
    {
        const game::Fns& f = game::F();
        int left = 3;
        for (auto& d : g_desc) d.ptr = 0;
        for (uint32_t id = 1; id <= 0x1FFF && left > 0; ++id)
        {
            void* d = nullptr;
            __try { d = reinterpret_cast<FnDescLookup>(f.descLookup)(0, id, mask); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!d) continue;
            const char* n = mem::RttiName(reinterpret_cast<uintptr_t>(d));
            if (!n) continue;
            for (auto& row : g_desc)
            {
                if (row.ptr || !strstr(n, row.cls)) continue;
                uint16_t size = 0;
                mem::Read16(reinterpret_cast<uintptr_t>(d) + kOff_Desc_Size, &size);
                if (row.id != static_cast<uint16_t>(id) || (size && size != row.size))
                    LOG("[desc] %s moved: 0x%04X/%u -> 0x%04X/%u", row.cls, row.id, row.size, id, size);
                row.id = static_cast<uint16_t>(id);
                if (size) row.size = size;
                row.ptr = reinterpret_cast<uintptr_t>(d);
                --left;
                break;
            }
        }
        g_descFound = 0;
        for (const auto& row : g_desc)
        {
            if (row.ptr) { ++g_descFound; LOG("[desc] 0x%04X size %u %s", row.id, row.size, row.cls); }
            else LOG_ERR("[desc] %s not found", row.cls);
        }
    }

    static void SelfTest()
    {
        if (g_selfTested) return;
        g_selfTested = true;
        const game::Fns& f = game::F();
        __try
        {
            uint32_t mask = 0;
            if (!mem::Read32(f.descMask, &mask)) { LOG_ERR("[test] DESC_MASK unreadable"); return; }
            uintptr_t q = 0;
            mem::ReadPtr(f.queue, &q);
            LOG("[test] DESC_MASK 0x%08X, queue %s", mask, q ? "present" : "EMPTY");
            reinterpret_cast<FnTlsInit>(f.tlsInit)();
            ResolveDescriptors(mask);
            if (!q || g_descFound < 3) { LOG_ERR("[test] sending stays disabled (queue %s, descriptors %d/3)", q ? "ok" : "missing", g_descFound); return; }
            g_sendAllowed = true;
            LOG_OK("[test] event path verified; sending enabled");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERR("[test] exception 0x%08X during self test; sending stays disabled", GetExceptionCode());
        }
    }

    void OnGameThreadTick()
    {
        InterlockedExchange(&g_gameTid, static_cast<LONG>(GetCurrentThreadId()));
        if (!g_selfTested) SelfTest();
    }

    static void* Build(Action act, uint32_t target, uint32_t player, uint32_t route, uint8_t mode, void** descOut)
    {
        const game::Fns& f = game::F();
        const Desc& d = g_desc[Row(act)];
        reinterpret_cast<FnTlsInit>(f.tlsInit)();
        uint32_t mask = 0;
        if (!mem::Read32(f.descMask, &mask)) return nullptr;
        void* desc = reinterpret_cast<FnDescLookup>(f.descLookup)(0, d.id, mask);
        if (!desc) { LOG_ERR("[send] descriptor 0x%04X not found", d.id); return nullptr; }
        unsigned char* e = static_cast<unsigned char*>(reinterpret_cast<FnAllocEvent>(f.allocEvent)(0, d.size));
        if (!e) { LOG_ERR("[send] event allocation failed"); return nullptr; }

        *reinterpret_cast<uint32_t*>(e + kOff_Ev_One)    = 1;
        *reinterpret_cast<uint32_t*>(e + kOff_Ev_Zero40) = 0;
        *reinterpret_cast<uint64_t*>(e + kOff_Ev_Zero48) = 0;
        *reinterpret_cast<uint32_t*>(e + kOff_Ev_Player) = player;
        *reinterpret_cast<uint32_t*>(e + kOff_Ev_Player + 4) = 0;
        *reinterpret_cast<uint32_t*>(e + kOff_Ev_Route)  = route;
        *reinterpret_cast<void**>(e + kOff_Ev_Desc)      = desc;
        *reinterpret_cast<uint16_t*>(e + kOff_Ev_Size)   = d.size;
        *reinterpret_cast<uint8_t*>(e + kOff_Ev_Flag78)  = 1;

        unsigned char* buf = *reinterpret_cast<unsigned char**>(e + kOff_Ev_Buffer);
        if (!buf) { LOG_ERR("[send] event has no payload buffer"); return nullptr; }
        buf[0] = static_cast<uint8_t>(d.id & 0xFF); buf[1] = static_cast<uint8_t>(d.id >> 8); buf[2] = 0xFF;
        switch (act)
        {
        case Action::Search:                                   // id FF <eid>
            *reinterpret_cast<uint32_t*>(buf + 3) = target;
            break;
        case Action::Catch:                                    // id FF <eid> 03
            *reinterpret_cast<uint32_t*>(buf + 3) = target; buf[7] = 3;
            break;
        case Action::Gather:                                   // id FF 05 <eid> 00 00 01 FF 00
            buf[3] = 0x05;
            *reinterpret_cast<uint32_t*>(buf + 4) = target;
            buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x01; buf[11] = 0xFF; buf[12] = 0x00;
            break;
        default:                                               // id FF <mode> <eid> 01 01 00 FF 00
            buf[3] = mode;
            *reinterpret_cast<uint32_t*>(buf + 4) = target;
            buf[8] = 0x01; buf[9] = 0x01; buf[10] = 0x00; buf[11] = 0xFF; buf[12] = 0x00;
            break;
        }
        if (descOut) *descOut = desc;
        return e;
    }

    static bool SendNow(Action act, uint32_t target, uint32_t player, uint32_t route, uint8_t mode)
    {
        if (!g_sendAllowed) return false;
        const game::Fns& f = game::F();
        bool ok = false;
        __try
        {
            void* desc = nullptr;
            void* ev = Build(act, target, player, route, mode, &desc);
            if (!ev) return false;
            uintptr_t q = 0;
            if (!mem::ReadPtr(f.queue, &q)) { LOG_ERR("[send] queue global unreadable"); return false; }
            InterlockedExchange(&g_sendTid, static_cast<LONG>(GetCurrentThreadId()));
            reinterpret_cast<FnEnqueue>(f.enqueue)(reinterpret_cast<void*>(q), ev, desc, 0);
            InterlockedExchange(&g_sendTid, 0);
            InterlockedIncrement(&g_sent);
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            InterlockedExchange(&g_sendTid, 0);
            LOG_ERR("[send] exception 0x%08X; event dropped", GetExceptionCode());
        }
        return ok;
    }

    static bool ArmNow(uintptr_t node, uintptr_t mode, uintptr_t arg3, uintptr_t ctx)
    {
        const game::Fns& f = game::F();
        if (!f.armFn || !mem::Readable(node, 0x400)) return false;
        static __declspec(align(16)) unsigned char scratch[256];
        memset(scratch, 0, sizeof scratch);
        void* a3 = arg3 && mem::Readable(arg3, 0x40) ? reinterpret_cast<void*>(arg3) : static_cast<void*>(scratch);
        __try { reinterpret_cast<FnArm>(f.armFn)(reinterpret_cast<void*>(node), mode, a3, ctx); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Send(Action a, uint32_t target, uint32_t player, uint32_t route, uint8_t mode)
    {
        if (!g_sendAllowed) return false;
        if (OnGameThread()) return SendNow(a, target, player, route, mode);
        Lock();
        const bool room = g_pendActN < 64;
        if (room) g_pendAct[g_pendActN++] = { a, target, player, route, mode };
        Unlock();
        return room;
    }

    bool Arm(uintptr_t node, uintptr_t mode, uintptr_t arg3, uintptr_t ctx)
    {
        if (!game::F().armFn) return false;
        if (OnGameThread()) return ArmNow(node, mode, arg3, ctx);
        Lock();
        const bool room = g_pendArmN < 32;
        if (room) g_pendArm[g_pendArmN++] = { node, mode, arg3, ctx };
        Unlock();
        return room;
    }

    void Drain()
    {
        if (InterlockedCompareExchange(&g_draining, 1, 0) != 0) return;
        PendAct acts[64]; PendArm arms[32]; int an, rn;
        Lock();
        rn = g_pendArmN; memcpy(arms, g_pendArm, sizeof(PendArm) * rn); g_pendArmN = 0;
        an = g_pendActN; memcpy(acts, g_pendAct, sizeof(PendAct) * an); g_pendActN = 0;
        Unlock();
        for (int i = 0; i < rn; ++i) ArmNow(arms[i].node, arms[i].mode, arms[i].arg3, arms[i].ctx);
        for (int i = 0; i < an; ++i) SendNow(acts[i].act, acts[i].eid, acts[i].player, acts[i].route, acts[i].mode);
        InterlockedExchange(&g_draining, 0);
    }

    void SpyEnqueue(uintptr_t ev)
    {
        if (!ev) return;
        if (static_cast<DWORD>(InterlockedCompareExchange(&g_sendTid, 0, 0)) == GetCurrentThreadId()) return; // ours
        uint32_t who = 0, rt = 0;
        if (!mem::Read32(ev + kOff_Ev_Player, &who) || !mem::Read32(ev + kOff_Ev_Route, &rt)) return;
        if ((who >> 24) != game::kTagPlayer) return;
        if (!RouteKnown() && rt)
        {
            InterlockedExchange(&g_route, static_cast<LONG>(rt));
            InterlockedExchange(&g_routeKnown, 1);
            LOG("[route] learned from the game: player %08X route %08X", who, rt);
        }
        // What did the player just do by hand? Same payload layout as ours.
        uintptr_t buf = 0; uint16_t size = 0;
        if (!mem::ReadPtr(ev + kOff_Ev_Buffer, &buf) || !mem::Read16(ev + kOff_Ev_Size, &size) || size < 7) return;
        uint16_t id = 0; uint8_t b3 = 0; uint32_t target = 0;
        if (!mem::Read16(buf, &id) || !mem::Read8(buf + 3, &b3)) return;
        Action act; bool known = false;
        if (id == g_desc[Row(Action::Take)].id && size >= 8)   { known = mem::Read32(buf + 4, &target); act = (b3 == 0x05) ? Action::Gather : Action::Take; }
        else if (id == g_desc[Row(Action::Catch)].id)          { known = mem::Read32(buf + 3, &target); act = Action::Catch; }
        else if (id == g_desc[Row(Action::Search)].id)         { known = mem::Read32(buf + 3, &target); act = Action::Search; }
        if (!known || (target >> 24) != game::kTagWorld) return;
        const LONG n = InterlockedCompareExchange(&g_seenN, 0, 0);
        if (n >= 32) return;
        g_seen[n] = { target, act, GetTickCount() };
        InterlockedExchange(&g_seenN, n + 1);
    }

    int DrainSeen(Seen* out, int max)
    {
        const LONG n = InterlockedExchange(&g_seenN, 0);
        const int k = n < max ? static_cast<int>(n) : max;
        for (int i = 0; i < k; ++i) out[i] = g_seen[i];
        return k;
    }
}
