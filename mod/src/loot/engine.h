#pragma once
#include <Windows.h>
#include <cstdint>

// The loot engine: a worker thread that reads the scene, decides what to take
// using the item database rules, and queues the game's own loot events for the
// game-thread pump. The menu reads its status and snapshots.
namespace ml::loot
{
    // The game raised its drop-on-break event for this entity. Called from the
    // event spy on the game thread; the scan reads it to tell a vein that
    // answered the break from a node that ignored it, which is the whole of the
    // bismuth chunk problem. See the break review in engine.cpp.
    void NoteBreakDrop(uint32_t eid);

    struct Status
    {
        bool  started = false, resolved = false, hooked = false, actorManager = false, playerFound = false;
        bool  sendAllowed = false, ownerOracle = false, routeKnown = false, settling = false;
        int   descriptors = 0, itemTable = 0, inventoryItems = 0, candidates = 0, lootable = 0, learned = 0;
        int   listed = 0;           // objects the Nearby list would show, before its row cap
        int   bagUsed = 0, bagSlots = 0;   // the carried bag; 0 when unreadable
        bool  bagFull = false;             // no room, or sends stopped landing
        uint32_t playerEid = 0;
        long  scans = 0, sent = 0, faults = 0, pumpTicks = 0;
        float lastScanMs = 0;
        const char* pump = "none";
        char  note[96] = "";
        char  hold[96] = "";     // why actions are paused right now, empty when not
    };

    // How many rows the Nearby list carries. The table scrolls, and a field of
    // flowers puts far more than this within range.
    inline constexpr int kNearbyRows = 96;

    struct Nearby
    {
        uint32_t eid;
        float    dist;
        char     name[48];
        char     klass[24];
        char     verdict[48];
        // The item's tags, so the rule that would change this verdict can be
        // read off the row instead of guessed at. Longest in the database is
        // 113 characters, plus the space the loader pads each end with.
        char     tags[120];
        long long value;      // copper, -1 when the database does not say
        bool     loot;
    };

    struct Recent { char text[80]; DWORD when; };

    void Start();          // spawns the worker; safe to call once per process
    void Stop();
    void OnGameTick();     // called by the pump hook on the game thread
    // Called on the game's server thread either side of its parse of a body
    // search. Before: true when this search is one to watch, having read the
    // bag. After: what the search paid that the rules refuse goes back on the
    // ground. DROP.md.
    bool SearchParse(uintptr_t sender, uint32_t target, bool after);
    // The same either side of its parse of a pick-up request, for a container
    // this mod opened in the last ten seconds with DropRefusedChests on.
    // Every other pick-up returns false at once without a word. Issue #89.
    bool ChestParse(uintptr_t sender, uint32_t target, bool after);
    // The same either side of its parse of a discard, the request a drop by
    // hand raises. Before: reads the slot and tells the scan to expect the
    // item on the ground at `at`, the world position the payload names. After:
    // says in the log whether it left the bag. The scan leaves it there.
    bool HandDropParse(uintptr_t sender, uint16_t key, uint16_t slot, long long amount, const float* at, bool after);
    // Whether a pet is reaching for something the player dropped by hand.
    // 0 no, 1 the scan recognised the entity, 2 its instance id is one a
    // discard registered, which holds before the scan has seen the object.
    // Game thread. Fills the entity id and the item's name when it can.
    int  HandDroppedForPet(uintptr_t ent, uint32_t* eid, char* name, size_t n);

    Status GetStatus();
    int  CopyNearby(Nearby* out, int max);
    int  CopyRecent(Recent* out, int max);
    long SessionCount(int action);   // per events::Action, items taken this session

    // Ask the item rules about a bare entity, for the pet-looting condition
    // to consult before the pet reaches for anything. 1 refuse, 0 allow, -1
    // no identity to judge. Game thread, cheap: one component walk and one
    // table lookup, no scan state touched. Fills `name` when it can.
    int  JudgeEntityForPet(uintptr_t ent, char* name, size_t n);

    // What this entity is, in one phrase, for a log line written from a hook
    // that has nothing but a pointer. Game thread. Names the item when the
    // thing carries item data and falls back to the prefab.
    void DescribeEntity(uintptr_t ent, char* out, size_t n);

    // What the loot engine was doing, for the crash handler to print beside the
    // faulting address. Reads plain statics and takes no lock: it is called
    // from a vectored exception handler, on whatever thread has just died, and
    // a lock that thread already holds would turn a crash report into a hang.
    void DescribeEngineState(char* out, size_t n);

    void RequestBurst();             // loot everything allowed in range once
    void ForgetLearned();            // clear the learned node yields (file too)
    void SetAuto(bool on);           // same as Config.enabled, saved
    void SetLootOwned(bool on);      // same as Config.lootOwned, saved, and drops the cached verdicts
}
