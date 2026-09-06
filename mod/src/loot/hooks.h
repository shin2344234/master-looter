#pragma once
#include <cstdint>

// MinHook detours on the game functions the engine needs: a per-frame pump on
// the game thread (drains queued sends and arms), the event queue (learns the
// player's route id), the ownership oracle (captures its context so we can ask
// it ourselves) and the node-arming dispatcher (captures its mode argument).
namespace ml::loot::hooks
{
    bool Install();   // after game::ResolveAll; MinHook must be initialised
    void Remove();

    const char* PumpName();   // "movement tick", "scene sweep", "event queue" or "none"
    long  PumpTicks();
    bool  OwnerCaptured();
    long  OwnerCalls();
    int   ArmMode();          // mode byte seen on the game's own arming calls; 0 until observed
    bool  ArmObserved();
    long  ArmCalls();
    // The 4th argument of the game's latest arming call: a pointer, not an id.
    // 0 until the game has been seen arming something.
    uintptr_t ArmContext();
    // The 3rd argument of the game's latest arming call (a heap object, not scratch).
    uintptr_t ArmArg3();
    // Every arming call the game makes: owner actor, mode, 3rd argument.
    struct ArmSeen { uintptr_t owner, a3; int mode; unsigned long at; };
    int DrainArmSeen(ArmSeen* out, int max);

    // 1 = taking it would be theft, 0 = free to take, -1 = cannot tell yet.
    int WouldSteal(uintptr_t playerEnt, uintptr_t targetEnt);
}
