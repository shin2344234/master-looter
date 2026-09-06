#pragma once
#include <Windows.h>
#include <cstdint>

// The loot engine: a worker thread that reads the scene, decides what to take
// using the item database rules, and queues the game's own loot events for the
// game-thread pump. The menu reads its status and snapshots.
namespace ml::loot
{
    struct Status
    {
        bool  started = false, resolved = false, hooked = false, actorManager = false, playerFound = false;
        bool  sendAllowed = false, ownerOracle = false, routeKnown = false, settling = false;
        int   descriptors = 0, itemTable = 0, inventoryItems = 0, candidates = 0, lootable = 0, learned = 0;
        uint32_t playerEid = 0;
        long  scans = 0, sent = 0, faults = 0, pumpTicks = 0;
        float lastScanMs = 0;
        const char* pump = "none";
        char  note[96] = "";
        char  hold[96] = "";     // why actions are paused right now, empty when not
    };

    struct Nearby
    {
        uint32_t eid;
        float    dist;
        char     name[48];
        char     klass[24];
        char     verdict[48];
        bool     loot;
    };

    struct Recent { char text[80]; DWORD when; };

    void Start();          // spawns the worker; safe to call once per process
    void Stop();
    void OnGameTick();     // called by the pump hook on the game thread

    Status GetStatus();
    int  CopyNearby(Nearby* out, int max);
    int  CopyRecent(Recent* out, int max);
    long SessionCount(int action);   // per events::Action, items taken this session

    void RequestBurst();             // loot everything allowed in range once
    void ForgetLearned();            // clear the learned node yields (file too)
    void SetAuto(bool on);           // same as Config.enabled, saved
}
