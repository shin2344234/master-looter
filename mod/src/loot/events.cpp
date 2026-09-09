#include "events.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game.h"
#include "hooks.h"
#include "mem.h"
#include "signatures.h"
#include "../core/log.h"
#include "../core/settings.h"

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

    // Every descriptor the engine answers for, kept from the walk the
    // resolver already does. Naming an unexpected event in the log is the
    // difference between a number and a lead.
    struct DescName { uint16_t id; uint16_t size; std::string cls; };
    static std::vector<DescName> g_descMap;
    static const DescName* DescById(uint16_t id)
    {
        for (const DescName& d : g_descMap) if (d.id == id) return &d;
        return nullptr;
    }

    static volatile LONG g_gameTid = 0;
    static bool g_selfTested = false, g_sendAllowed = false;
    static int  g_descFound = 0;
    static volatile LONG g_route = 0, g_routeKnown = 0, g_sendTid = 0, g_sent = 0, g_draining = 0;
    static Seen g_seen[32]; static volatile LONG g_seenN = 0;
    // One report per distinct kind of event, not per event. The old per-line
    // budget was exhausted by the scene-load flood before the player moved,
    // so anything raised during play was never seen.
    static uint32_t g_probeSeen[96] = {};
    static int      g_probeSeenN = 0;
    static bool ProbeFirstTime(uint16_t id, uint8_t b3)
    {
        const uint32_t kind = (static_cast<uint32_t>(id) << 8) | b3;
        for (int i = 0; i < g_probeSeenN; ++i) if (g_probeSeen[i] == kind) return false;
        if (g_probeSeenN >= 96) return false;
        g_probeSeen[g_probeSeenN++] = kind;
        return true;
    }

    struct PendAct { Action act; uint32_t eid, player, route; uint8_t mode; };
    struct PendArm { uintptr_t node, mode, arg3, ctx; };
    // ev 0 means the vein's pair of transitions, which is what DriveBreak
    // wants. Any other value is one transition driven by id, which is what a
    // well's sequence is made of.
    struct PendDrive { uintptr_t comp, actor; uint32_t player, target; float x, y, z; uint32_t ev; };
    static PendAct g_pendAct[64]; static int g_pendActN = 0;
    static PendArm g_pendArm[32]; static int g_pendArmN = 0;
    static PendDrive g_pendDrv[32]; static int g_pendDrvN = 0;
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
    // The one call into the game, on its own so the resolver can hold a
    // std::string: MSVC will not mix __try with anything that unwinds.
    static void* LookupDesc(uintptr_t fn, uint32_t id, uint32_t mask)
    {
        __try { return reinterpret_cast<FnDescLookup>(fn)(0, id, mask); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void ResolveDescriptors(uint32_t mask)
    {
        const game::Fns& f = game::F();
        for (auto& d : g_desc) d.ptr = 0;
        g_descMap.clear();
        // The whole range every time, rather than stopping at the third
        // match: the map is what lets an unrecognised event be named.
        for (uint32_t id = 1; id <= 0x1FFF; ++id)
        {
            void* d = LookupDesc(f.descLookup, id, mask);
            if (!d) continue;
            const char* n = mem::RttiName(reinterpret_cast<uintptr_t>(d));
            if (!n) continue;
            {
                uint16_t sz = 0;
                mem::Read16(reinterpret_cast<uintptr_t>(d) + kOff_Desc_Size, &sz);
                g_descMap.push_back({ static_cast<uint16_t>(id), sz, n });
            }
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
                break;
            }
        }
        LOG("[desc] %d event descriptors in this build.", static_cast<int>(g_descMap.size()));
        // The ones that could plausibly carry mining an uncracked vein, so
        // the names are in the log next to whatever the spy catches.
        static const char* kOfInterest[] = { "Interaction", "Gimmick", "Break", "Drop", "PickUp", "Gather", "Collect" };
        for (const DescName& d : g_descMap)
            for (const char* k : kOfInterest)
                if (strstr(d.cls.c_str(), k)) { LOG("[desc] 0x%04X size %-3u %s", d.id, d.size, d.cls.c_str()); break; }
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
            // Ours, so the spy does not report it back as something the game did.
            ml::loot::hooks::SelfSendBegin();
            reinterpret_cast<FnEnqueue>(f.enqueue)(reinterpret_cast<void*>(q), ev, desc, 0);
            ml::loot::hooks::SelfSendEnd();
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

    static bool ArmCall(uintptr_t fn, uintptr_t node, uintptr_t mode, void* a3, uintptr_t ctx)
    {
        __try { reinterpret_cast<FnArm>(fn)(reinterpret_cast<void*>(node), mode, a3, ctx); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool ArmNow(uintptr_t node, uintptr_t mode, uintptr_t arg3, uintptr_t ctx)
    {
        const game::Fns& f = game::F();
        if (!f.armFn || !mem::Readable(node, 0x400)) return false;
        static __declspec(align(16)) unsigned char scratch[256];
        memset(scratch, 0, sizeof scratch);
        void* a3 = arg3 && mem::Readable(arg3, 0x40) ? reinterpret_cast<void*>(arg3) : static_cast<void*>(scratch);
        // Tell the detour this one is ours, so it does not take our scratch
        // buffer for the game's third argument and hand it back next time.
        ml::loot::hooks::SelfArmBegin();
        const bool ok = ArmCall(f.armFn, node, mode, a3, ctx);
        ml::loot::hooks::SelfArmEnd();
        return ok;
    }

    // The two events a pickaxe fires, in order. Names rather than numbers: the
    // ids are hashes of these strings and the game computes them the same way.
    // A node whose chart has no transition for an event ignores it, so aiming
    // the pair at something that is not a vein is a no-op, not a mistake.
    static bool DriveNow(const PendDrive& d)
    {
        const float pos[3] = { d.x, d.y, d.z };
        // A named single transition is not a vein break. This is how a well is
        // wound, and the marker below opens a two second window in which the
        // ore bonus treats any vein payout as ours. Stamping it here kept that
        // window open across the eleven seconds of a well run, so a vein the
        // player swung at by hand during it collected the bonus, which is the
        // one outcome the marker exists to prevent.
        if (d.ev)
            return ml::loot::hooks::DriveGimmickEvent(d.comp, d.ev, d.player, d.actor, d.target, nullptr);

        // Stamp the time so the ore bonus knows this vein was broken by the
        // mod. The drop resolves after this returns, so a flag held only for
        // the call would never cover it.
        ml::loot::hooks::MarkOurBreak(true);
        ml::loot::hooks::DriveGimmickEvent(d.comp, game::NameId("onattackimpulsecomplete"),
                                           d.player, d.actor, d.target, nullptr);

        return ml::loot::hooks::DriveGimmickEvent(d.comp, game::NameId("onbreak"),
                                                  d.player, d.actor, d.target, pos);
    }

    bool DriveBreak(uintptr_t comp, uint32_t player, uintptr_t actor,
                    uint32_t target, float x, float y, float z)
    {
        if (!comp) return false;
        const PendDrive d{ comp, actor, player, target, x, y, z, 0 };
        if (OnGameThread()) return DriveNow(d);
        Lock();
        const bool room = g_pendDrvN < 32;
        if (room) g_pendDrv[g_pendDrvN++] = d;
        Unlock();
        return room;
    }

    bool DriveEvent(uintptr_t comp, uint32_t eventId, uint32_t player,
                    uintptr_t actor, uint32_t target)
    {
        if (!comp || !eventId) return false;
        const PendDrive d{ comp, actor, player, target, 0, 0, 0, eventId };
        if (OnGameThread()) return DriveNow(d);
        Lock();
        const bool room = g_pendDrvN < 32;
        if (room) g_pendDrv[g_pendDrvN++] = d;
        Unlock();
        return room;
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

    // Everything queued was aimed at a world that has since been replaced.
    //
    // An arm and a drive both carry a raw component pointer, and after a
    // teleport the object behind it is freed. Checking the pointer does not
    // help: mem::Readable only proves the page is still mapped, and a freed
    // gimmick sits on a heap that very much is, so the check passes and the
    // game reads a null out of the wreckage. lsimo's log has it happening
    // inside the driver at +0x891EDC, four seconds after a 20 m jump, followed
    // by a call through a null function pointer that took the process. The
    // pending sends carry only entity ids, which are safer, but a recycled id
    // now names something else entirely, so those go too.
    int DropPending()
    {
        Lock();
        const int n = g_pendActN + g_pendArmN + g_pendDrvN;
        g_pendActN = g_pendArmN = g_pendDrvN = 0;
        Unlock();
        return n;
    }

    void Drain()
    {
        if (InterlockedCompareExchange(&g_draining, 1, 0) != 0) return;
        PendAct acts[64]; PendArm arms[32]; PendDrive drvs[32]; int an, rn, dn;
        Lock();
        rn = g_pendArmN; memcpy(arms, g_pendArm, sizeof(PendArm) * rn); g_pendArmN = 0;
        an = g_pendActN; memcpy(acts, g_pendAct, sizeof(PendAct) * an); g_pendActN = 0;
        dn = g_pendDrvN; memcpy(drvs, g_pendDrv, sizeof(PendDrive) * dn); g_pendDrvN = 0;
        Unlock();
        for (int i = 0; i < rn; ++i) ArmNow(arms[i].node, arms[i].mode, arms[i].arg3, arms[i].ctx);
        for (int i = 0; i < an; ++i) SendNow(acts[i].act, acts[i].eid, acts[i].player, acts[i].route, acts[i].mode);
        for (int i = 0; i < dn; ++i) DriveNow(drvs[i]);
        InterlockedExchange(&g_draining, 0);
    }

    // One line holding everything a reproduction needs: which descriptor, how
    // big the payload is, and the payload itself.
    static void LogPayload(const char* why, uint16_t id, uint16_t size, uintptr_t buf)
    {
        const DescName* d = DescById(id);
        char hex[3 * 32 + 1] = {}; int p = 0;
        const int n8 = size > 32 ? 32 : static_cast<int>(size);
        for (int i = 0; i < n8; ++i)
        {
            uint8_t b = 0;
            if (!mem::Read8(buf + i, &b)) break;
            p += snprintf(hex + p, sizeof hex - p, i ? " %02X" : "%02X", b);
        }
        uint32_t eid = 0; mem::Read32(buf + 4, &eid);
        LOG("[probe] %s: 0x%04X size %u %s | %s | raw u32 at +4 %08X",
            why, id, size, d ? d->cls.c_str() : "not in the descriptor map", hex, eid);
    }

    void SpyEnqueue(uintptr_t ev)
    {
        if (!ev) return;
        if (static_cast<DWORD>(InterlockedCompareExchange(&g_sendTid, 0, 0)) == GetCurrentThreadId()) return; // ours
        uint32_t who = 0, rt = 0;
        if (!mem::Read32(ev + kOff_Ev_Player, &who) || !mem::Read32(ev + kOff_Ev_Route, &rt)) return;
        // The three event kinds a vein's break could travel on, named whoever
        // raised them. Not filtered by owner tag: a gimmick retiring itself is
        // tagged world, not player, which is why no such event had ever reached
        // a log. A budget per kind so a flood of drops cannot crowd out the one
        // removal event we are looking for.
        //
        // Only what the game raises reaches here. hkEnqueue in hooks.cpp skips
        // this whole function while t_selfSend is set, and BreakNow sets it
        // around its own enqueue, so a vein the mod breaks logs nothing. These
        // lines appear when the player swings a pickaxe, not when auto-loot
        // works, which is what makes a manual-mining capture the useful one.
        if (Settings::Get().debugLog)
        {
            uintptr_t payload = 0; uint16_t payloadSize = 0, eventId = 0;
            if (mem::ReadPtr(ev + kOff_Ev_Buffer, &payload) &&
                mem::Read16(ev + kOff_Ev_Size, &payloadSize) && payloadSize >= 3 &&
                mem::Read16(payload, &eventId))
            {
                const DescName* named = DescById(eventId);
                if (named)
                {
                    static const char* kinds[] = {
                        "TrocTrDropItemOnGimmickBreakOnceTimer",
                        "TrocTrGimmickLogoutSelfByBreakReq",
                        "TrocTrGimmickBranchStateReq",
                        // Not a Gimmick* name, so the first sweep of the binary
                        // missed it: a vein is a scene object as well as a
                        // gimmick, and this is the other transition its break
                        // could travel on. 0x0B60 in build 2.01.00.
                        "TrocTrBreakSceneObjectReq"
                    };
                    static volatile LONG counts[4] = {};
                    for (int kind = 0; kind < 4; ++kind)
                    {
                        if (named->cls.find(kinds[kind]) == std::string::npos) continue;
                        const LONG ordinal = InterlockedIncrement(&counts[kind]);
                        if (ordinal <= 64)
                        {
                            LOG("[ore-review] kind %d sample %ld tick %lu who %08X route %08X thread %lu",
                                kind, ordinal, GetTickCount(), who, rt, GetCurrentThreadId());
                            LogPayload("native break-related event", eventId, payloadSize, payload);
                        }
                        break;
                    }
                }
            }
        }
        // Anything the game raised about itself rather than about the player
        // has already been named above if it is one of the break-related kinds.
        //
        // Everything past this line is about the player, and that has hidden
        // more than one answer. A gimmick acting on itself is tagged world, so
        // the ore vein's own removal never reached a log, and neither does
        // whatever a well raises when the bucket comes up full: four minutes of
        // watching produced nothing, which looked like proof no event existed.
        // It was proof of this filter.
        //
        // So name a world-raised descriptor before dropping it. Three sightings
        // each and forty distinct ids for the session, with the payload on the
        // first sighting only, which is enough to tell what the game did without
        // burying the log in scenery chatter.
        if (Settings::Get().debugLog && (who >> 24) != game::kTagPlayer)
        {
            uintptr_t payload = 0; uint16_t payloadSize = 0, eventId = 0;
            if (mem::ReadPtr(ev + kOff_Ev_Buffer, &payload) &&
                mem::Read16(ev + kOff_Ev_Size, &payloadSize) && payloadSize >= 3 &&
                mem::Read16(payload, &eventId))
            {
                struct Slot { volatile LONG id; volatile LONG seen; };
                static Slot slots[40] = {};
                const LONG want = static_cast<LONG>(eventId) + 1;   // 0 means the slot is free
                for (int i = 0; i < 40; ++i)
                {
                    const LONG had = InterlockedCompareExchange(&slots[i].id, want, 0);
                    if (had != 0 && had != want) continue;
                    const LONG n = InterlockedIncrement(&slots[i].seen);
                    if (n <= 3)
                    {
                        const DescName* nm = DescById(eventId);
                        LOG("[world] event 0x%04X size %u from %08X sample %ld (%s)", eventId, payloadSize,
                            who, n, nm ? nm->cls.c_str() : "unnamed");
                        if (n == 1) LogPayload("world event", eventId, payloadSize, payload);
                    }
                    break;
                }
            }
        }
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
        if (id == g_desc[Row(Action::Take)].id && size >= 8)
        {
            known = mem::Read32(buf + 4, &target);
            act = (b3 == 0x05) ? Action::Gather : Action::Take;
            // 0x00 and 0x05 are the two the mod sends. Anything else on this
            // descriptor is a sub-action it has never seen, which is the shape
            // mining an uncracked vein would most likely take.
            if (b3 != 0x00 && b3 != 0x05 && Settings::Get().debugLog && ProbeFirstTime(id, b3))
                LogPayload("pick-up descriptor, unfamiliar sub-action", id, size, buf);
        }
        else if (id == g_desc[Row(Action::Catch)].id)          { known = mem::Read32(buf + 3, &target); act = Action::Catch; }
        else if (id == g_desc[Row(Action::Search)].id)         { known = mem::Read32(buf + 3, &target); act = Action::Search; }
        if (!known)
        {
            // Something the player did that the mod has no name for.
            if (Settings::Get().debugLog && ProbeFirstTime(id, b3))
                LogPayload("descriptor the mod does not use", id, size, buf);
            return;
        }
        if ((target >> 24) != game::kTagWorld) return;
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
