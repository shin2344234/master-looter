# The well, end to end

Everything established about drawing water, 8 September 2026. Written because
three separate conclusions along the way were wrong and confidently argued, and
the wrong ones are as useful to record as the right one.

## What a well is

Seven entities. One parent and six children, all matching `/well/` in the prefab
path:

| prefab | role |
| --- | --- |
| `gimmick_well_0002` | the parent, cat2 00 |
| `gimmick_well_0001_parts01` | **the bucket**, the only part that fills |
| `gimmick_well_0001_parts02` | **the winch**, carries the interaction |
| `gimmick_well_0001_parts02_part` | hangs off the winch |
| `gimmick_well_0001_parts03/04/05` | scenery |

The bucket grows a gather block only when the player is near: none at 14.5 m,
one at 1.8 m. That gather block is how the mod knows there is something in it.
The Nearby tab shows it as `class gather node`; a `[why]` line never will,
because that line prints the distance an object was *first* seen at.

Declared states, from `gimmick_states.py` over the extracted `.binarygimmick`:

    Wait 0x866C7489 · GimmickOn 0x150B14D0 · Clear 0xE300ACFE
    Lock 0x6AFC553C · Deactive 0xA488AC26
    MinAngle 0x61964BBC (winch) · PreGimmickOn 0xC7D8C27C · PreWait 0xB9C3379D

A gimmick component carries the name id of its current state at **+0x270**.
Observed live: the winch reads `MinAngle` while a player turns the handle and
`Wait` otherwise.

## What does not work

**Gathering the bucket.** Water arrives, bucket vanishes permanently.

**Picking the bucket up**, with it wound full by hand first so only the contents
should be needed. Water arrives, bucket vanishes again.

Loot travels on one descriptor, `0x0809`, with a mode byte at payload+3: `0x05`
gathers, `0` picks up, and the spy reads the same byte to tell them apart. The
mode is not the variable. **The loot descriptor cannot take from a well without
taking the well**, and no third phrasing of that request will change it.

**Driving the whole winch sequence.** This works and pays out every time. It is
also unusable: a run is eleven seconds, and with any sane cooldown the well is
never free, so a player reaching for the handle has it taken out of their hands.
Nineteen back-to-back runs in two minutes during testing. Checking the winch is
idle before starting cannot help when a run is always already in progress.

## What works

Drive the bucket's own transition, `0x003ECC59`, and nothing else. The player
winds the handle; the mod lifts out what is in the bucket. The winch is read so
the log can say what the player was doing, and never driven, so nothing the mod
does can disturb a handle someone is holding.

Behind `DrawWells`, off by default.

## How it is done, exactly

The whole feature is one transition driven at one entity. Everything else is
finding the right entity and knowing when it has something in it.

**1. Find the bucket.** Walk the scan for a candidate whose prefab path contains
`/well/` and `parts01`. That is the only part of a well that fills.

**2. Wait for it to hold something.** The bucket grows a gather block when the
player is close and it has been wound. In Master Looter that is `Cand::gather`,
set in `Fill` from the gimmick component's gather data at `+0xE0`. No gather
block means an empty bucket and nothing to take.

**3. Get its gimmick component.**

    const uintptr_t comps = game::Comps(bucket.ent);
    const uintptr_t comp  = game::CompByClass(comps, kCls_Gimmick);

**4. Drive one transition at it.**

    events::DriveEvent(comp, 0x003ECC59, playerEid, playerActor, bucket.eid);

`DriveEvent` builds a state-machine event record and calls the game's own
gimmick state driver, the function resolved as `stateDriver`. It is the same
call 1.4.0 uses to break ore veins, with the event id passed as a number rather
than hashed from a name. `0x003ECC59` is the last transition of a hand draw and
the only one aimed at the bucket rather than the winch.

That is it. The water arrives and the bucket stays on the well.

**What not to do.** Do not drive the winch (`parts02`). It works and it makes
the feature unusable, for the reasons above. Do not send a loot event at the
bucket in either mode; both take the bucket with the water.

**Reading the player's grip**, if you need it: the gimmick component holds the
name id of its current state at `+0x270`. On the winch that is `0x61964BBC`
(`MinAngle`) while the handle is being turned and `0x866C7489` (`Wait`)
otherwise.

The implementation is `WellTick` in `mod/src/loot/engine.cpp`, and
`events::DriveEvent` in `mod/src/loot/events.cpp`.

## Wrong turns worth remembering

**"A well declares `Clear` where a vein declares `break`, so it is built to be
cycled rather than consumed."** A peony declares `Clear` too and gathering
consumes a peony. `Clear` is just the state a collection gimmick enters once
taken. Diff against `peony_01.binarygimmick` before reading anything into a
state name.

**`RemoteCatchPivot`, `GimmickOnPullOutDirection`, `PullOutDurationTime`** look
like a dedicated pull-out interaction. They are schema keys on every gimmick, a
peony included, mostly empty.

**"Both loot modes fail, therefore this is impossible."** Does not follow, and
it closed the issue twice. The loot descriptor was never the only way in.

**"Drawing water raises no event, so the mod cannot see it."** True but
incomplete. It raises no *loot* event. It raises a run of state transitions, and
those were invisible only because the state driver logged nothing.

## The full captured hand draw

Kept because it is expensive to recapture and it is what the one working
transition was found in. Fifteen transitions over eleven seconds; `parts02`
unless noted.

    t+0.000   92A049DE            t+5.293   EB0F048C
    t+0.009   A4078B62  (part)    t+6.828   EB0F048C
    t+1.470   A327105E            t+8.184   A327105E
    t+1.479   D08DADE4            t+8.238   A809EFDF
    t+3.013   D08DADE4            t+8.247   F00346BD  (part)
    t+4.553   D08DADE4            t+8.663   0F7569ED
    t+5.106   A327105E            t+8.673   F00346BD  (part)
                                  t+11.115  003ECC59  (bucket)

Eight of these resolve to no name: `name_events.py` hashes every identifier in
the exe and misses them, and hashing all twelve well definitions matched none
either. `DriveGimmickEvent` takes the number, so it does not matter.

The state logger caps at three samples per id, so the two repeating phases in
the middle may be short. That mattered for the full run and does not for the
one-step take.
