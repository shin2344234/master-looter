#include "hooks.h"

#include <Windows.h>
#include <cstring>

#include "engine.h"
#include "farhook.h"
#include "events.h"
#include "game.h"
#include "mem.h"
#include "signatures.h"
#include "../core/log.h"
#include "../core/settings.h"

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
    // Set while the mod is inside the game's arming routine, so the detour can
    // tell our call from the game's. Thread local because arming runs on the
    // game thread while the scan runs on the worker.
    static thread_local int t_selfArm = 0;
    // The mod's own image, so a pointer that lives in it is never mistaken for
    // one of the game's objects.
    static uintptr_t g_selfLo = 0, g_selfHi = 0;
    static bool InSelf(uintptr_t a)
    {
        if (!g_selfLo)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(&g_selfLo), &mbi, sizeof mbi))
            {
                const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                g_selfLo = base;
                g_selfHi = base + nt->OptionalHeader.SizeOfImage;
            }
        }
        return g_selfLo && a >= g_selfLo && a < g_selfHi;
    }
    // A pointer worth remembering from one of the game's calls.
    static bool Foreign(uint64_t a)
    {
        const uintptr_t p = static_cast<uintptr_t>(a);
        return mem::Plausible(p) && !InSelf(p);
    }

    void SelfArmBegin() { ++t_selfArm; }
    void SelfArmEnd()   { if (t_selfArm) --t_selfArm; }

    static thread_local int t_selfSend = 0;
    void SelfSendBegin() { ++t_selfSend; }
    void SelfSendEnd()   { if (t_selfSend) --t_selfSend; }
    bool SelfSending()   { return t_selfSend != 0; }
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
        // Ours is not news. Only what the game raises is worth recording.
        if (!t_selfSend) events::SpyEnqueue(reinterpret_cast<uintptr_t>(ev));
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

    // One line per distinct node-and-name pairing the game uses. The point is
    // to learn which trigger name an ore vein listens on, since the one the
    // mod reuses is only ever the last one it happened to overhear.
    static void NoteGameArm(uint64_t comp, uint64_t nameId, unsigned mode)
    {
        if (!Settings::Get().debugLog) return;
        static char s_seen[160][96];
        static int  s_n = 0;
        if (s_n >= 160) return;

        char path[192] = {};
        if (!game::NodePrefab(static_cast<uintptr_t>(comp), path, sizeof path)) return;
        // The basename is the part that identifies the node; the folder is
        // the same for everything in a family.
        const char* base = path;
        for (const char* p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;

        char key[96];
        _snprintf_s(key, sizeof key, _TRUNCATE, "%.60s|%llX|%u", base,
                    static_cast<unsigned long long>(nameId), mode);
        for (int i = 0; i < s_n; ++i) if (strcmp(s_seen[i], key) == 0) return;
        _snprintf_s(s_seen[s_n++], sizeof s_seen[0], _TRUNCATE, "%s", key);
        LOG("[armname] the game armed %-46s with name %llX mode %u", base,
            static_cast<unsigned long long>(nameId), mode);
    }

    static uint64_t hkArm(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
    {
        // Our own call, coming back through the detour. Pass it straight
        // through: copying our arguments out and arming with them next time
        // is exactly the bug this guard exists for.
        if (t_selfArm) return oArm(a1, a2, a3, a4, a5, a6, a7, a8);

        const LONG n = InterlockedIncrement(&g_armCalls);
        InterlockedExchange(&g_armMode, static_cast<LONG>(a2 & 0xFF));
        // a4 is not an argument: the routine never reads it. It is kept only
        // because the register still holds whatever the caller last had there,
        // which has been useful for identifying who called. a3 is the one that
        // matters, a pointer to the name id being armed.
        if (Foreign(a4)) InterlockedExchange64(&g_armCtx, static_cast<LONG64>(a4));
        if (Foreign(a3)) InterlockedExchange64(&g_armA3, static_cast<LONG64>(a3));
        NoteGameArm(a1, a3, static_cast<unsigned>(a2 & 0xFF));
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

    // Breaking a vein means driving the game's own state machine: the swing
    // landing, then the break. The transition is what spawns the ore, so the
    // node empties itself through the game's own path and then disappears, the
    // way it does under a pickaxe.
    //
    // Most of an event record is opaque, so rather than build one the mod copies
    // one. The game fires both events at a vein the instant a pickaxe lands, and
    // the driver hook below keeps those records whole; the drive path then
    // replays one with only the node's own identity patched in. A vein mined by
    // hand teaches the mod, and until one is, a mostly empty record still drives
    // the transition correctly.
    static constexpr unsigned kRecSize = 0xE8;
    struct LearnedEvent { uint32_t id; unsigned char rec[kRecSize]; };
    static LearnedEvent g_learned[2] = {};
    static int g_learnedN = 0;

    // Only the two events the mod ever replays. Keeping everything filled the
    // table with unrelated events long before the player reached a vein.
    static bool WorthLearning(uint32_t id)
    {
        static uint32_t want[2] = {};
        if (!want[0])
        {
            want[0] = game::NameId("onattackimpulsecomplete");
            want[1] = game::NameId("onbreak");
        }
        return id == want[0] || id == want[1];
    }

    static void LearnEvent(uint32_t id, uintptr_t ev)
    {
        if (!id || !ev || !WorthLearning(id)) return;
        for (int i = 0; i < g_learnedN; ++i) if (g_learned[i].id == id) return;
        if (g_learnedN >= 2 || !mem::Readable(ev, kRecSize)) return;
        LearnedEvent& e = g_learned[g_learnedN];
        if (!mem::ReadBytes(ev, e.rec, kRecSize)) return;
        e.id = id;
        ++g_learnedN;
    }

    static const LearnedEvent* Learned(uint32_t id)
    {
        for (int i = 0; i < g_learnedN; ++i) if (g_learned[i].id == id) return &g_learned[i];
        return nullptr;
    }

    // The gimmick transition driver. Hooked to keep a trampoline the mod can
    // call, and to catch the two records worth copying on the way past.
    typedef uint64_t (__fastcall *FnStateDriver)(uintptr_t comp, uintptr_t ev,
                                                 uintptr_t instigator, uintptr_t outChanged);
    static FnStateDriver oStateDriver = nullptr;

    // Every gimmick state change in the world comes through here, and until now
    // it recorded two ids and said nothing. That is the one place a well's
    // bucket coming up full could show, because drawing water raises no event
    // through the queue at all: a manual draw six metres away produced not one
    // line, player-tagged or world-tagged.
    //
    // The ids are Jenkins hashes of lowercase names, so they cannot be read
    // back. They can be guessed at: hash a list of plausible words once and
    // report a match when one turns up. Three sightings per id, sixty-four
    // distinct ids for the session, debug log only.
    static const char* GuessStateName(uint32_t id)
    {
        static const char* kWords[] = {
            "wait", "gimmickon", "gimmickoff", "break", "onbreak", "open", "close",
            "use", "used", "on", "off", "start", "end", "fill", "filled", "empty",
            "water", "draw", "up", "down", "reset", "idle", "action", "interact",
            "gather", "collect", "take", "pick", "loop", "play", "stop", "hold",
            "bucketup", "bucketdown", "begin", "finish", "enter", "leave",
        };
        static uint32_t s_hash[sizeof kWords / sizeof kWords[0]];
        static bool s_ready = false;
        if (!s_ready) { for (size_t i = 0; i < sizeof kWords / sizeof kWords[0]; ++i) s_hash[i] = game::NameId(kWords[i]); s_ready = true; }
        for (size_t i = 0; i < sizeof kWords / sizeof kWords[0]; ++i) if (s_hash[i] == id) return kWords[i];
        return nullptr;
    }

    static uint64_t __fastcall hkStateDriver(uintptr_t comp, uintptr_t ev,
                                             uintptr_t instigator, uintptr_t outChanged)
    {
        uint32_t evId = 0;
        if (ev && mem::Read32(ev, &evId)) LearnEvent(evId, ev);
        if (evId && Settings::Get().debugLog)
        {
            struct Slot { volatile LONG id; volatile LONG seen; };
            static Slot slots[64] = {};
            for (int i = 0; i < 64; ++i)
            {
                const LONG had = InterlockedCompareExchange(&slots[i].id, static_cast<LONG>(evId), 0);
                if (had != 0 && had != static_cast<LONG>(evId)) continue;
                const LONG n = InterlockedIncrement(&slots[i].seen);
                if (n <= 3)
                {
                    uint32_t target = 0; mem::Read32(ev + 0x20, &target);
                    const char* name = GuessStateName(evId);
                    LOG("[gstate] event %08X%s%s%s target %08X comp %llX sample %ld",
                        evId, name ? " (" : "", name ? name : "", name ? ")" : "",
                        target, static_cast<unsigned long long>(comp), n);
                }
                break;
            }
        }
        return oStateDriver(comp, ev, instigator, outChanged);
    }

    bool DriveGimmickEvent(uintptr_t comp, uint32_t eventId, uint32_t instigatorEid,
                           uintptr_t instigatorActor, uint32_t targetEid, const float* pos3)
    {
        if (!oStateDriver || !comp || !eventId) return false;
        if (!mem::Readable(comp, 0x400)) return false;

        // 0xE8 is the size the game's own allocator copies for one of these.
        __declspec(align(16)) unsigned char rec[kRecSize] = {};
        if (const LearnedEvent* l = Learned(eventId)) memcpy(rec, l->rec, kRecSize);

        *reinterpret_cast<uint32_t*>(rec + 0x00) = eventId;
        *reinterpret_cast<uint32_t*>(rec + 0x10) = instigatorEid;
        *reinterpret_cast<uint32_t*>(rec + 0x14) = instigatorEid;
        *reinterpret_cast<uint32_t*>(rec + 0x20) = targetEid;
        if (pos3)
        {
            *reinterpret_cast<float*>(rec + 0x30) = pos3[0];
            *reinterpret_cast<float*>(rec + 0x34) = pos3[1];
            *reinterpret_cast<float*>(rec + 0x38) = pos3[2];
        }

        // +0x08 is a 64-bit pointer to whatever the game was holding when the
        // record was captured, and that object may since have been freed. Null
        // is a case the game already handles, since its own spawn events send
        // one; a dangling pointer is a crash.
        uintptr_t ctx = 0;
        memcpy(&ctx, rec + 0x08, sizeof ctx);
        if (ctx && !mem::Readable(ctx, 0x08)) memset(rec + 0x08, 0, sizeof ctx);

        // The driver's third argument is the instigator actor, which the chart's
        // SetTargetActor uses to record who is working the node. Passing null
        // leaves it with nobody to set.
        const uintptr_t subject =
            (instigatorActor && mem::Readable(instigatorActor, 0x80)) ? instigatorActor : 0;

        bool changed = false;
        // Straight to the trampoline: going back through the detour would only
        // watch the mod talk to itself.
        __try
        {
            oStateDriver(comp, reinterpret_cast<uintptr_t>(rec), subject,
                         reinterpret_cast<uintptr_t>(&changed));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LOG_ERR("[drive] exception 0x%08X driving event %08X; disabling", GetExceptionCode(), eventId);
            oStateDriver = nullptr;   // one fault is enough, never try again this session
            return false;
        }
        return changed;
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
        Hook("gimmick driver", f.stateDriver, reinterpret_cast<void*>(&hkStateDriver), reinterpret_cast<void**>(&oStateDriver));
        return true;
    }

    void Remove()
    {
        farhook::RemoveAll();
        oMove = oArea = oArm = nullptr; oEnq = nullptr; oOwn = nullptr; oStateDriver = nullptr;
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
    // Say a thing once. These run every scan until arming takes, and the
    // reason it has not taken is worth exactly one line, not thousands.
    static void OnceErr(bool& said, const char* msg)
    {
        if (said) return;
        said = true;
        LOG_ERR("%s", msg);
    }

    static bool ArmFromPlayer(uintptr_t me)
    {
        if (g_ownCtx && g_ownTag) return true;
        static bool s_noFn = false, s_noTag = false, s_noCtx = false, s_noMe = false;
        if (!me) { OnceErr(s_noMe, "[owner] no player entity yet, so the ownership check cannot be armed from it."); return false; }

        // The tag lives in the image, so one search settles it, but only
        // once there is a resolved function to search around. Latching on a
        // call made before the signature scan finished would switch this off
        // for the whole session for no reason.
        static void* s_tag = nullptr;
        static bool  s_looked = false;
        const uintptr_t ownFn = game::F().ownCheck;
        if (!ownFn) { OnceErr(s_noFn, "[owner] the ownership routine is not resolved yet; arming from the player will retry."); return false; }
        if (!s_looked) { s_looked = true; s_tag = FindOwnTag(ownFn); }
        if (!s_tag) { OnceErr(s_noTag, "[owner] no ownership tag, so the mod has to wait for the game to ask."); return false; }

        const uintptr_t holder = mem::Deref(me, ml::sig::kOff_Own_CtxHolder);
        const uintptr_t ctx = holder ? mem::Deref(holder, ml::sig::kOff_Own_CtxField) : 0;
        if (!ctx) { OnceErr(s_noCtx, "[owner] the player carries no ownership context yet; arming will retry as the world finishes loading."); return false; }

        g_ownCtx = reinterpret_cast<void*>(ctx);
        g_ownTag = s_tag;
        InterlockedExchange(&g_ownCaptured, 1);
        LOG_OK("[owner] ownership check armed from the player, without waiting for the game to ask.");
        return true;
    }

    // Called from the scan the moment a player exists. Arming used to happen
    // only inside WouldSteal, which is the last test in Decide() and is
    // therefore never reached in a session where nothing else passes. Both
    // logs of 2026-09-06 show it never ran once.
    bool EnsureOwnerArmed(uintptr_t playerEnt)
    {
        if (OwnerCaptured()) return true;
        return ArmFromPlayer(playerEnt);
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
