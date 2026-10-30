#pragma once
#include <cstdint>

// MinHook detours on the game functions the engine needs: a per-frame pump on
// the game thread (drains queued sends and arms), the event queue (learns the
// player's route id), the ownership oracle (captures its context so we can ask
// it ourselves) and the node-arming dispatcher (captures its mode argument).
namespace ml::loot::hooks
{
    // Fire one named gimmick event at a node, through the game's own transition
    // driver. The node's chart decides what the event does from its current
    // state; an event it has no transition for is simply ignored, which is why
    // this is safe to aim at anything. Game thread only. Returns false when the
    // driver is unavailable or the call faulted.
    // Records that the mod just drove a break, so the ore bonus applies to that
    // vein and not to one the player mined by hand.
    void MarkOurBreak(bool on);

    bool DriveGimmickEvent(uintptr_t gimmickComp, uint32_t eventId,
                           uint32_t instigatorEid, uintptr_t instigatorActor,
                           uint32_t targetEid, const float* pos3);

    bool Install();   // after game::ResolveAll; MinHook must be initialised
    void Remove();

    const char* PumpName();   // "movement tick", "scene sweep", "event queue" or "none"
    long  PumpTicks();
    bool  OwnerCaptured();
    long  OwnerCalls();
    int   ArmMode();          // mode byte seen on the game's own arming calls; 0 until observed
    bool  ArmObserved();
    long  ArmCalls();
    // Bracket the mod's own call into the game's arming routine. That call
    // returns through the same detour, and without this the mod copies its own
    // arguments back out and then arms with them, which is how combo 0 quietly
    // turned into combo 1.
    void SelfArmBegin();
    void SelfArmEnd();

    // The same for the event queue. Without it the spy reports the mod's own
    // sends as traffic it overheard from the game, which is how 39 of 41
    // "captured" break events turned out to be ours.
    void SelfSendBegin();
    void SelfSendEnd();
    bool SelfSending();

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

    // Work out the two opaque arguments of the ownership routine from the
    // player, so the mod never has to wait for the game to run its own check.
    // Call it as soon as there is a player: cheap once armed, and it reports
    // why it could not the first time each reason applies.
    // The entity id the game itself passes as the player when it runs its own
    // take-or-steal check. 0 until it has asked once. Authoritative: the engine
    // otherwise picks the first player-tagged actor it enumerates, and a party
    // has more than one.
    uint32_t PlayerEidFromGame();

    bool EnsureOwnerArmed(uintptr_t playerEnt);

    // Whether the switch that stops pets looting has anything to act on.
    // False means the condition's vtable slot was not found on this build
    // and the switch does nothing; the log says which step gave up.
    bool PetLootingHooked();
    // Looks Dead_Loot_Pet up in interactioninfo by name and keeps its row for
    // the body half of the pet switch. Call from the scan thread once the
    // tables are loaded; it does the walk once and is free after that.
    void ResolveInteractionRows();

    // Leaving refused body loot on the ground. DROP.md.
    //
    // The routine the discard request's parser hands every drop to, found as
    // the first call after GetInventoryHolder in that parser, and a detour on
    // the server's parse of the mod's own search, which runs the engine's
    // SearchParse either side of it. Both come from descriptors found by
    // class name.
    void InstallDrop(uintptr_t discardDesc, uintptr_t searchDesc);
    bool DropReady();
    // The per-thread context the drop routine reads out of the exe's TLS
    // block at +0x250. Only the game's server thread has one.
    uintptr_t ThreadContext();
    // One call into the drop routine. Server thread only. False when it
    // faulted; otherwise *err is the game's answer, 0 for success. The
    // transform is the position at +0 and a rotation quaternion at +0xC.
    bool CallDrop(uintptr_t holder, uint32_t* err, uintptr_t actor, uint16_t key,
                  uint16_t slot, int64_t amount, const float* transform);
}
