# Ore veins: yield and disappearance

Working record. Last updated 2026-09-07. Both symptoms are fixed, and they were one cause. The record below is kept as a
trail rather than as a status: it is how the answer was reached, including the
turns that were wrong. The fix shipped in 1.4.0 and is in the last section.

Every claim below is marked **fact** (read from a log, the source, or the disassembly)
or **inference**. Several earlier conclusions were wrong and are kept in "Retracted"
rather than deleted, because two of them were reached twice.

## The two symptoms

1. An auto-mined vein yields one ore. The same vein mined by hand with the same level 1
   pickaxe, in one swing, yields two. (**fact**, user observation, repeated and confirmed)
2. An auto-mined vein does not disappear. A hand-mined one does. (**fact**, user
   observation)

**Inference:** one cause. The vein never enters the broken state, so its drop component
pays what an untouched vein pays, and nothing removes it.

## What the drop event actually does

Descriptor `TrocTrDropItemOnGimmickBreakOnceTimer`, id 0x0821, size 27, callback at RVA
0x2B65B60. Disassembly in `../../codex/descriptor-disassembly.txt:335`.

    r8d = [payload+3]            look up target actor (the vein)
    r8d = [payload+7]            look up player actor
    r9  = player actor or 0
    [rsp+0x20] = [payload+0xb]   the u32 at +11
    [rsp+0x28] = payload+0xf     impulse vector
    rcx = [[targetActor+0x68]+0x30]
    call 0x1429e45d0             spawn routine

**Fact:** the routine is invoked on an object reached through the *vein*, not through the
payload. The payload contributes identity, one u32, and a direction.

**Inference:** the yield is a property of the vein's state when the event runs. No payload
change can alter it. This is the strongest reason to stop trying to fix this event-side.

The mod's event is the same shape. Five captures of the game's own event, all five
identical in structure to what `BreakNow()` builds, zeros at +11 in both:

    10:19:30  target B01001D2  21 08 FF D2 01 10 B0 01 00 10 A0 00 00 00 00 DB B5 45 3F 3A CD 13 3F 59 AA 87 BE
    10:19:47  target B01001D6  21 08 FF D6 01 10 B0 01 00 10 A0 00 00 00 00 0F 98 18 3F 3A CD 13 3F 44 D9 0E BF
    10:22:20  target B0100329  21 08 FF 29 03 10 B0 01 00 10 A0 00 00 00 00 83 0A F5 3E 3B CD 13 3F 93 59 29 BF
    10:22:31  target B010032B  21 08 FF 2B 03 10 B0 01 00 10 A0 00 00 00 00 DA CB 38 3F 3A CD 13 3F 28 5B C3 BE
    10:22:47  target B010032A  21 08 FF 2A 03 10 B0 01 00 10 A0 00 00 00 00 F8 8E 45 3F 3A CD 13 3F 3A 8C 88 BE

(Transcribed. The raw session was lost when the game relaunched and truncated the log.
Snapshot logs before analysing them.)

`[r8+0]`, passed as an argument to the spawn routine, is never written by the mod, so it
is whatever the shared allocator left there for both. Not a candidate. (**fact**)

## The vein-exclusive arm names

Run `extract_evidence.py` to regenerate. From session A, 158 `[armname]` lines:

    5D951A104B8 mode 0   110 uses   generic, nearly every prefab
    5D951A10518 mode 1    23 uses   generic, many prefabs
    5D951A10458 mode 0     2 uses   copperstone + ironstone ONLY
    146B99078   mode 0     2 uses   copperstone + ironstone ONLY

**Fact:** two name slots in that whole session were used on ore veins and on nothing else.
Both ore types show the same three-name pattern, `...A10518 mode 1`, `146B99078 mode 0`,
`...A10458 mode 0`.

**Fact:** `146B99078` is a static global, not a heap pointer. CrimsonDesert.exe has
RELOCS_STRIPPED and DYNAMIC_BASE off with ImageBase 0x140000000, so RVA +0x6B99078 lands
in `.srdata` beside DESC_MASK and the queue global. It dereferences to id 0, the shared
empty-name singleton. That is why it is byte-identical across sessions.

**Fact:** the other slots follow a per-session base (`5D951A1xxxx`, `2FD65A1xxxx`,
`3E0F5A1xxxx`, `4394DA1xxxx`), so the offsets `+0x10458`, `+0x104B8`, `+0x10518` are
stable but the absolute values and the numeric ids are not. Id 52235 in one session was
50875 in another the same morning. Nothing may hardcode these.

**Fact:** arming with a zeroed name is the routine's broad path: it writes state 0 to
every trigger the node owns (`signatures.h:67-71`). A vein's triggers are both at state 1.

## What the mod does instead

**Fact**, `engine.cpp`:

- `rec.mode = 1` is unconditional, despite the comment above it describing alternation.
  The mod has never issued a mode 0 arm.
- `armA3 = nids ? HoldId(ids[...]) : ((combo == 1) ? 0 : ArmArg3())`. The zeroed name is
  only reachable when the node has no trigger ids. A vein has two (50869, 67819), so the
  documented "broad path" combo never runs for a vein.
- `combo` and `armCtx` are dead: `kArmCombo` is `{1,0,1,0}` and the only other use of
  `combo` tests `== 2`. The arm routine takes three arguments; the fourth is ignored.
- Every `[loot] break` in every session logs `type 0`: the node was never filled.

## Evidence by session

| Session | Build | Breaks | Pickups | Break type | Nodes filled after arming |
|---|---|---|---|---|---|
| A `logs/session-A-v1.2.1-2026-09-06.log` | v1.2.1 | 42 | 41 | all type 0 | 3, all non-ore (52234, 52107) |
| B `logs/session-B-v1.3.0-0942.log` | v1.3.0 | 12 | 10 | all type 0 | 3, all non-ore (52052, 50748) |

**Fact:** no ore node has ever filled after arming, in any session. Copper veins B0100327
and B0100328 were each armed four times across full 9-second windows and never filled and
were never struck. Two nodes logged `nothing after 4 arms`.

**Fact:** yield per break is ~1 in both sessions (41/42 and 10/12), unchanged by any mod
edit made on 2026-09-07.

**Inference:** lsimo's five chunks per vein come from his x5 loot multiplier
(nexusmods.com/crimsondesert/mods/2275), not from a yield the mod is losing. Unconfirmed;
needs his bag totals.

## Ruled out

- **The payload.** Five captures, identical structure, zeros at +11 in both. The count is
  not in the event.
- **Two strikes per vein.** One swing gives two ore. (**fact**, user)
- **The generic arm id.** 52235 is not in a vein's trigger map (50869, 67819), and per
  `signatures.h` a lookup miss writes nothing and reports nothing.
- **Shrinking the storage-stack radius** to separate spill from container contents. Tried
  in f8ae0e3, 0.15 m to 0.02 m, did not help.
- **Exempting ore by item class** from the storage rule. `data/gimmick_drops.csv` shows
  chest tables containing CopperOre and IronStone, so the class cannot tell them apart.

## Retracted

- ~~"The vein goes inert after the break, so that part works."~~ `entity: does not respond`
  is the mod's own bookkeeping: the break inserts the vein into `g_searched`, the next scan
  skips Fill, and `Decide` at engine.cpp:1046 prints that reason because `c.item` and
  `c.gather` are unset. It appears one scan tick later and says nothing about the game.
  Reached wrongly twice.
- ~~"The pick does a two-step arm 9 ms apart."~~ `NoteGameArm` dedups on (basename, id,
  mode) with no entity pointer, so adjacent lines are first-sightings of distinct tuples
  and may be different nodes minutes apart. The timestamps are not an inter-call delay.
  The three-name pattern above is real; the timing was not.
- ~~"No world-tagged event fires after a break, which closes the removal question."~~ The
  probe that produced that silence only opened on the mod's own breaks and was filtered by
  `t_selfSend` in `hkEnqueue`. It could not have seen anything.
- ~~"The mod's own breaks appear in the spy as kind 0."~~ They do not. `hkEnqueue` skips
  `SpyEnqueue` entirely while `t_selfSend` is set, and `BreakNow` sets it.

## Instrumentation traps

Each of these produced a wrong conclusion at least once.

- `[armname]` dedups on prefab basename plus id plus mode, with no entity pointer. Two
  adjacent lines are not a sequence on one node.
- `[why] ... does not respond` is self-inflicted after a break, not a game reply.
- `hkEnqueue` filters the mod's own sends, so the spy is blind to anything the mod does.
- Arm ids and name pointers vary between sessions. Only the static `146B99078` and the
  per-session slot offsets are stable.
- Logs are truncated on every game launch. Snapshot before analysing.

## Fixed on 2026-09-07 (in the working tree, uncommitted)

- Storage-stack rule refused the ore a vein's own break spills. `NearOwnBreak` exempts a
  loose item at a site the mod itself broke, 3 m and 45 s. Unit-tested; not yet exercised
  in game, because a cluster needs 4+ items at one point and this player gets one.
- `Decide()` did not refuse puzzle prefabs while `OffLimits()` did, and five puzzle
  prefabs are `kind=ore, src=tag`, so the shipped 1.3.0 sends a break event at ice walls
  and pickaxe break points with default settings. `Decide()` now defers to `OffLimits()`.
- The retirement shortcut compared a raw entity id against a set keyed by instance id, so
  anything with an iid was re-Filled every scan forever.
- CJK font: the atlas was built with Latin ranges from a face with no CJK glyphs.
- Crops got their own switch; 37 nodes moved from Ground items to Crops.

## What a break actually changes on the vein (2026-09-07, session D)

Ran the slot-dump protocol: auto-loot on, Ore unticked, four iron veins mined by hand.
Regenerate with `diff_slots.py logs/session-D-manual-iron-oreoff.log <eid> <hh:mm:ss.mmm>`.

**Fact.** Control first: two dumps of the same vein *before* the break differ in zero
slots, on both veins. So every difference below is caused by the break, not drift.

Common to both B0100279 and B010027C:

    +140   (absent)           -> GameData_GimmickPointData@pa@@
    +148   (absent)           -> ptr
    +278   866C748900000000   -> 150B14D000000000
    +280   ptr                -> a float pair
    +288   2410800000000      -> 2440800000000
    +2B8   (absent)           -> ptr
    +350   3                  -> 5

**Fact:** `+0x68`, the pointer the drop callback dereferences, is NOT among the changed
slots. The object it points at is the same one; only its contents can have changed, and
the dump does not follow that pointer.

**Inference, and it inverts the mod's model:** the node acquires its
`GameData_GimmickPointData` at +140 *as a result of* the break. The mod waits for a vein
to fill so it can break it; the game fills it by breaking it. That is why no ore node has
ever filled after arming in any session, and why every `[loot] break` logs `type 0`. The
wait can never succeed.

**Inference:** `+0x350` going 3 -> 5 is the state field, 3 intact and 5 broken.

**Fact:** the four watched descriptors caught only kind 0, the drop, on all four manual
breaks. No `TrocTrBreakSceneObjectReq`, no `TrocTrGimmickLogoutSelfByBreakReq`, no
`TrocTrGimmickBranchStateReq`.

**Inference:** the transition is not carried by an event at all. It is the arm routine
writing trigger state directly, which is why nothing appears on the queue. That is
consistent with the vein-exclusive arm names, and it means the mod already has the
capability: it calls the same routine, with the wrong names.

Session D arm capture, same three-name shape as session A:

    10:50:49.959  gimmick_mine_ironstone_place_01  id 52234  mode 1  (name at 41435A10518)
    10:50:49.967  gimmick_mine_ironstone_place_01  id 0      mode 0  (name at 146B99078)

## Next experiment

The slot probe already in the mod dumps a gimmick's first 0x400 bytes when the node is
unfilled, has no item or gather data, and is within 4 m, refreshing every 3 s
(`engine.cpp:1009`). It covers +0x68.

Protocol: auto-loot on, **Ore unticked** so the scan runs but leaves veins alone. Stand
within 4 m of an untouched vein for ~5 s, mine it with one swing, stay put another ~5 s.
That gives a before and an after dump of the same entity. Diffing them shows which fields
the break transition sets, `[[+0x68]+0x30]` among them.

That is the first thing that would say what to change rather than what to rule out.

## Experiment queued (2026-09-07)

Setting **ArmLikePick**, default off, menu checkbox under the ore switches. When on, the
mod makes the two calls the game makes before a break: arm mode 1 with the name last seen
in use, then arm mode 0 with a zeroed name. Logs `[armpick]` per vein, 20 lines max.
Restricted to tagged ore veins, and everything OffLimits refuses is already refused before
this point.

Rationale: a hand-broken vein gains `GameData_GimmickPointData` and moves state 3 to 5;
the mod break changes neither; no watched descriptor carries the transition. The arm
routine is what is left.

Known weakness: the id at the vein-exclusive slot is still unknown, so this replays the
generic name rather than that one. The mode 0 zeroed-name call takes the broad path and is
the more likely operative half. If the experiment does nothing, the next step is the slot
id, not a bigger change.

Instrumentation fixed alongside it: `NoteGameArm` dedup is keyed on the name pointer again
as well as the id. Keying on the id alone (added earlier the same day) collapsed the
vein-only slot into the generic one and hid it from session D entirely.

## Experiment result: ArmLikePick does not work (session E, 2026-09-07)

`logs/session-E-armlikepick.log`. `ArmLikePick=1`, three iron veins broken.

**Fact:** three `[armpick]` lines, three breaks, three Iron Ore entities (B0100178,
B0100179, B010017A). Still one ore per vein. The arm sequence changed nothing.

**Fact, correcting a first reading:** the session shows zero pickups, but not because the
ore was missing. All three were refused with `pick up off`, so Ground items was unticked
at that moment. The ore spawned; the mod was told not to take it.

**Fact, and this is why the experiment was doomed:** the per-slot dedup fix worked and
revealed the vein-exclusive slot at last. A manual mine at 11:02:14 gives the full
sequence:

    11:02:14.878  gimmick_mine_ironstone_place_01  id 52234  mode 1  (name at 48CA3A10518)
    11:02:14.886  gimmick_mine_ironstone_place_01  id 0      mode 0  (name at 146B99078)
    11:02:14.887  gimmick_mine_ironstone_place_01  id 0      mode 0  (name at 48CA3A10458)

The vein-exclusive slot dereferences to **id 0**, the same value as the static global.
Both mode 0 calls are therefore the routine's broad path by the u32 test in
`signatures.h`, and are indistinguishable to it. There is no special vein name to arm
with. The lead that motivated this experiment is weaker than it looked: the slot is
vein-exclusive, its *value* is not.

**Inference:** arming is probably not the mechanism, which is where the 2026-09-07 design
review landed for different reasons. Replaying it more faithfully is unlikely to pay.

**Confirmed with Ground items back on** (`logs/session-F-armlikepick-groundon.log`, same
session continued): 8 breaks with `ArmLikePick=1`, 5 pickups. Four copper veins broken at
11:04:37 to 11:04:49 produced three Copper Ore. Still ~1 per vein, and the user reports
the veins neither broke visibly nor disappeared. `ArmLikePick` is inert across 8 breaks
and should be removed before any release; it is default-off and its only effect is the
broad-path arm.

**New observation, unexplored.** The tool is armed as part of the swing, moments before
the vein:

    11:02:10.091  gimmick_equip_mining_drill  id 52234  mode 1
    11:02:10.092  gimmick_equip_mining_drill  id 52343  mode 0
    11:02:12.620  gimmick_equip_mining_drill  id 52343  mode 1

The mod involves the tool nowhere. The drop callback reads `[[targetActor+0x68]+0x30]`,
and the yield bonus is a property of the equipped pickaxe, so how the tool enters that
calculation is the obvious next thread.

## The tool thread (2026-09-07)

**Fact:** the spawn routine the drop callback calls is at RVA 0x29E45D0, a large function
(0x788 frame). Disassemble with `disasm.py 29E45D0`. Its first act on arg3, which the
callback sets to `[event+0]`, is a chain `[arg3+0x68]` then `+0x20` then `+0x18`. The mod
never writes `[event+0]`; the shared allocator sets it.

**Fact:** the mining drill arms 2 to 4 seconds *before* the vein, not with it:

    11:02:10.091  gimmick_equip_mining_drill  id 52234  mode 1
    11:02:10.092  gimmick_equip_mining_drill  id 52343  mode 0
    11:02:12.620  gimmick_equip_mining_drill  id 52343  mode 1
    11:02:14.878  gimmick_mine_ironstone_place_01 ...

**Correction to an earlier note in this file:** that is not "the tool armed as part of the
swing". The gap and the ordering read as the player *equipping* the pickaxe before mining.

**Fact:** `gimmick_equip_mining_drill` is one of a set of equip gimmicks the character
carries at all times, alongside `gimmick_equip_rangeweapon_quiver`, `_shield`,
`_onehandsword`, `_lantern_01`, `_spear_equip`, `_giantsword`, `_openclose_helm`. Their
presence says what is carried, not what is in hand.

**Fact:** in every session the mod logs the Bekker Bow on the player hundreds of times
(329 in session B, 380 in session F) while it breaks veins.

**Hypothesis, and it fits every observation so far:** Mining Yield Up is applied from the
tool that is *in hand* when the drop resolves. Mining by hand requires the pickaxe to be
drawn, which is why it pays two. The mod breaks whenever it likes, usually with a bow out,
so the drop resolves with no mining tool active and pays base, which is one.

**REFUTED, session G** (`logs/session-G-pickaxe-out.log`). Pickaxe drawn at 11:11:09
(`gimmick_equip_mining_drill id 52344 mode 0`), 28 s before the first break. Five iron
veins broken, five Iron Ore: B010010F, B010010C, B0100110 at 11:11:37-38 and B01001AF,
B01001AE at 11:12:15. Exactly one per vein. Having the right tool in hand changes nothing,
so the tool is out and the search goes back to the state transition.

**Fact from the same session:** the two veins that got a slot dump were dumped at
11:12:14.699 and .700, both *before* the 11:12:15.455 break, both reading `+350=3` and
`+288=2410800000000`. Those match session D's pre-break values exactly. There is no
after-dump, because the mod retires a vein into `g_searched` the instant it breaks it and
the fill loop then skips it, which suppresses the probe from the next scan onward.

**Instrument added:** `g_brokeWatch` keeps a mod-broken vein readable for 10 s
(debug log only), so the probe can dump it after the break. This is the success criterion
for any future fix: a vein that reaches `+0x350 = 5` and gains `+0x140` has genuinely
broken, whatever route got it there. If the probe simply stops dumping after a break, that
also means success, since a filled node fails the probe's `!k.gather` test.

## Static search for the state writer (2026-09-07)

`find_state_writer.py` scans the exe for stores into `[reg+0x350]`. `disasm.py <rva>`
disassembles any address. Both use capstone from `../../codex/python-deps`.

**Fact:** zero instructions store an immediate 5 into `[reg+0x350]`. The value comes from a
register, so the constant cannot be grepped for.

**Fact:** 470 stores touch `[reg+0x350]` anywhere in the image. Excluding `rbp`/`rsp`
(stack frames, not object fields) leaves 132, of which 13 are byte-sized:

    0x0515495  mov byte ptr [rax + 0x350], cl
    0x05174F7  mov byte ptr [rcx + 0x350], dl
    0x0535F80  mov byte ptr [rbx + 0x350], al
    0x0CA6720  mov byte ptr [rdi + 0x350], dh
    0x0CAB37F  mov byte ptr [rdi + 0x350], 0
    0x37EAC7D  mov byte ptr [rdi + 0x350], bh
    0x3EA8ACD  mov byte ptr [rbx + 0x350], dh
    0x3EA8B0B  mov byte ptr [rbx + 0x350], dh
    0x3EA8B27  mov byte ptr [rcx + 0x350], 0
    0x3EA8B3F  mov byte ptr [rcx + 0x350], 0
    0x3EAD83F  mov byte ptr [rdi + 0x350], 1

**Unknown:** the probe prints `+350=3` from an 8-byte read, so the field could be a byte,
dword or qword, and nothing yet says which of the 132 (or which base object) is the vein's.
Static analysis narrows but does not identify. None of the 13 sit near the known break
RVAs (0x29E45D0, 0x2915150, 0x29126C0).

**Note on the binary:** code lives in a section called `.data2` (80.9 MB), and there is a
246 MB `.debug$P`. Section names are not the usual ones; treat tooling assumptions about
`.text` with care.

## Watchpoint: reviewed, and NOT built in-process (2026-09-07)

A 12-agent design and safety review concluded: do not put a hardware watchpoint in the
plugin. Use a debugger. Reasons, in order of weight.

**Fact, demonstrated not argued.** A reviewer built the proposed mechanism as a standalone
MSVC harness and triggered a defect on demand: a thread parked inside the vectored handler
during disarm keeps a **live debug register set on a game thread**. Debug registers are per
thread and survive the plugin unloading. That is a landmine in someone else's process.

**Fact:** `CrimsonDesert.exe` imports `sentry.dll` (30 functions), so the game ships crash
telemetry. A crash caused by a stray debug register is reported upstream from a modded
process. Verified with `dumpbin /dependents`.

**Fact:** the exe has RELOCS_STRIPPED, DYNAMIC_BASE clear and GUARD_CF clear, ImageBase
0x140000000, so module addresses are fixed and a debugger's breakpoints are stable across
runs. This is what makes the external route easy.

**Reputation:** `SetThreadContext` with `CONTEXT_DEBUG_REGISTERS` plus
`AddVectoredExceptionHandler` is a textbook anti-debug pattern. The mod already takes
ML-only VirusTotal hits every release and the README leans on a short import list. Being
default-off does not help, because the code would be in every shipped binary.

**What was done instead, one line.** The slot probe now prints the interaction component's
address: `[probe] gimmick eid B01001AF 1.7 m at 7FF6ABCD1234 slots: ...`. That is all an
external debugger needs.

**Recipe.** Attach x64dbg to CrimsonDesert.exe, take the address from a `[probe]` line for
a vein within 4 m, and set a one-byte write breakpoint:

    ba w1 <address>+350

Mine the vein by hand. The break lands on the instruction *after* the store, because a data
breakpoint is a trap rather than a fault, so the writer is the instruction immediately
before. Take its address, subtract ImageBase 0x140000000, and read it with
`disasm.py <rva>`.

**Correct technical notes worth keeping**, from the review, in case this is ever revisited:
DR7 for a 1-byte write in slot 0 is `0x00010001` (L0 bit 0, R/W0 = 01 at bits 16-17, LEN0 =
00 at bits 18-19). LEN encoding is not in numeric order: 00 = 1 byte, 01 = 2, 11 = 4,
10 = 8. A data breakpoint raises `EXCEPTION_SINGLE_STEP` (0x80000004), not
`EXCEPTION_BREAKPOINT`. DR6 is never cleared by the processor and must be cleared by hand,
only your own bit. A 1-byte watchpoint has no alignment requirement.

**Also corrected by the review:** `[[targetActor+0x68]+0x30]`, which the drop callback
reads, is not unexplored territory. Component slot 6 is `ClientGimmickActorComponent`, so
that chain resolves to the same object the slot probe already dumps for 0x400 bytes. The
`+0x350` and `+0x140` changes recorded above are changes *in that object*.

## THE WRITER, caught 2026-09-07 12:11

External debugger (`scratchpad/veinwatch.cpp`, a standalone tool, nothing in the plugin).
Hardware write breakpoint on an intact iron vein's `+0x350`, 91 threads armed, vein mined
by hand. **Two hits, same instruction:**

    HIT 1: thread 40400, store ends at RIP 1420475D4, rbx=25AC47EB000, rsi=150B14D0, byte now = 4
    HIT 2: thread 40400, store ends at RIP 1420475D4, rbx=25AC47EB000, rsi=353C1CAD, byte now = 5

`rbx` is exactly the gimmick component address the probe reported. The writing instruction
is at RVA 0x20475CE:

    +0x20475CE: inc dword ptr [rbx + 0x350]

**Fact: it is an `inc`, not a store of a constant.** That is why the static scan for a
write of 5 into `[reg+0x350]` found nothing, and it means `+0x350` is a CHANGE COUNTER,
not the state. A break bumps it twice: 3 -> 4 -> 5.

**Fact: the containing function is RVA 0x2047580 .. 0x20476AD (301 bytes)**, found via the
PE exception directory (`funcbounds.py`). Its shape is a state setter:

    SetState(rcx = gimmick component, edx = state name id)
      rbx = rcx, esi = edx
      ... call 0x14038D1D0 with a string-ish argument
      ebp = [rbx+0x270]            save the previous state
      [rbx+0x270] = esi            store the new state
      [rbx+0x280] = 0
      inc dword ptr [rbx+0x350]    bump the change counter
      r9 = [rbx+0x40]              then walk a table at +0x40 looking for esi

**Inference, and it is the first actionable one in this whole investigation:** the mod has
`rcx` already (`game::CompByClass(game::Comps(k.ent), kCls_Gimmick)`, engine.cpp:1604). If
the two state ids are stable, calling this function twice would put a vein into the broken
state properly, which is what every other approach failed to do.

**The two state ids observed:** `0x150B14D0` then `0x353C1CAD`. `0x150B14D0` also appears
in session D's before/after diff, where `+278` moved to `150B14D000000000` on a hand-broken
vein. Appearing in two sessions days apart suggests a stable name hash rather than a
per-session id, unlike the arm trigger ids. **Not yet confirmed across a restart.**

### The full function, and where the work happens

    SetState(rcx = gimmick component, edx = state name id)
      rbx = component, esi = newState
      call 0x38D1D0                  refcounted temp built from [rcx+8]
      ebp = [rbx+0x270]              save old state
      [rbx+0x270] = esi              store new state
      [rbx+0x280] = 0
      inc dword ptr [rbx+0x350]      bump the change counter
      r9 = [rbx+0x40]                the node's state table
        search [r9+0x90], count [r9+0x98], for esi
        if found: [rbx+0x27c] = esi  the last RECOGNISED state
      [rbx+0x28f] |= 1
      [rbx+0x3e0] = 0
      if [rbx+0x358] == -1 and ebp == [r9+8]: [rbx+0x358] = [rbx+0x350]
      ... release the temp ...
      rax = [rbx]; r8d = [rbx+0x270]; edx = ebp; rcx = rbx
      jmp qword ptr [rax+0x878]      virtual OnStateChanged(this, old, new)

**Fact:** it tail-calls a virtual at vtable slot +0x878. That handler is where the visible
break actually happens; this function only records state and notifies.

**Resolved: the `+0x270` vs `+0x278` discrepancy.** `[rbx+0x27c] = esi` runs only when the
id is in the node's own table. The probe prints 8-byte slots, so `0x27C` shows up in the
high half of the slot labelled `+278`. Session D's `+278 -> 150B14D000000000` was
`[0x27C] = 0x150B14D0`. Nothing is mislabelled and both numbers are real.

**Inference:** in session D the final recognised state stayed `0x150B14D0` even though a
break drives SetState twice, which fits `0x353C1CAD` not being in that node's table, so
`[0x27C]` kept the earlier value.

### The state ids cannot be hardcoded, but they can be read

**Fact:** `0x150B14D0`, `0x353C1CAD` and the intact `0x866C7489` appear **zero** times as
literal dwords anywhere in the image (`find_const.py const <hex>`). They are computed at
load, almost certainly hashes of state-name strings; Codex's archive dump lists state names
like `Clear`, `Deactive` and `SelfForceBreakImpulse`.

**Fact:** `0x150B14D0` appeared in session D (10:52) and again in session I (12:11), across
a game restart. Deterministic, but derived rather than stored.

**Fact:** a node carries its own valid state ids in memory: `[component+0x40]` then the
dword array at `+0x90` with count at `+0x98`. The mod can enumerate them the same way
`ReadTriggerIds` enumerates trigger names, so nothing needs hardcoding.

### The implementable path

**Fact:** `SetState` has 7 direct call sites (`find_const.py xref 2047580`), so a detour at
its entry sees every state change in the game.

**Proposed architecture, not yet built.** This is the same learn-then-replay pattern the mod
already uses for the drop event, and the same class of operation as the existing arm hook,
so it adds no new risk category:

 1. MinHook detour on RVA 0x2047580. The mod already installs five detours.
 2. Observe `(component, newStateId)`, resolve the component to its prefab the way
    `NoteGameArm` does, and log it.
 3. Learn from a real swing: "gimmick_mine_ironstone_place_01 breaking is SetState(a) then
    SetState(b)".
 4. Replay those two calls on a vein the mod wants to break, then let the existing drop
    event follow.

**Success test, already instrumented:** the vein reaches `+0x350` incremented twice and
gains `+0x140`, and the slot probe shows it. `g_brokeWatch` keeps a mod-broken vein readable
for 10 s specifically so this can be checked.

**Still open:** what the virtual at `+0x878` does, whether calling SetState out of band
skips a prerequisite one of the 7 callers performs, and whether the two ids are the same for
copper as for iron.

## SOLVED: the state ids are Jenkins lookup3 hashes of lowercase names

**Fact.** RVA 0x12D5630 is Jenkins lookup3 `hashlittle` with the seed baked in:
`a = b = c = length + 0xDEBA1DCD` (`lea ebx,[rdx-0x2145e233]` at 0x12D563F), the standard
mix schedule (rol 4/6/8/16/19/4) and final schedule (rol 14/11/25/16/4/14/24). Reproduced
in `statehash.py`, which regenerates every id below.

**Fact.** All three observed ids are hashes of lowercase state names:

    "wait"       -> 0x866C7489    the intact state
    "gimmickon"  -> 0x150B14D0    first transition of a break
    "break"      -> 0x353C1CAD    second transition of a break

So a vein break is `wait` -> `gimmickon` -> `break`.

**This removes the last blocker.** The ids are not literals in the image and vary by nothing
but the name, so the mod can compute them from strings instead of hardcoding numbers or
learning them at runtime. `statehash.py <name>` prints any of them.

## The state machine is per-prefab data, which decides the generic design

**Fact.** Gimmick behaviour is defined in `.binarygimmick` files in the game archives.
Codex extracted three (`mine_bismuth`, `mine_copper`, `mine_iron`); all three carry the
identical state machine, written as name pairs:

    InitialBranchState  = Wait
    GimmickOnEnterState = GimmickOn      GimmickOnExitState = Wait
    LockEnterState      = Lock           LockExitState      = Wait
    DeactiveEnterState  = Deactive       DeactiveExitState  = Wait
    ClearEnterState     = Clear

So a mine node's declared states are **Wait, GimmickOn, Lock, Deactive, Clear**. The names
are capitalised in the data and lowercased before hashing, which matches the ids.

**Fact, and it matters:** `break` is NOT among them. Yet the watched vein transitioned to
`break` (0x353C1CAD). That explains the session D observation that `[component+0x27c]`
stayed at `gimmickon` after the break rather than becoming `break`: SetState only writes
+0x27c when the id is present in the node's own table, so `gimmickon` is a declared state
for that node and `break` is not. An undeclared state still bumps the counter and still
fires OnStateChanged.

**Consequence for the design.** State names are per-prefab data, not a global enum, so the
mod must read a node's own table at `[component+0x40]` and match against computed hashes
rather than assume any name. A design that hardcodes "break" would be wrong even for mine
nodes, since they do not declare it.

### RETRACTED: the "one state machine" claim below was a sampling error

**The section that follows is wrong and is kept only because the mistake is instructive.**
It extracted 1,028 definitions using `*tree*`, `*collect*` and `*mine*` name filters and
concluded "1,028 of 1,028 identical, this settles the generality requirement". Archive 0008
holds **13,906** definitions. The sample was 7.4 per cent of the corpus, and the filter
selected for similarity: trees, collect nodes and mine nodes are all simple harvest props.

**Corrected, from all 13,906 (`gimmick_states.py` over a full extraction):**

    72 distinct state machines, not 3

    12041  Wait, GimmickOn, Lock, Clear, Deactive          the common case, 86.6%
      734  Wait, PreWait, PreGimmickOn, Lock, Clear, Deactive
      375  Wait, PreGimmickOn, Lock, Clear, Deactive
      153  + UnLock_Ing        93  PreGimmickOn_1      68  + Active_Ing
       50  + Wait_Enter        46  + PreWait           40  + both
       39  + MinAngle          37  Active_ing/Deactive_Ing   26  + MaxAngle   ...

    Wait       declared by 13906
    GimmickOn  declared by 12622, so 1284 definitions do NOT have it

**And a hole the sample hid entirely.** Matching the mod's 1,076 node rows against the
13,906 definitions by exact prefab name yields only 552 matches, of which 36 declare no
`GimmickOn` (32 of them stone, using `PreGimmickOn`). The other 524 do not match because the
mod's names are world *placements* (`gimmick_mine_ironstone_place_01`) while definitions are
*types* (`mine_iron.binarygimmick`). **That placement-to-definition mapping is unsolved**,
so for half the mod's own table we do not know which machine applies.

**Lesson for this file:** a filtered sample is not a survey. State the corpus size and the
filter next to any claim of universality.

### The original (wrong) section follows

### ANSWERED: every gimmick kind shares one state machine

Extracted 1,028 `.binarygimmick` definitions from archive 0008 with
`tools/crimson-desert-unpacker/python/paz_unpack.py` (537 tree, 567 collect, 43 mine;
`lz4` had to be installed for CPython 3.11, the bundled one in `codex/python-deps` is a
3.12 build). Parsed with `gimmick_states.py`.

**Fact: there are only three distinct state machines across all 1,028, and 1,018 of them
are identical.**

    1018 files   Wait, GimmickOn, Lock, Clear, Deactive        e.g. apple_smalltree_01
       8 files   Wait, PreWait, PreGimmickOn, Lock, Clear, Deactive
       2 files   Wait, PreGimmickOn, Lock, Clear, Deactive

    declared by all 1028:  Wait 0x866C7489, Lock 0x6AFC553C,
                           Clear 0xE300ACFE, Deactive 0xA488AC26
    declared by 1018:      GimmickOn 0x150B14D0
    rare:                  PreGimmickOn 0xC7D8C27C, PreWait 0xB9C3379D

**This settles the generality requirement.** Trees, collect nodes and mine nodes share the
same states, so one mechanism covers every kind the mod deals with. `Wait` and `GimmickOn`
are exactly the two ids the watchpoint caught, which confirms the extraction against live
capture.

**Fact: not one of the 1,028 declares a `Break` state** (zero files contain
`BreakEnterState` or `BreakExitState`). Yet the vein transitioned to `break`
(0x353C1CAD) as its second step.

**Inference:** `break` is an engine-level state, not data-driven, so the mod cannot find it
in a node's table and should not try. The data-driven half of the transition is
`Wait -> GimmickOn`, which every node declares.

**Design consequence, and it is a simplification:** the mod should drive the declared
transition and let the engine's own logic produce whatever follows. Driving `Wait ->
GimmickOn` is a thing every one of these 1,028 nodes understands; synthesising `break` is
not. That is also the safer half, since `GimmickOn` is what the node's own data says
happens when it is activated.

## Corrections to earlier entries in this file

- ~~"plus one in .debug$P (ignore that one)"~~ **Wrong, and it was mine.** `.debug$P` is
  vaddr 0x747B000, vsize 0xF60F9C9, characteristics **0xE0000020** (CODE | EXECUTE | READ |
  WRITE). It holds 246 MB of live, `.pdata`-registered code, and `OnStateChanged` itself
  lives in it. The seventh SetState call site is real.
- ~~`OnStateChanged` is "about 795 bytes"~~ It is 0xDF8E7F0..0xDF8EACF, **735 bytes**.

## What SetState is, and is not

**Fact:** `SetState` records state and notifies. It runs no action list. The exit/enter
action loops live in a different function (below).

**Fact:** `OnStateChanged` does not change the mesh, touch collision or the transform, and
does not remove the node. It also does **not** drop items: no path from it reaches the drop
spawn (0x29E45D0), the drop callback (0x2B65B60), the event allocator or the enqueue,
within depth 8 across 351 functions. So driving the state will not double the mod's yield,
which was the main risk.

**Fact:** `OnStateChanged` ends in one unconditional no-argument virtual on actor
component-array slot **+0x88**, called through `[rax+0x148]`, and that is the only site in
the whole image making that call. The mod has never seen that component: `kComps_SlotsEnd`
is 0x80 in `signatures.h:121`, so `CompByClass` stops before it. One
`mem::RttiName(mem::Deref(game::Comps(ent), 0x88))` would name it.

## The two candidate drivers, and which is wrong

**RVA 0x101E9F00** (709 bytes) makes five calls in a row on the same component and state id:
SetState, then 0x29CB320, 0x29CB990, 0x29CBD30, 0x29BCFF0. Each of those four has exactly
one direct caller in the entire image, and it is this function.

**But it is the wrong target.** It is reachable by exactly one path: the network descriptor
callback `TrocTrGimmickBranchStateReq` (RVA 0x29126C0). Session D watched four hand-broken
iron veins and that descriptor never fired. This is the server-driven path, not the pickaxe.

**RVA 0x891E90 is the better candidate.** It is **vtable slot +0x7D0 of
ClientGimmickActorComponent** itself, and it contains the real state-machine driver: an exit
loop at 0x89214B..0x8921CC over `[oldEntry+0x30]` with count `+0x38` and stride 12, and an
enter loop at 0x8922C5..0x892351 over `[newEntry+0x20]` with count `+0x28`. That is where a
state's actions actually run, and it is a virtual on the component the mod already holds.

## Design review of 0x891E90: all four dimensions refuted. Do not call it yet.

**The driver is not a state setter. It is `TransitionByEvent`.**

    TransitionByEvent(rcx = component, rdx = GimmickEvent*, r8 = instigator (nullable),
                      r9 = bool* outChanged)

You do not tell it a state. You fire a named EVENT, and the node's own data decides the
target state. The exit and enter arrays are read INSIDE the function from
`[component+0x40]` keyed by the old and new state, not supplied by the caller. My scouting
read of "rsi/r15 are caller-supplied begin/end" was wrong: both are reloaded from the state
definition at 0x89214B after the resolver runs. The new state id in r14d is an OUT parameter
of the resolver at 0xDF6E1F0, read back at 0x891F63.

That makes the mechanism *more* generic than hoped: the mod would send an event and let each
node's data choose, with no prefab table and no hardcoded state ids. It also means
"drive Wait to GimmickOn" is a request, not an instruction.

**Corrections to earlier claims in this file, from the review:**

- ~~"OnStateChanged does not drop items, so driving the state cannot double the yield"~~
  **Wrong, and dangerous.** The state's actions run in the DRIVER's own loops, through
  polymorphic calls at 0x895D95 and 0x895DF8, not in OnStateChanged. The depth-8 reachability
  walk that produced the reassurance was run on the wrong function. The duplication risk is
  real and unmeasured.
- The event record is **0xE8 (232) bytes**, not the ~0x6C one proposal assumed. A design
  shipping a 0x70-byte struct would read and write out of bounds in a plugin used by
  thousands.

**Layers, and which is which:**

    0x86E780   ENQUEUES ONLY. Branches on byte [component+0x45C]; one path hands to a
               manager, the other appends to a vector at +0x470 that is drained only when
               0x871120 next runs for that component. A mod call can return success and
               never be processed. Do not call.
    0x86EC60   the synchronous layer. Calls the driver at 0x86EE4E and carries four gates:
               channel (0x86ED7F), legality 0x142175160 (0x86EDE7, its only caller in the
               image), subsystem (0x86EDFF), and 0x142050770 (0x86EE34).
    0x891E90   the driver itself, reached from exactly one direct call site and no indirect
               dispatch through vtable slot +0x7D0 anywhere in the image.

**A save-data question that must be answered before anything ships.** SetState call site
0x204593A sits in 0x2045440, a serializer: it reads four bytes through a virtual at
`[stream+8]` into SetState, then serializes +0x274, +0x350..+0x358 in a loop at 0x2045970,
and twelve bytes at +0x3C4. The state round-trips through a stream. `README.md` line 34
promises the mod writes nothing to a save. Whether that stream is a save or replication is
open.

**Consensus next step, and it is cheap and safe:** do not call anything. Hook SetState
(0x2047580) with MinHook, as the mod already does five times, and log
`(component, newStateId, prefab, RETURN ADDRESS, [component+0x27C], [component+0x40])`.
The return address is what identifies which layer a real pickaxe swing goes through, and it
separates the driver path at 0x8921D4 from the serializer at 0x204593A. One hand-mined vein
answers it.

## ANSWERED by the SetState hook (session J, 2026-09-07 13:58)

`logs/session-J-setstate-entity.log`. A `kSig_SetState` pattern (unique, validated by
`mod/scripts/sigcheck.py`) plus a sixth MinHook detour that watches and drives nothing,
logging component, transition, call site, chart and last-declared state.

**Fact. A hand-mined vein transitions through the driver, and only the driver:**

    13:58:01.432  comp 5E9F4EA2000  wait      -> gimmickon  from +0x8921D9
    13:58:01.441  comp 5E9F4EA2000  gimmickon -> break      from +0x8921D9
    13:58:01.443  comp 5E9E831C600  wait      -> gimmickon  from +0x101E9FDC   (a DIFFERENT vein)
    13:58:01.447  comp 5E9E81C6C00  gimmick_item_basic_onehand 0 -> initial    (the ore spawning)

`+0x8921D9` is the return address of the SetState call at 0x8921D4, inside the transition
driver **0x891E90**. Both of the mined vein's transitions come from there. The design
target is confirmed.

**Correction to session I.** Without the component in the line, each transition appeared
twice, from `+0x8921D9` and `+0x101E9FDC`, and it looked like one node taking two paths.
With entities it is plainly two different nodes. `+0x101E9FDC` is a real gameplay path (44
hits) but it is not the pickaxe's, and nothing suggests it is part of a break sequence.

**This also refutes the design review's reason for discarding 0x101E9F00**, which was that
it is reachable only from a network callback that never fires. It fires often. It is simply
not the path a swing takes.

**Fact:** the serialiser at `+0x204593F` fired **once**, on a component with no chart,
`0 -> initial`, at load. Two sessions, one hit each, never during play. The `README.md`
line 34 promise about save data is not obviously threatened, though this is weak evidence.

**Fact:** a node's life begins `0 -> initial -> wait`. `initial` is `0x52029BCF`, the
Jenkins hash of `"initial"`, found by the same method as the rest.

**Call-site distribution over two sessions:** ~45 driver, ~44 at 0x101E9FDC, ~37 spawn,
1 serialiser, 1 at 0x2043FEB.

## SOLVED: the event ids that break a vein (session K, 2026-09-07 14:0x)

`logs/session-K-event-ids.log`. A second watch-only detour on the driver
(`kSig_StateDriver`, unique, sigcheck clean) reads the event id out of arg2 and the state
either side of the call, logging only when the state actually moved.

**Fact. A hand-mined iron vein:**

    event 128B06E9 fired initial(52029BCF) -> wait(866C7489)
    event 3DB4A808 fired wait(866C7489)    -> gimmickon(150B14D0)
    event FD8F7D2C fired gimmickon(150B14D0) -> break(353C1CAD)

**Fact. The event ids are the same Jenkins lookup3 hashes, of lowercase names.** Recovered
by hashing all 403,901 unique identifiers in the exe and matching:

    "onbranchendframe"         0x128B06E9   initial -> wait      (spawn settling)
    "onattackimpulsecomplete"  0x3DB4A808   wait -> gimmickon    (the swing lands)
    "onbreak"                  0xFD8F7D2C   gimmickon -> break

Verify any of them with `statehash.py <name>`.

**So a pickaxe swing is two events, not one.** The impulse completing moves the node to
`gimmickon`; a separate `onbreak` moves it to `break`. The chart does not chain them.

**Everything the mod would need is now computable from strings.** No hardcoded ids, no
runtime learning: hash `"onattackimpulsecomplete"` and `"onbreak"` and hand them to the
driver, which the node's own chart then resolves against its current state. A node that
does not declare a transition for an event simply does not move, which is the safe default
and the answer to the generality problem: the wrong event on the wrong node is a no-op
rather than a corruption.

### The event record is small, not 232 bytes

**Correction.** The 0xE8 figure came from `0x86E780`, the ENQUEUE path, which copies 232
bytes into a heap allocation. The record the SYNCHRONOUS path takes is different and much
smaller. `0x871120` builds one on its own stack at `rsp+0x60` and passes it straight to
`0x86EC60` (`lea rdx,[rsp+0x60]` / `mov rcx,r13` / `call 0x14086EC60` at 0x8711FF-0x871207).

**Fact, the only fields it writes:**

    +0x00  u32   event id, loaded from a cached global (mov eax,[rip+0x634911D])
    +0x04  u32   zero
    +0x08        never written
    +0x0C        never written
    +0x10  u32   entity handle, [rdi+0x60]
    +0x14  u32   the same handle again
    +0x18  ptr   result of 0x141387610 plus a global, the TLS-static idiom

**Fact:** there is no memset of that stack region anywhere in `0x871120` (the function
starts at 0x871120 and the first write to the record is at 0x8711AB), so `+0x08` and `+0x0C`
hold stack garbage. A consumer that read them would be reading noise, so it does not.

**Also worth having:** the game caches computed event-name hashes in globals rather than
hashing at each call site. The mod does not need that, since it can compute them, but it
means the ids are fixed for a build and a global address could be used as a cross-check.

**Still open before building a call:** confirm the above against a live dump of the record
the driver actually receives (the hook now logs all 0xE8 bytes so the tail can be seen to be
noise); pin what `+0x18` points at and whether a mod-supplied value is acceptable; and the
duplicate-loot question, which is unmeasured. The driver's own exit and enter action loops
run the state's actions, so if one of those drops items then driving the transition plus the
mod's existing drop event would double the yield. That is the bug this codebase already
shipped once.

## DECISIVE: the transition produces the drop (session L, 2026-09-07 14:10)

`logs/session-L-event-record.log`.

    14:10:01.255  gimmick_mine_ironstone_place_01  onattackimpulsecomplete  wait -> gimmickon
    14:10:01.264  gimmick_mine_ironstone_place_01  onbreak                  gimmickon -> break
    14:10:01.389  gimmick_item_basic_onehand       onbranchendframe         initial -> wait

**Fact:** the ore item entity comes into existence 125 ms after the break, with no other
input. The state transition is what drops the ore.

**Therefore the mod must send the transition INSTEAD of its drop event, never both.**
Driving the transition and also sending `TrocTrDropItemOnGimmickBreakOnceTimer` would pay
out twice. This is the duplicated-loot bug the codebase already carries a guard for, and it
is now a live risk rather than a hypothetical. It also means the fix is a replacement for
the current `breakIt` branch, not an addition to it.

**And it is the whole fix.** Driving the two events gets the visible break, the node
retiring itself, the tool's Mining Yield Up (the game runs its own drop path, which is what
1.3.0 was reaching for), and the drop, from the game rather than from the mod.

## The event record, from live captures

Records for player-driven events are HEAP allocated (0x403C2279E00, 0x403C237CC00, each
different), which is the 0xE8 path. Spawn-time events use the caller's stack (0x13FF890,
the same address every time), which is the small builder at 0x871120. Both reach the driver.

    onattackimpulsecomplete             onbreak
    +00  08 A8 B4 3D  event id          2C 7D 8F FD  event id
    +04  5B EC B3 64                    00 00 00 00
    +08  D0 DD FB 98                    80 EE E8 C2
    +0C  03 04 00 00  = 0x403           03 04 00 00  = 0x403
    +10  01 00 10 A0  player            01 00 10 A0  player
    +14  01 00 10 A0  player            01 00 10 A0  player
    +18  0E CE 00 00 00 00 00 00        16 CE 00 00 00 00 00 00
    +20  28 01 10 B0  the vein          28 01 10 B0  the vein
    +30  (zero)                         BD 63 1B C6  91 A7 16 44  85 81 92 C5
                                        = three floats, a world position

**Fact, from the spawn-time record (`onbranchendframe`):** `+0x04`, `+0x08` and `+0x0C` are
all zero and `+0x10`/`+0x14` hold the NODE's own entity rather than the player. So those
fields are not required to be meaningful; the small stack builder leaves `+0x08`/`+0x0C`
untouched entirely.

**Inference on the fields:**

    +0x00  event id, computable from the name
    +0x0C  0x403 for both vein events, 1 for a tool event, 0 at spawn: event-kind flags
    +0x10  instigator entity (the mod has this: g_meEid)
    +0x14  instigator again (0x86EC60 tests it for zero and then skips resolution)
    +0x18  a small counter, 0xCE0E then 0xCE16 eight ms apart: a frame or sequence number
    +0x20  TARGET entity (the mod has this: k.eid)
    +0x30  world position, only on onbreak (the mod has this: k.pos)

`+0x04` and `+0x08` vary per event and look like per-event payload rather than structure;
a third event (`0xB97BA6BC` on the mining drill) carries a float at `+0x08` and `1` at
`+0x0C`, which supports that reading.

**Still unknown:** whether `+0x04`, `+0x08` and `+0x18` must be plausible or may be zero.
The spawn record has all three zero or near it and still transitions, which is encouraging
but is a different event.

## CLOSED: the mod was never losing the ore (session AA, 17:08-17:13)

Fourteen consecutive **mod-driven** breaks kept both rows and paid full yield:

    17:11:30 .. 17:12:08   14 MOD-BREAKs (copper, rock, iron)
                           every walk: 9107 = FALSE, rows = 2
                           22 items collected in that window

The pickups show the pairs plainly: two Copper Ore together at 17:11:35.668, four more at
17:11:36.309, two Iron Ore at 17:11:48.556, and four Stone with four Fine Stone at
17:11:54.277. Rock nodes paid Stone AND Fine Stone from a single driven break.

Either side of that window, condition 9107 reads TRUE and every walk keeps one row, driven and
hand alike. The full session:

    17:08:32   hand swing        9107 FALSE   2 rows
    17:09:44   driven            9107 TRUE    1 row     (flipped with NO mod activity in the
    ..17:10:13 driven            9107 TRUE    1 row      72 s since the swing)
    17:11:30   driven            9107 FALSE   2 rows    <- flipped back on its own
    ..17:12:08 driven            9107 FALSE   2 rows
    17:12:53   driven            9107 TRUE    1 row     <- flipped again
    ..17:13:16 driven            9107 TRUE    1 row

**The same code path produces both outcomes.** The row count tracks the player's BonusMining
tag and nothing else. Driving the state machine does not lose the bonus rows, and never did.

### What that means for everything above

The entire "driven break pays one ore where a swing pays two" premise was an artefact of test
order. Every session in this document hand-mined first and drove afterwards, so the swing
always landed in a FALSE window and the drives always landed in a TRUE one. The correlation
held for a dozen sessions because the procedure never varied, not because the paths differ.

Retracted as causes, all of them measured and none of them the answer:

    the event record          three sessions of rising fidelity, no effect
    +0x1A0 collect drop rate  0 on a swing paying two, 1000000 on drives paying one
    +0xE0 and +0x140          never read by the row walk
    +0x1D8 worker eid         satisfies the presence gate, does not change the verdict
    the null instigator       resolves fine on both paths
    the count function        returns 1 on every call on both paths
    the bonus list (loop A)   empty for veins

Still standing and still useful: the drop path map, the conditioninfo access, the row layout,
and the instrumentation itself. The `+0x1D8` write and the arg3 instigator are both kept
because both are more correct than what they replaced, not because either fixes a yield.

### What is NOT determined

What flips the BonusMining tag. It changed three times in six minutes, once during 72 seconds
in which the mod broke nothing at all, so it is not the mod driving it. A timed buff granted by
mining is the obvious candidate but has not been measured, and the tag list in `buffinfo` is
hashed with something that is not CRC32, FNV-1 or FNV-1a, so the tag cannot be read directly.

This is a curiosity now, not a defect. Worth answering only if someone wants to explain the
mechanic on the mod page.

## THE ROWS ARE BUFF-GATED, and the mod is NOT reliably losing them (session Z, 17:01-17:03)

### What the conditions are

Read straight out of the game's own `conditioninfo` table, which the mod now resolves by name
(`TableGlobal("conditioninfo")` walks the accessor at RVA 0x39A210 to the global at 0x6C2AFD0,
10784 rows). The condition text is stored verbatim as a plain string:

    conditioninfo 9107 (0x2393)   "CheckBuffTag(BonusMining_1) || CheckBuffTag(BonusMining_2), parserType:0"
        evaluator OrContentsLogicFunction        vtable +58A7768
    conditioninfo 9108 (0x2394)   CheckBuffTag(BonusMining_2)
        evaluator ConditionData_CheckBuffTag     vtable +584E460

An ore vein's three drop rows are therefore: one unconditional, one gated on 9107, one on 9108.
Every mining record in `gimmickinfo` carries exactly three output blocks, each min=1 max=1,
which is why a kept row is worth exactly one ore.

Related table content, from the same extraction:

    buffinfo 1000176        BuffLevel_Drop_Mining, the only Mining-tagged buff
    skill 66010/66011/66048 Skill_Mining_I / _II / _III
    knowledgeinfo 1004222.. Knowledge_Skill_Mining_I / _II / _III, Knowledge_Mining

The literal strings BonusMining_1 and BonusMining_2 appear only in `conditioninfo` and
`gimmickinfo`, never in the exe, so the buff's tag list is stored hashed and the hash is not
CRC32, FNV-1 or FNV-1a. Which skill level grants which tag is therefore **not determined**.

### The polarity, settled by measurement

The condition EXCLUDES the row. Interleaving the verdicts with the per-row count calls in one
walk makes it unambiguous:

    two rows kept        COUNT, 9107 FALSE, COUNT, 9108 TRUE
    one row kept         COUNT, 9107 TRUE,          9108 TRUE

The first COUNT is the unconditional row. When 9107 returns FALSE a second COUNT follows, so
FALSE keeps the row and TRUE drops it. That also matches the walk's own branch: the evaluator's
result non-zero falls through to `mov bl, 1`, and bl = 1 is the skip.

This contradicts the natural reading of the ladder ("more buff, more ore"). Having a
BonusMining tag removes these rows rather than adding them. Bismuth shows how the buffed case
is served instead, with its own FrameEvents in `gimmickinfo` rather than drop rows:

    <FrameEvent Event="OnAttackImpulseComplete" Type="SummonGimmick"
        CachedTargetCondition="CheckBuffTag(BonusMining_1) || CheckBuffTag(BonusMining_2)"
        GimmickKey="ore_bismuth_01" SpawnOffsetSocketName="HitPoint" SpawnReason="Drop"/>

### THE IMPORTANT PART: a driven break already got two rows

Rows kept per walk, in order, across one session:

    17:02:34.921  2   HAND SWING
    17:02:39.276  2   driven
    17:02:39.277  2   driven
    17:02:46.309  2   driven      (rock node: dropped Stone AND Fine Stone)
    17:02:56.237  1   driven      <- 9107 flipped FALSE -> TRUE here
    17:02:56.239  1   driven
    ... every walk after this      1

Three driven breaks produced two rows each, the same as the hand swing seconds earlier, and
the pickups confirm it: two Iron Ore at 17:02:39.393, and Stone plus Fine Stone at
17:02:46.558. Then condition 9107 flipped from FALSE to TRUE and everything, driven, dropped
to one row.

**So the mod's driven break is not inherently losing the rows.** The discriminator is a global
player-state change that flips 9107 partway through a session, not the difference between a
swing and a driven break. Every earlier session happens to fit the same shape: the hand swing
was always done first, before the flip, and the drives came after it.

**The test that settles it and has never been run: hand-mine a vein AFTER the yield has
dropped to one.** If a swing also gives one at that point, there was never a mod bug here and
the whole ore-yield investigation was chasing a game mechanic. If a swing still gives two, the
flip is something the mod does and the next question is what.

What flips it is **not determined**. It is a change in whether the player carries a
BonusMining tag, it happened after three driven breaks, and nothing in the mod knowingly grants
or removes a buff.

### Corrections this forces

- `+0x1D8` is NOT the fix. Writing the player's entity id there satisfies the presence gate but
  the rows are still evaluated by condition, and the verdicts above show the condition is what
  varies. The write is left in place as a fallback because it is harmless and the gate is real.
- `+0x1A0` was never the fix either, already retracted.
- The bonus list (loop A) is empty for veins, already retracted.
- The "two rows vs one row" framing is right, but the cause is buff state, not the break path.

## What other mods know (research, 2026-09-07)

**Nobody else breaks nodes programmatically.** Both published auto-loot mods arm the node and
forge the ordinary pickup event instead, which avoids this entire problem.

    CDAutoLoot        https://github.com/Hrecha/cdloot        C++, built for 1.0.0.2760
    Desert Gatherer   https://github.com/KiefBC/desert-modding  Rust, MIT
    Desert Looter     same repo, forges TrocTrProcessPickUpItemOnceTimer (0x0809, 13 bytes)

CDAutoLoot is the reference implementation everyone else decompiled, is built for this exact
build, and already carries a merged PR from this project. Its author's own account of the
arming problem:

> "For a vein that gather data is null until the game itself decides the interaction is
> reachable... call the same function the game uses to fill that node, then send the ordinary
> pickup event with method byte 0x05 instead of 0x00."

Traps it documents that cost real debugging time: the node's data appears a frame or two after
the arm call, not immediately; the ownership oracle is only populated when the game has a
reason to evaluate it, so every wild vein reads as owned; and the byte at gather+5 is not a
reliable kind marker.

### Verified drop-row layout, from the shipped tables

`gimmickinfo` is where gathering yields live, NOT `dropsetinfo`. Each gather record holds a u32
count followed by that many 68-byte blocks:

    +0   u8   presence flag, always 1
    +1   u32  item key
    +10  u32  conditioninfo KEY, owner/gimmick slot
    +14  u32  conditioninfo KEY, third slot, 0 in every gather record seen
    +18  u32  conditioninfo KEY, PLAYER slot: this is where BonusMining lives
    +22  u32  tag/rate hash (0x1DA48CBA mine+ore, 0x1D22D6FF firewood, 0x5FEA8821 plants)
    +42  u64  min
    +50  u64  max
    +58  u16  0xFFFF
    +60  u32  item key again

0x1DA48CBA is the collect key the mod has been logging on every count call all along.

**On disk the slot holds the conditioninfo KEY; at runtime it holds the resolved INDEX.** Key
1009761 is index 9107, key 1009762 is index 9108. Indices move between builds (8,934 rows in an
April 2026 build, 10,784 in this one), so resolve by key or name, never by index.

Block counts per family: Mining 36 records / 108 blocks (three each, all 1-1), Ore Nodes 16/16
(one each, which is why they never vary), Foraging 82/322, Logging 141/141.

### Reading the tables offline

    <game>\0008\0.paz indexed by 0.pamt, as gamedata/<table>.staticinfobody + .staticinfoheader
    header:  u16 count, then count pairs of (u32 key, u32 offset into body)
    record:  u32 key, u32 name_len, name bytes, 0x00, then fields; ends at the next offset

Recent builds renamed `.pabgb`/`.pabgh` to these, which is why searching for
`conditioninfo.pabgb` finds nothing. 134 tables, about 135 MB extracted.
`NattKh/CrimsonDesertModdingTools` ships `schemas/pabgb_full_schema_with_readers.json`, 434
structs and 3,708 fields, including `DropInfoData` with `_ownerConditionInfo` and
`_playerConditionInfo`, and `ConditionInfo` with `_originalString` and `_parserType`.

Warning recorded by another modder: patching `parser_type` 03 to 00 in conditioninfo crashes
the game at the title screen.

## NARROWED to one list of three rows (session U, 16:21-16:23)

The count hook now logs its return address, which separates the two call sites, and the row
walk logs both candidate lists. Both questions answered in one run.

**The probabilistic bonus list is empty for ore veins.** Every single walk reported

    listA 0 x0        the LCG-gated list at [a1+0x268] / [a1+0x270]
    listB <ptr> x3    the plain list at [a1+0x278] / [a1+0x280]

so loop A contributes nothing to a vein and its random threshold at [entry+0x50] is
irrelevant here. The 27 bonus-list count calls in the session all carry key 00000000 and come
from other things entirely, not from veins.

**Both of a swing's count calls come from the row walk.**

    17 calls  from +1775C7D [row walk]     key 1DA48CBA -> 1
    27 calls  from +22168EA [bonus list]   key 00000000 -> 1

and correlating the row-walk calls against breaks:

    16:21:34.951  hand swing on comp 3FD657CB800     2 calls
    16:21:52      2 fresh veins driven               1 call each
    16:22:13      2 fresh veins driven               1 call each
    16:22:26 onward, every driven vein               1 call each

**So the whole problem is one list of three rows.** listB holds three candidate rows for an
ore vein. Two survive the filter on a swing and one survives on a driven break. Nothing else
is involved: the bonus list is empty, the count function returns 1 every time, the drop
definition is the same object.

### What can possibly differ, by elimination

The rows live on the GimmickInfo, which is the same pointer on both paths, so the rows are
identical. a2 is a process-wide singleton, identical. a3 is identical (3FDC00E0200 in every
call). That leaves exactly three inputs the gates are evaluated against:

    [[a4+0x68]+0x20] + 0x2E0   the filter dword, compared against row+8
    [[a4+0x68]+0x30] + 0x1D8   presence gate; zero silently skips any row whose
                               rowdata+8 is not 0xFFFF
    a4 itself                  the vein actor, subject of the rowdata+4 condition

Both dwords were measured as zero on both paths in session T, but that was at the SetState
transition and the walk runs about fourteen milliseconds later, with +0x1D8 known to be
cleared somewhere in that window. The next build therefore reads them inside the walk hook,
at the moment they are actually used, and also prints the three rows: the key at row+8 and
the three u16 condition ids plus the collect key from the rowdata each row points at.

## RETRACTED: +0xE0 and +0x140 are correlation, not cause (static, 2026-09-07)

The section below observed that a hand-swung vein has both vectors populated and a driven one
has neither, two for two against eleven for eleven. That observation stands. The inference
drawn from it does not.

The drop-row walk at RVA 0x1775790 was disassembled in full, all 1474 bytes, and **it never
reads +0xE0 or +0x140**, directly or through any helper it calls. The "one memorised element
plus one base row" arithmetic was a coincidence.

Why the two fields always move together: both belong to the BASE class
`CommonGimmickActorComponent` (vtable RVA 0x057F9D88, type descriptor 0x06981B48), not to
`ClientGimmickActorComponent` (vtable 0x054A5A10, TD 0x06981B10), and one virtual clears both.

    derived destructor   0x0085EC20   via vtable slot 0 thunk 0x0085EBC0 -> 0x097EE300
                                      cleans +0x418..+0x710, then at +0x085F012 tail-jumps to
    base destructor      0x0203A750   cleans +0x18..+0x3D8, including
                                        +0x203AB83  lea rcx,[rdi+0x140]  call 0x2062210
                                        +0x203AB9D  lea rcx,[rdi+0xE0]   call 0x0387120
    reset virtual        base vtable slot +0x020 -> 0x02043ED0 (derived override 0x0085F200)
                                        +0x204427C  [rdi+0x148] = 0
                                        +0x20442C0  [rdi+0x0E8] = 0

    +0x140  std::vector<GameData_GimmickPointData>, stride 0x58
    +0x0E0  keyed vector, stride 0x28, element destructor 0x0388FD0

### Who writes +0x140 (three writers, not one)

All three go through the append helper **0x2061A50** (thunk to body 0x0E016990, which does
`imul rdi, rax, 0x58`, copies the element, refcount-bumps [rsi+0x28], then `inc [rbx+8]`).

    (a) 0x0205B9EE inside 0x0205B6C0, the MemorizeTransform worker.
        execute      0x23440E0, vtable 0x056AD858 slot +0x28 (the pointer is at RVA 0x056AD880)
        type         .?AVGimmickEventHandlerData_MemorizeTransform@pa@@ at RVA 0x069DF540
        dedupe       compares each element's +0x28 against [handlerData+0x58]; equal means skip
        source       [handlerData+0x60] is a bool selecting the handler's own transform or the
                     actor's live one, read via [actor+0x68]+0x1A0 -> +0x98/+0xB8/+0xC0/+0xC2
    (b) 0x026DE3DA inside 0x026DDDB0. Not a chart handler. Sole caller 0x026CF465 inside the
        spawn/setup dispatcher 0x026CE680, gated by `mov rax,[r14+0x88]; cmp byte [rax+1], 7`.
    (c) 0x0283131A inside 0x02831240, the save restore. Copies from [arg2+0x158] with count at
        [arg2+0x160]. Callers 0x026E1867 and 0x0FCF83F5.

So the earlier note that +0x140 "only appears after a node is worked on" is one of three ways
it fills, and none of them touches the drop.

### Who writes +0xE0 (no chart handler at all)

All 206 `GimmickEventHandlerData_*` classes were enumerated, each vtable slot +0x28 execute
resolved, and each execute plus two levels of callees scanned. **None touches +0xE0.**

The single find-or-append accessor is **0x02040F00** (a jmp thunk, the only one) to body
**0x0DF3DA20**:

    +0xDF3DA2F  lea rdi, [rcx + 0xE0]        rcx = the component
    +0xDF3DA36  mov r8, [rdi]                linear search, stride 0x28
    +0xDF3DA50  cmp dword ptr [r8], eax      keyed by a u32 at element +0
    +0xDF3DAAA  call 0x2062FB0               grow
    +0xDF3DAC2  mov dword ptr [rdx], ebx     new element: the key
    +0xDF3DAF7  add dword ptr [rdi + 8], eax  = comp+0xE8, +1

    element  { u32 key; u8 flag @+4; u32* @+8, count @+0x10, cap @+0x14;
               u64* @+0x18, count @+0x20, cap @+0x24 }   0x28 bytes
    callers  0x00C0FCDC (in 0x00C0EFB0..0x00C0FFE0; its callers 0x00C0EF69 in 0x00C0EE60,
             0x0A50B6DE, 0x0A50C2DE), 0x0204E939 (in 0x0204E934..0x0204EC1B),
             0x0DF9AC8F (in 0x0DF9AC70 = base vtable slot +0x840, thunk 0x0204EC30)
    reader   0x02055760, key lookup; key 0 merges all entries via 0x020410E0

What the two inner lists hold is **not determined**.

## THE ROW WALK, disassembled in full (RVA 0x1775790 .. 0x1775D52)

Frame: `lea rbp,[rsp-0x1878]` after 8 pushes, so rbp+0x18C0 is the rcx home slot.

    a1 = rcx   GimmickInfo, looked up from a global by word [component+0x48] via 0x382240,
               which is exactly why a1 is the same pointer for every node of a kind
    a2 = rdx   a process-wide singleton
    a3 = r8    subject of one row condition
    a4 = r9    the VEIN actor (= component + 8)
    a5 = [rbp+0x18E0]  the PLAYER actor
    a6 = [rbp+0x18E8]   a7 = [rbp+0x18F0]

**Two loops, and both trip counts come only from a1:**

    loop A  +0x177580E  mov rbx, [r15 + 0x268]      list
            +0x1775815  mov eax, [r15 + 0x270]      count, stride 8
    loop B  +0x17758B7  mov r14, [r15 + 0x278]      list
            +0x17758BE  mov r15d,[r15 + 0x280]      count, stride 0x10

Since a1 is identical on both paths, both trip counts are identical on both paths. Nothing
else feeds a loop bound.

**Every component access in the whole function** (two, and both on the component at container
slot +0x20, not +0x30 where the ClientGimmickActorComponent lives):

    +0x17757CE  mov rax, [r9 + 0x68]
    +0x17757D2  mov rbx, [rax + 0x20]
    +0x17757D6  mov rax, [rbx + 8]           smart pointer, constructed then released
    +0x17757F5  mov eax, [rbx + 0x2E0]       the filter dword, cached at [rsp+0x44]

Through helpers: 0x17608C0 is a thunk to 0x0C5AD370, which reads [[a4+0x68]+0x30] + 0x1D8,
which IS the gimmick component but at 0x1D8, not 0xE0 or 0x140. 0x2216450 reads only its own
entry object.

**What gates a row in loop B** (bl = 1 means skipped):

    1  +0x17758ED  [r14+8] vs [rsp+0x44]: skip when both nonzero and unequal (the +0x2E0 filter)
    2  +0x17759CE  u16 [rowdata+6], 0xFFFF = none. 0x39A210 resolves the condition object;
                   virtual [obj+8] when a3 is null, [obj+0x10] otherwise. Subject = a3.
    3  +0x1775AC4  u16 [rowdata+4], same shape. Subject = a4, the vein actor.
    4  +0x1775AF1  u16 [rowdata+8]: 0x17608C0(a4) must yield a PRESENT optional. 0xC5AD370
                   returns empty when [comp+0x1D8] == 0, and the row is then skipped outright.
    5  +0x1775BCC  survivors reach +0x1775C78 call 0x20737B0 with rcx = a5 (player),
                   rdx = a4 (vein actor), r8d = [rowdata+0xC]; spawn loop at +0x1775CCD

**Loop A is a separate, probabilistic list.** It resolves each entry with 0x4324D0 and calls
0x2216450 (bounds 0x2216450..0x2216AE2), which:

    rolls an LCG   imul 0x343FD, add 0x269EC3, mod 1000000
    returns ZERO rows when  rand % 1000000 >= [entry+0x50]
    otherwise walks [entry+0x28] with count [entry+0x30], same three conditions,
    keeps only the LAST survivor, makes exactly ONE count call at +0x22168E5,
    and spawns at +0x221694E

**This is the correction that matters.** There are TWO call sites into the count function:

    +0x1775C7D   returning from loop B's call, the node's own drop rows
    +0x22168EA   returning from loop A's call, the probabilistic bonus list

Every earlier statement of the form "a swing makes two count calls and a driven break one" is
therefore ambiguous. Two calls from loop B is a filtered row. One from each is a whole bonus
list that never ran. Those are different bugs.

### The count function's arguments, confirmed

    rcx  the PLAYER actor.   null -> return 1 before reading any rate.
                             [rcx+0x68]+0x20 -> buff array at +0x3E8, count +0x3F0
    rdx  the VEIN actor.     gated by [rdx+0x88]+1 == 7, then [rdx+0x68]+0x30 -> +0x1A0
    r8d  the collect key

Both call sites pass rcx = a5 and rdx = a4, which is how a5 and a4 were identified.

### What runs MemorizeTransform

The chart, not the attack code. 0x23440E0 has zero direct call sites and exactly one pointer
to it anywhere in the image, the vtable slot at RVA 0x056AD880. Its worker 0x205B6C0 has one
caller, inside that execute. So it can only run through the generic handler dispatch on
whichever chart node carries it. Which node and which event carry it is chart data, not code,
and is **not determined** from the exe.

## +0x1D8 measured, and it is not the discriminator

[comp+0x1D8] silently kills any row whose [rowdata+8] is not 0xFFFF, so it was worth checking.
Read out of the session T dumps, no relaunch needed:

    at gimmickon (the mod's snapshot before firing onbreak)   01 00 10 A0  = A0100001, the player
    at break     (every snapshot, hand-swung and driven)      0

It holds the player's entity id while the node is active and is cleared by the break, on both
paths, so by the time the walk runs it is zero either way.

## THE DIFFERENCE, on the component itself: +0xE0 and +0x140 (session T, 15:38-15:41)

A build that hooks the drop-row walk (RVA 0x1775790) and the 0x0821 callback (0x2B65B60) on
top of the count function, and hex-dumps the whole neighbourhood reachable from a vein at
every break, hand or driven. One session, no relaunching between hypotheses.

**Same drop definition, different row count.** The row walk receives the GimmickInfo object
as its first argument, and it is the SAME pointer on both paths for the same node kind:

    iron    hand-swung B0100175  info 25AEC382300  ->  2 count calls
            driven     B0100176  info 25AEC382300  ->  1 count call
    copper  hand-swung B01001D7  info 25AEC35EE00  ->  2 count calls
            driven     B01001D9  info 25AEC35EE00  ->  1 count call

Its other arguments are identical too: a2 is a process-wide singleton (0x12714FBA0 in every
call), a3 is the instigator actor (0x25AF80E0200 in every call). Only a4 is per-call. So the
drop definition, the singleton and the player are all the same, and the walk still emits two
rows on one path and one on the other. The difference has to be state it reads.

**And there is exactly one structural difference, on the vein's own component.** Diffing the
full 0x400-byte component dump of a hand-swung vein against a driven one:

    offset   hand-swung                driven
    +0x0E0   pointer                   0
    +0x0E8   1     count               0
    +0x0EC   1     capacity            0
    +0x140   pointer                   0
    +0x148   1     count               0
    +0x14C   1     capacity            0

Two vector-shaped triples, each holding one element after a swing and empty after a driven
break. This is not a one-off. Across every break in the session:

    25AA0FE5800  hand-swung iron      both populated, count 1
    25A4F101800  hand-swung copper    both populated, count 1
    25AA0FE6800  driven iron          both empty, before AND after the break
    25B0D6B8000  25B0D6B2000  25B0D6B9000  25AA1F25800
    25A6BEDF000  25B0D5E6800          all driven, all empty

Two for two on swings, eleven for eleven on drives. The two hand-swung components kept their
populated vectors when the mod later drove them again, which is expected since they were
already broken.

The arithmetic fits: one memorised element plus one base row is two rows on a swing, zero
plus one is one row driven. That is a hypothesis, not a fact, and the static question is now
what writes +0xE0 and +0x148 and whether the row walk reads them.

**The asymmetry is upstream of onbreak.** The driven snapshot taken before the mod fires
onbreak, with the vein already moved to gimmickon by the mod's own
onattackimpulsecomplete, already shows both vectors empty. So the game's swing populates
them at or before the gimmickon step and the mod's replay of the same event does not. The
event record is byte-identical apart from the stale +0x08 context pointer and the +0x18
counter, so if the record is what matters here, it matters through one of those two, not
through the fields the mod patches.

**Everything else in the neighbourhood matches.** The player's collect-drop buff array is the
same object with the same single entry on both paths, so the player-side bonus is not it. The
row filter candidate at [[veinActor+0x68]+0x20]+0x2E0 is zero on both paths. actor+0x88 is
the same pointer. The component's +0x1A0 confirms the mod's write landed (0xF4240 on driven,
0 on the swing) and confirms again that it changes nothing.

### Rejected, for the record

    +0x1A0 collect drop rate   0 on a swing that paid two, 1000000 on drives that paid one
    the player buff array      identical object, identical contents, both paths
    the row filter at +0x2E0   zero on both paths
    the count function         returns 1 on every call on both paths, 35 calls observed
    instigator resolution      non-null on every call, same actor pointer

## SOLVED, the mechanism: a swing rolls TWO drop rows, a driven break rolls ONE (session S, 15:29)

The count function hook settles it. A hook on RVA 0xE0AD9F0, gated to a window around each
break so an area load cannot spend the line budget, caught both halves in one session:

    15:29:07  hand swing on B0100110                    ->  TWO count calls, 1 item each
    every one of 13 driven breaks that followed         ->  ONE count call, 1 item

Thirteen breaks, thirteen calls, no exceptions, across iron, copper and rock:

    B0100114 B010010C B0100110 B010014F B01001BB B01001BA B01001DE B01001BC
    B01001E1 B01001F8 B01001F9 B0100200 B010020E B0100215 B0100207 B0100222

Copper was checked separately on the suspicion it behaved differently: five veins driven
(B01001DE, B01001BC, B01001E1, B0100200, B0100207), five count calls, five Copper Ore. The
two pickups that arrive together at 15:32:17.856 come from the two breaks at .580 and .788,
one each, not two from one vein.

Every call returned **1**, every call had the same row key **0x1DA48CBA**, and every call had a
valid non-null instigator (0x3055A0E0200). So:

- The count function is innocent. It never returns anything but 1 on either path.
- The instigator resolves fine on the driven path. The null early-out never fires.
- The bonus arithmetic is irrelevant. Driven rows had +0x1A0 at 1000000 and still returned 1,
  which independently confirms session Q: whatever that field is, this call does not read it.

**A swung vein matches two drop rows. A driven vein matches one.** That is the entire
difference, and it has been the difference all along.

The user's live count agrees: a retest gave one ore per driven vein and two per swing. An
earlier report of "two for each, one fell down a cliff" was withdrawn on retesting, so pickup
counts and the hook now tell the same story.

### Where the filter is

The static analysis named the mechanism: which drop rows run at all is filtered by a key read
off a sibling component of the vein, and rows whose filter field is nonzero and unequal are
skipped entirely. The chain is

    [[veinActor + 0x68] + 0x20] + 0x2E0        dword

with the row iterator at RVA 0x1775790. The staged build prints that dword on every arrival at
break, along with each link of the chain so a broken pointer and a genuine zero cannot be
confused:

    [bonus] <comp> reached break, rate <n>, filter <XXXXXXXX>
            (actor <p> sib <p> sub <p>, event <id>, instigator yes/no)

A hand swing and a driven break in the same session put the two filter values side by side.
If they differ, that dword is the whole bug and the mod can set it before firing onbreak, the
same way it tried with +0x1A0 but this time against a field the drop path actually reads.

### Dead, do not retry

    the event record            three sessions, rising fidelity, no effect. The driver reads +0x00 only.
    +0x1A0 collect drop rate    zero on the swing, 1000000 on driven veins, no effect either way.
    the state machine fields    +0x270, +0x27C, +0x350, +0x358 are not read on the drop path.
    GameData_GimmickPointData   a vector of memorised transforms, not a drop definition.
    tool tier / gather skill    no such query exists anywhere on the drop path.
    the payload u32 at +11      overwritten at +0x29E49A0 before any read.
    firing the break twice      one descriptor per break on both paths, 13 for 13.
    the null-instigator branch   instigator is non-null on both paths, measured.

## Session Q: +0x1A0 is NOT the difference, and the hand swing proves it (15:17-15:18)

The build that raises the vein-side bonus was tested against a hand swing in the same session.
Both halves of the measurement came out against the hypothesis.

**The hand swing leaves +0x1A0 at zero.**

    15:17:34.423  [bonus] 5217B168000 reached break with collect drop rate 0 (event FD8F7D2C)

That vein, B0100122, then dropped B0100147 and B0100148: two ore, from a component whose
additional collect drop rate was zero. So the vein-side term is zero on the path that pays
two, and the SetAdditionalCollectDropRate handler does not run on an ore vein at all.

**Raising it to 1000000 changed nothing.** Six fresh veins were driven with the field
confirmed at 1000000 on arrival at break, and every one paid exactly one:

    15:17:41  B0100120, B0100125  rate 1000000  ->  B0100152, B0100153      one each
    15:18:09  B01001B9, B01001BA  rate 1000000  ->  B01001DE, B01001DF      one each
    15:18:34  B01001FF, B01001F9  rate 1000000  ->  B0100215, B0100216      one each

This is the informative part. If the raised field were reaching the arithmetic at all, one
whole extra unit on top of whatever the player contributes would have produced three ore, or
at the very least two. It produced one, unchanged. A value that is written, verified in place,
and then has no effect whatsoever is a value that is never read.

**Inference, not yet confirmed:** the count function returns at its first branch. The
disassembly at RVA 0xE0AD9F0 is unambiguous about that branch being before any rate lookup:

    +0xE0ADA0D  test rcx, rcx
    +0xE0ADA10  jne  0xE0ADA1C
    +0xE0ADA12  mov  eax, 1
    +0xE0ADA17  jmp  end

and it is the only path that both ignores the bonus and returns exactly one. It also explains
the hand swing paying two from a zero vein term, because the player-side lookup after that
branch is where the bonus lives:

    +0xE0ADA2B  mov rax, [rsi + 0x68]        ; rsi = instigator
    +0xE0ADA2F  mov rcx, [rax + 0x20]
    +0xE0ADA33  mov rax, [rcx + 0x3E8]       ; buff array, stride 0x10
    +0xE0ADA3A  mov edx, [rcx + 0x3F0]       ; count
    +0xE0ADA50  cmp [rax], ebx               ; keyed on the drop row's collect key

Two other readings survive the same evidence and the measurement below separates all three:
the drop callback rolls two rows on a swing and one on a driven break, or the count is fine
and something downstream discards an entity.

### The measurement now staged

`kSig_DropCount` matches the body of 0xE0AD9F0 uniquely. The function has no direct callers,
every call site reaching it through a one-instruction thunk, so hooking the body catches every
drop in the game. Watch only, nothing calls it.

    [count] drop row key %08X -> %u item(s)   instigator %llX [ (NULL: base count...) ]

Read it like this, on one hand swing followed by a few driven veins:

    one call, instigator non-null, returns 2   then driven returns 1 with a NULL instigator
        -> the instigator fails to resolve on the driven path. That is the bug.
    one call returning 2, driven one call returning 1, instigator non-null in both
        -> the player-side buff lookup is being missed for a reason not yet identified.
    swing rolls two rows returning 1 each, driven rolls one
        -> a row is being filtered out, and the count function is innocent.

### Retracted from the section below

The claim that a swing walks the chart through a node running SetAdditionalCollectDropRate,
and that a break driven straight to onbreak skips it, is **refuted** for ore veins. The field
is zero on both paths. Everything else in that section (the count formula, the field map, the
list of what the drop path does not read) still stands, and the ruled-out list in particular
was independently confirmed by this test: writing to the state machine's own fields changed
nothing about the yield.

## ANSWERED: what decides how many items a break drops (static analysis, 2026-09-07)

The count comes from one small function, RVA **0xE0AD9F0**, which has no direct callers at
all: every call site (0x1775C78, 0x20504C8, 0x22168E5) goes through the one-instruction thunk
at RVA 0x20737B0, so a single hook catches all of them.

    if (instigator == null) return 1;
    bonus = playerBuffValue + gimmickComponent[+0x1A0];      // signed qword, millionths
    return 1 + bonus / 1000000 + (rand() % 1000000 < bonus % 1000000 ? 1 : 0);

1000000 is exactly one guaranteed extra item. The base is always 1. It is called once per
matching drop-table row and its return value is the trip count of the loop that spawns the
entities, so the item total is the sum over rows.

**The vein-side term lives at ClientGimmickActorComponent + 0x1A0**, the same object the mod
already holds and whose +0x270 state id it already reads. In the whole image it is written by
exactly one thing: the gimmick chart handler `GimmickEventHandlerData_SetAdditionalCollectDropRate`,
whose execute is vtable slot +0x28 at RVA 0x2347C10 and which copies its own +0x58 straight
into the field. It is not an accumulator and not node init. It is a handler hung off a node of
the vein's chart.

That is the whole explanation. A pickaxe swing walks the chart through the node carrying that
handler, which sets +0x1A0 to 1000000, and the break then pays two. A break driven straight to
onbreak never visits that node, +0x1A0 stays zero, and the count function returns its base 1.

### What this rules out, permanently

- **The event record.** The transition driver at 0x891E90 reads exactly one offset of it,
  +0x00, the event id. Nothing on the drop path reads +0x04, +0x08, +0x18 or anything else.
  Three sessions of increasingly faithful record replay changed nothing, and now it is clear
  why. Stop tuning record contents.
- **The state machine.** Nothing on the drop path reads +0x270, +0x27C, +0x350 or +0x358.
  How the node reached break cannot change the yield except through +0x1A0.
- **GameData_GimmickPointData at +0x140.** It is not a drop definition. It is a
  std::vector<GameData_GimmickPointData> of memorised transforms (data ptr +0x140, count
  +0x148, stride 0x58), written by a MemorizeTransform handler, which is why it only appears
  after a node is worked on. The drop path never touches it, and re-fetches its data row from
  a global table keyed by [component+0x30] instead.
- **Tool tier and gathering skill.** There is no such query anywhere on the drop path. The
  player-side half of the bonus is a pre-aggregated buff value written by
  CommonVaryCollectDropRateBuffProcessor and keyed by the drop row's collect key at +0xC. A
  pickaxe can only matter by having put a value in that table beforehand.
- **The payload u32 at +11.** It arrives as arg5 and is overwritten at +0x29E49A0 before any
  read. Dead on this path.
- **RNG.** The roll only fires on the fractional part of the bonus. A clean 12-of-12 split
  means the fraction is zero on both paths.
- **A second spawn path.** The local (non-replicated) route at 0x276F140 converges on the
  same spawn routine, so there is no alternative path to blame.

### Two things that could still be the real shape of it

- The drop callback at 0x2B65B60 bails to a base count of 1 the moment the instigator fails
  to resolve. The mod passes the player's entity id and the payload bytes are identical to a
  swing's, so this is unlikely, but it produces exactly the observed symptom and costs one
  log line to eliminate.
- Two ore might be two rows returning 1 each rather than one row returning 2. The item total
  cannot distinguish them. Counting calls to 0xE0AD9F0 would.

### The change now staged

`DriveNow` reads +0x1A0, logs it, and raises it to 1000000 if it is lower, immediately before
firing onbreak. It uses the game's own field and the game's own arithmetic, and the node is
about to break and despawn so nothing needs restoring. `RaiseDropBonus` never lowers a value,
so a vein whose chart already pays more keeps its own rate.

Alongside it, every arrival at the break state now logs +0x1A0 whoever caused it, capped at
16 lines. One hand swing and one driven break in the same session put the two values side by
side, which is the measurement that confirms or kills the whole theory:

    [bonus] <comp> reached break with collect drop rate <n> (event ..., instigator yes/no)
    [bonus] eid ... drop rate was <n>, raised to 1000000 before driving the break

If a hand swing shows 1000000 and an untouched driven vein shows 0, the diagnosis is exact.
If a hand swing also shows 0, the extra ore comes from the player-side buff term instead and
the same fix still works, but for a different reason worth recording.

### Field map additions

    ClientGimmickActorComponent +0x1A0  qword, additional collect drop rate, millionths
                                +0x198  dword, unidentified
    count function        RVA 0xE0AD9F0   (only reachable through the thunk at 0x20737B0)
    drop row iterator     RVA 0x1775790   calls the count function once per matching row
    spawn routine         RVA 0x29E45D0   0x38-byte drop entries, count at [rbp-0x78]
    SetAdditionalCollectDropRate execute  RVA 0x2347C10

## Session P: the whole record replays cleanly and the yield still does not move (14:42-14:44)

The mod now copies a real captured record verbatim and patches only event id, instigator,
target and position. Both records were captured on the first hand-mined vein, and the
context pointer at +0x08 stayed readable through every later drive (no "went away" line).

    [learn] event 3DB4A808 record captured, context 22C351BB89C (no rtti)
    [learn] event FD8F7D2C record captured, context 22BDE1E0D80 (ClientNormalInGameActor@pa@@)

So the onbreak context IS an actor. The onattackimpulsecomplete one carries no RTTI, so it is
a plain struct rather than a polymorphic object.

Yield, small sample but consistent with every session before it:

    hand-mined vein B0100131  ->  B010017C and B010017D, two Iron Ore
    2 fresh iron veins driven ->  2 Iron Ore, one each
    1 rock node driven        ->  1 Stone (declined by the mod's own "stone off" rule)
    4 breaks, 4 drop descriptors, 1:1 as always

### The record is now exonerated, by session-to-session diff

Diffing the session O and session P captures of the same two events, on different veins in
different game runs, separates structure from instance. Everything in the SAME column the mod
now supplies byte-for-byte correct, and the yield did not change:

    onbreak    SAME across sessions   +0x60 = 1.6397   +0x70 = 10.0   +0x74 = 11936.0
                                      +0x90 = FFFF0005   +0xC0..C8 = 1.0,1.0,1.0   +0xD8 = 1.0
               per-instance           +0x08 pointer, +0x18 counter, +0x20 target, +0x30 pos,
                                      +0x78, +0x84

That is the end of the record hypothesis. Three sessions have now tested it at increasing
fidelity (nothing, three fields, the entire structure) with no movement at all. It matches
what the drop-callback disassembly said back at the top of this file: the spawn routine is
reached through the VEIN, not the payload.

One caveat on the current build: +0x78 held 0x013FF000 in session P and zero in session O.
0x13FFxxx is the stack region the game's small event builder uses, so that field is sometimes
a stack address and the mod is replaying a stale one. Only +0x08 is readability-guarded.
Harmless so far, but if a crash ever appears on the drive path, look here first.

## A vein is TWO components, and the mod only knows one (session P)

Every gimmick state change is applied twice, on two different objects, from two different
call sites, in an exact 1:1 pairing:

    45 transitions  return address +0x8921D9      inside the transition driver 0x891E90
    45 transitions  return address +0x101E9FDC    inside the function at 0x101E9F00

    hand-mined vein B0100131, 14:43:53
      .834  22BDE2B9000  wait      -> gimmickon   from +0x8921D9
      .843  22BDE2B9000  gimmickon -> break       from +0x8921D9
      .845  22C91843600  wait      -> gimmickon   from +0x101E9FDC
      .849  22C91843600  gimmickon -> break       from +0x101E9FDC

The addresses are the giveaway. The mod's own entity-to-component map only ever produces the
first family (22BDE2B6800, 22BDE2B8000, 22BDE2B9000, 22BEE32E800 for the four veins it
touched this session). The second family, 22C918xxxxx, never appears in a single probe line,
so `game::CompByClass(game::Comps(eid), kCls_Gimmick)` cannot reach it. The mod has been
driving one half of a pair the whole time.

The second half is not a passive mirror. Its call site sits in a 709-byte function that runs
SetState and then four more calls, each handed the component and the new state id:

    +0x101E9FD7  call 0x2047580   SetState
    +0x101E9FDC  call 0x29CB320
    +0x101E9FE7  call 0x29CB990
    +0x101E9FF2  call 0x29CBD30   also gets arg2
    +0x101EA000  call 0x29BCFF0

Those four sit in the same neighbourhood as the drop spawn routine at 0x29E45D0, which is
where the item count is decided. The function also opens with a guard that can skip
everything:

    mov  rcx, [rcx + 8]        ; arg1 is a gimmick component, so this is its actor
    mov  rax, [rdx + 0xa0]     ; arg2 + 0xA0
    test rax, rax
    je   -> eax = 0
    mov  eax, [rax + 0x60]
    cmp  [rcx + 0x90], eax
    jne  0x101EA1A4            ; bail

and a later block gated on `test r15b, r15b` (set only when the old state was found in the
component's state table) and `cmp ebx, [r14]` (skipped when old state already equals new).

**Do not treat this as the answer yet.** The mod's drives DO reach the second component: at
14:44:36 the drive on the already-broken B0100131 produced break->break on 22BDE2B9000 at
.052 and on 22C91843600 at .059, seven ms later. Propagation works. And every driven break
does fire its drop descriptor. So the second component is reacting; the open question is
whether it reacts with the same information a real swing gives it.

## CORRECTION: +0x08 and +0x0C are one pointer, and the mod was forging it (session O, 14:31-14:34)

The section above reads `+0x0C` as an "event-kind flag" because it held 0x403 on both vein
events in session L. Session O captured the same two events in a fresh game run and it held
**0x47A**. A flag does not move between runs. A heap region base does.

    session L   records allocated at 0x403C2279E00 / 0x403C237CC00      +0x0C = 0x403
    session O   records allocated at 0x47AFCBDAB00 / 0x47AFCBDF200      +0x0C = 0x47A

`+0x08` and `+0x0C` are the low and high halves of one 64-bit pointer, and it points into the
same heap region the record itself was allocated from:

    session O   onattackimpulsecomplete   +0x08 = 0x0000047A951BB89C
                onbreak                   +0x08 = 0x0000047AFC417080
                onbranchendframe (spawn)  +0x08 = 0                      null, no instigator

The consequence is that `DriveGimmickEvent` has been writing a wild pointer on every single
drive since it was written: 0x403 hardcoded into the top half, over a low half that was
either zero or (once learning worked) a stale value from the hand-mined vein. Never once a
valid object. That is the strongest remaining candidate for the missing second ore, since a
per-swing context object is exactly where a tool tier or gather bonus would live.

It did not crash because nothing on the break path dereferences it unconditionally, which is
also consistent: a yield bonus is read behind a null check, and a garbage pointer that fails
that check falls through to the base yield of one.

### Full record layout, three events side by side (session O)

    offset  onattackimpulsecomplete  onbreak            onbranchendframe   reading
    +0x00   3DB4A808                 FD8F7D2C           128B06E9           event id
    +0x04   64B3EC5B                 00000000           00000000           payload, per event
    +0x08   0000047A951BB89C         0000047AFC417080   0                  CONTEXT POINTER (8 bytes)
    +0x10   A0100001                 A0100001           B010021C           instigator entity
    +0x14   A0100001                 A0100001           B010021C           instigator again
    +0x18   00011D8C                 00011D93           000215D4           sequence counter
    +0x20   B0100113                 B0100113           0                  target entity
    +0x30   0                        -9946, 602.6, -4688  0                world position
    +0x60   0                        1.640              0                  scalar, break only
    +0x6C   00000400                 0                  00000400
    +0x70   5.0                      10.0               5.0
    +0x74   1000.0                   11872.4            1000.0
    +0x84   00000479                 00000001           0
    +0x90   FFFFEC05                 FFFF0005           FFFF0005
    +0xAC   00000001                 0                  00000479
    +0xC0   1.0, 1.0, 1.0            1.0, 1.0, 1.0      1.0, 1.0, 1.0      a scale vector
    +0xD8   1.0                      1.0                1.0

Every field from `+0x60` down was zero in the record the mod built. The three-field learn
covered `+0x04`, `+0x08` and `+0x18` and left the rest, which is why it changed nothing.

## Session O: learning works, replaying three fields does not (14:31-14:34)

Filtered learning behaved exactly as designed. Two `[learn]` lines, no others, both from the
hand-mined vein before auto-loot ran:

    14:32:23.922  [learn] event 3DB4A808 carries +04 64B3EC5B +08 951BB89C +18 00011D8C
    14:32:23.931  [learn] event FD8F7D2C carries +04 00000000 +08 FC417080 +18 00011D93

The yield did not move.

    hand-mined vein B0100113   ->  B0100166 and B0100167, two Iron Ore, consecutive eids
    5 iron veins driven        ->  4 Iron Ore
    5 copper veins driven      ->  5 Copper Ore
    3 rock nodes driven        ->  3 Stone, all three skipped as "stone off"

First time both halves of the comparison appear in one log: a real swing paying two, driven
breaks paying one, same session, same character, same tool.

**The rock "regression" is not a regression.** Sessions M and N2 recorded zero stone from
driven rock nodes and I treated it as a second fault. Session O shows the stone entities
spawning normally (B0100243, B010024E, B0100259, one per node) and being declined by the
mod's own class rule:

    [why] B010024E 11.3m Stone: stone off | type 1224 tag 07 cat 00/15

Rock nodes were working the whole time. Nothing to chase.

**One drop event, two items.** Each break fires exactly one `DropItemOnGimmickBreak`
descriptor (0x0821), hand-mined and driven alike, 13 breaks and 13 descriptors. The
hand-mined one produced two ore. So the count is decided inside the drop handler, not by
firing the drop twice, which rules out "send the break event twice" as a fix and points
back at the context the handler reads.

The descriptor payload is identical in shape either way, player as instigator in both:

    21 08 FF | target eid | instigator eid | 00 00 00 00 | three floats, a unit vector

## Driving the state machine WORKS, and does not fix the yield (session M, 14:18-14:20)

`logs/session-M-drive.log`. `DriveBreak=1`. The mod builds a 0xE8 record with event id,
0x403 at +0x0C, the player at +0x10/+0x14, the target at +0x20, position at +0x30 for the
break, everything else zero, and calls the driver through its trampoline.

**Fact: the transition happens.** Two milliseconds after the mod's first `[drive]`:

    14:18:53.458  [drive] eid B0100123 driven through the state machine
    14:18:53.460  comp 4FA40524800  wait      -> gimmickon  from +0x8921D9
    14:18:53.460  comp 4FA40524800  gimmickon -> break      from +0x8921D9
    14:18:53.470  comp 4FA40524800  break     -> break      from +0x8921D9

The zeroed fields are accepted. The events resolve. The node moves through the game's own
machine, driven by the mod.

**Fact: the yield is unchanged.**

    5 copper veins -> 5 Copper Ore
    5 iron veins   -> 5 Iron Ore
    6 rock nodes   -> 0

Exactly one per vein, the same as the drop event gave. The mod sent no drop event this
session (`DriveBreak` replaces it), so that ore came from the transition, which confirms
the transition drops. It just drops one.

**So the yield was never about the mechanism.** Hand-mining and mod-driving now run the
same events through the same states and produce different amounts. The remaining
differences are narrow and enumerable:

 - `+0x04` and `+0x08`, which the mod zeroes. The game's own
   `onattackimpulsecomplete` carried 0x64B3EC5B and 0x98FBDDD0 there, and its `onbreak`
   carried 0 and 0xC2E8EE80. Attack context is the obvious candidate, and the tool bonus
   would live in attack context.
 - The driver's arg3, the instigator, which the mod passes as 0 and the game passes a real
   pointer for. If the yield bonus is read from the instigator, zeroing it loses it.
 - `+0x18`, a small counter the mod zeroes.

**CONFIRMED BY THE PLAYER: the veins broke and disappeared.** That is symptom 2 from the top
of this file, solved. Driving the state machine does what sending the drop event never
could. Symptom 1, one ore instead of two, remains.

**Also unexplained:** six rock nodes were driven and yielded nothing at all, where the drop
event used to get stone from them. Worth checking whether they transitioned.

**Note on instrumentation:** `DriveGimmickEvent` calls the trampoline, so the `[event]` hook
cannot see the mod's own calls. The `[state]` hook can, because SetState is a separate
detour reached from inside the driver. That is why `[event]` looked empty and `[state]` had
the answer.

## Addresses worth keeping

    SetState                     RVA 0x2047580 .. 0x20476AD   (301 bytes)
      the inc that gave it away  RVA 0x20475CE   inc dword ptr [rbx+0x350]
      direct call sites          0x0088BFE3 0x008921D4 0x02043FE6
                                 0x0204593A 0x0205153E 0x029CAEC7
    ClientGimmickActorComponent  type descriptor RVA 0x06981B10
      vtable                     RVA 0x054A5A10
      slot +0x878                VA 0x14204D570 -> jmp thunk -> RVA 0xDF8E7F0
    OnStateChanged               RVA 0xDF8E7F0   (about 795 bytes)
    transition driver            RVA 0x891E90    vtable slot +0x7D0; its SetState call
                                                 returns to +0x8921D9
    driver wrapper               RVA 0x86EC60    resolves arg3, runs three gates and a
                                                 post dispatch; the mod skips all of it
    second state applier         RVA 0x101E9F00 .. 0x101EA1C5 (709 bytes)
                                                 its SetState call returns to +0x101E9FDC
    logout candidate             RVA 0x2915150
    branch-state candidate       RVA 0x29126C0

    the drop path, end to end
    0x0821 descriptor callback   RVA 0x2B65B60 .. 0x2B65C3D  (221 bytes)
                                 one spawn call, two conditionals, neither selects a
                                 different call or different arguments
    spawn routine                RVA 0x29E45D0   0x38-stride vector of drop entries,
                                 element count at [rbp-0x78], filled by 0x1775790,
                                 handed on at +0x29E544A to 0x29D5F40 with the string
                                 "spawnByGimmickSceneObjectDrop" at RVA 0x5A0A380
    ROW WALK                     RVA 0x1775790 .. 0x1775D52  (1474 bytes)
      loop A list/count          [a1+0x268] / [a1+0x270], stride 8
      loop B list/count          [a1+0x278] / [a1+0x280], stride 0x10
      the +0x2E0 filter read     +0x17757F5, cached at [rsp+0x44]
      row skip branches          +0x17758ED +0x17759CE +0x1775AC4 +0x1775AF1 +0x1775BCC
      loop B count call          +0x1775C78, returns to +0x1775C7D
    bonus list                   RVA 0x2216450 .. 0x2216AE2
      LCG                        imul 0x343FD, add 0x269EC3, mod 1000000
      gate                       zero rows when rand % 1000000 >= [entry+0x50]
      rows                       [entry+0x28] with count [entry+0x30], last survivor only
      its count call             +0x22168E5, returns to +0x22168EA
    COUNT FUNCTION               RVA 0xE0AD9F0 .. 0xE0ADB53  (355 bytes)
      reached only through       the thunk at RVA 0x20737B0; zero direct callers
      null-instigator early out  +0xE0ADA0D test rcx,rcx / +0xE0ADA12 mov eax,1
      player buff array          [[rcx+0x68]+0x20] + 0x3E8, count +0x3F0, stride 0x10
      vein bonus                 [[rdx+0x68]+0x30] + 0x1A0, gated on [rdx+0x88]+1 == 7
      formula                    1 + bonus/1000000 + (rand()%1000000 < bonus%1000000)
    GimmickInfo lookup           word [component+0x48] into a global via 0x382240

    class layout
    ClientGimmickActorComponent  TD RVA 0x06981B10, vtable 0x054A5A10
      destructor                 0x0085EC20 (slot 0 thunk 0x0085EBC0 -> 0x097EE300)
      reset override             0x0085F200
    CommonGimmickActorComponent  TD RVA 0x06981B48, vtable 0x057F9D88   the BASE
      destructor                 0x0203A750, cleans +0x18..+0x3D8
      reset virtual              slot +0x020 -> 0x02043ED0, clears +0x0E8 and +0x148
      slot +0x840                0x0DF9AC70 (thunk 0x0204EC30), touches +0xE0

    component field map, as far as it is known
      +0x008  the owning actor (ClientNormalInGameActor)
      +0x030  data-table key the drop path re-fetches the GameData row with
      +0x040  state table       ([+0x90] dword array, [+0x98] count, [+0x8] compared to old state)
      +0x048  u16, indexes the global that yields this node's GimmickInfo
      +0x0C0  object the handler propagates the recognised state into
      +0x0E0  BASE. keyed vector, stride 0x28, count +0x0E8, capacity +0x0EC
              accessor 0x02040F00 -> 0x0DF3DA20, reader 0x02055760
              element {u32 key; u8 flag@+4; u32* @+8 cnt@+0x10 cap@+0x14;
                       u64* @+0x18 cnt@+0x20 cap@+0x24}
              NOT written by any of the 206 chart handlers; not read by the drop path
      +0x140  BASE. std::vector<GameData_GimmickPointData>, stride 0x58, count +0x148
              written by MemorizeTransform (0x205B6C0), by the spawn dispatcher
              (0x26DDDB0) and by save restore (0x2831240); append helper 0x2061A50
              NOT a drop definition and NOT read by the drop path
      +0x160  queue of delayed actions, drained by 0x2047FA0 on every transition
      +0x1A0  additional collect drop rate, signed qword in millionths
              written only by SetAdditionalCollectDropRate, execute RVA 0x2347C10,
              which copies its handlerData+0x58 into it
      +0x1D8  the instigator's entity id while the node is active, cleared by the break.
              Zero here makes 0xC5AD370 return an empty optional, which silently skips
              any drop row whose [rowdata+8] is not 0xFFFF
      +0x270  current state id
      +0x27C  last RECOGNISED state id (only written when the id is in the table)
      +0x280  time elapsed in the current state, reset to zero by SetState
      +0x28F  flag, |= 1 on every state change
      +0x350  change COUNTER, incremented per state change (3 -> 4 -> 5 across a break)
      +0x358  latched counter value, set once when the state first equals [table+8]
      +0x3E0  cleared on every state change
      +0x460  queue the game posts {record*, int} pairs into, drained at 0x871120

    chart handlers named so far
      MemorizeTransform            execute 0x23440E0, vtable 0x056AD858 slot +0x28
                                   (pointer at RVA 0x056AD880), TD RVA 0x069DF540
      SetAdditionalCollectDropRate execute 0x2347C10

    signatures the mod resolves for this investigation
      kSig_SetState      +0x2047580     kSig_StateDriver  +0x891E90
      kSig_DropCount     +0xE0AD9F0     kSig_DropRows     +0x1775790
      kSig_DropCallback  +0x2B65B60

## Artifact index

- `logs/` snapshots of the sessions above, A through T.
- `diffdump.py <log> <ts-a> <ts-b>` diffs two `[dump]` snapshots byte by byte, marking which
  bytes of each 32-byte row differ. The timestamps are the ones on the `---- ... break ----`
  header lines. This is what found the +0xE0 / +0x140 difference.
- `extract_evidence.py` regenerates the session tables in this file from those logs.
- `diff_slots.py <log> <eid> <hh:mm:ss.mmm>` diffs a gimmick's slots across a break.
- `watch_helper.py addr` tails the log and prints a debugger command per nearby vein;
  `watch_helper.py rip <addr>` turns a debugger stop into an RVA and disassembles it.
- `disasm.py <rva> [count]` disassembles from an RVA.
- `funcbounds.py <rva>` exact function bounds from the PE exception table.
- `vtable.py find <Class>` RTTI to vtable; `vtable.py slot <vtable> <off>` reads a slot.
- `find_const.py const <hex>` is a dword a literal in the image; `find_const.py xref <rva>`
  finds direct call sites.
- `find_state_writer.py [value|any]` scans for stores into `[reg+0x350]`.
- `../../scratchpad/veinwatch.cpp` the external debugger used to catch the writer. Not part
  of the mod and must never become part of it; see the watchpoint section above.
- `../../codex/` first Codex review: descriptor disassembly, break symbols, exe hash.
- `../../codex/log-review/` second Codex review, against the logs.

# The yield function, found 2026-09-08

Four parallel investigations, each challenged by a second pass. Two conclusions
survived, two were thrown out. What follows is only what survived scrutiny.

## The number comes from one function

**RVA 0x0E0AD9F0.** Reached only through the thunk 0x020737B0, from three call
sites: 0x1775C78, 0x20504C8, 0x22168E5.

    count(rcx = instigator object, rdx = vein component, r8d = collect key)

      rcx null                  -> return 1                    (0xE0ADA0D)
      player term               [rcx+0x68] -> +0x20 -> table at +0x3E8,
                                keyed by the collect key, count at +0x3F0
      vein term                 [rdx+0x68] -> +0x30 -> +0x1A0, which is what
                                SetAdditionalCollectDropRate writes,
                                gated on byte [[rdx+0x88]+1] == 7   (0xE0ADA7D)
      then scaled by 1e6, rolled through an MSVC LCG, returns rbx + setl + 1

A player term of exactly 1,000,000 turns a base of one into a base of two. That
is lsimo's five against ten with an external times-five on top, and it is the
first arithmetic in this file that predicts the reported numbers exactly.

## The event record is not involved. At all.

**This kills the hypothesis the rest of this file was built on.** The count
function does not take the event record as an argument. Verified twice
independently, by disassembling the prologue and by tracing the row-walk call
site 0x1775C71, which loads rcx from an incoming argument threaded from the
spawn routine 0x29E45D0 and the descriptor callback 0x2B65B60. That callback
resolves the actor from an EID through the manager 0x029B3E10.

So `+0x08`, the context pointer this file called "the strongest remaining
candidate for the missing second ore", cannot reach the yield. Neither can
`+0x04`, `+0x18`, or any other byte. The null check that inspired that theory is
real, but it guards the count's own instigator argument, which is resolved from
record `+0x10` and which the mod already fills correctly.

Corrections to the record table earlier in this file, which is stale:

- The mod does a full 0xE8 copy of a learned record, not a three-field build.
- The mod passes the real player actor as the driver's arg3, not zero.
- `+0x08` on a driven break holds a live, readable actor from the first learned
  vein. Wrong vein, but not garbage, and not dereferenced on this path anyway.

## Also dead

**No direct-to-inventory ore path.** lsimo's "puts ore right into my inventory"
is instant pickup of floor drops, not a separate spawn. Session O logged a
hand-mined vein producing two ground entities with parent 0 while the drill was
equipped. No inventory descriptor is raised on a break in roughly twenty-five
logged sessions.

**The BonusMining gate is not the missing row.** Satisfying it *removes* a floor
row rather than adding one, which is what the earlier "inverted polarity" note
was seeing. Waiting for the gate would lower the yield.

One useful fact did come out of that angle and it stands on its own: **buff tags
are Jenkins lookup3 over the lowercase name**, the same hash the mod already uses
for events. This file previously recorded that the tag hash "is not CRC32, FNV-1
or FNV-1a" and stopped there; lookup3-lowercase was never tried.

    bonusmining_1  0xE04443D9
    bonusmining_2  0x0154D670

Both live in buffinfo key 1000176, BuffLevel_Drop_Mining, one leveled buff.

## What is actually left

Two inputs, and the answer is one of them:

1. **The player term.** The buff table at `[instigator+0x68]->+0x20->+0x3E8`
   keyed by the collect key. Either the mod's break resolves an instigator whose
   table lacks the entry, or the tool's buff is not active at that moment.
2. **The vein term.** `veinComp+0x1A0`, written only by
   SetAdditionalCollectDropRate. A real swing runs a chart branch that may call
   it; the driven break may never reach that branch, leaving it zero. The
   `[[vein+0x88]+1] == 7` gate has never been read on a live vein either.

**Stop disassembling and measure.** Both inputs are readable at the moment of a
break. Hook the count, or read its three arguments, and compare a hand-mined
vein against a driven one in the same session. That single capture decides it,
and no amount of further static reading will.

The standing lesson from this round: two of the four investigations reached a
confident answer by following a chain that stopped short of the yield and
assuming the last link. Both were thrown out by the challenge pass for exactly
that. The chain has to reach the number.

## Measured at last, 2026-09-08 19:20

The count function was hooked and its arguments logged on a live session, with a
character carrying a mining drill that pays three ore by hand.

**A hand swing makes the game evaluate two drop rows. A driven break evaluates
one.** Matching the yield calls against the mod's own `[loot] break` lines
separates them without ambiguity:

    hand   19:19:20   two calls on vein C5EFF300, paid 1 and 1     total 2
    hand   19:20:00   two calls on vein C2AEF800, paid 2 and 1     total 3
    mod    19:19:28   one call each on three veins, paid 1         total 1 each
    mod    19:20:05   one call each on three veins, paid 1         total 1 each

**Both candidates this probe was built to test are dead.** The player term at
`[instigator+0x68]->+0x20->+0x3E8` is byte-identical on every line, hand and
driven alike, and so is the count beside it at `+0x3F0`. The vein term at
`+0x1A0` reads zero throughout, on hand swings that paid three as much as on
driven breaks that paid one. The `[[vein+0x88]+1] == 7` gate is satisfied in
every case.

Neither term differs. The number of times the function is called does.

So the missing ore is not a smaller number coming back from the count. It is a
whole drop row that the game never walks when the mod drives the break. That
moves the question to the row walk at 0x1775790 and the conditions that admit a
row, which is where the conditioninfo 9107/9108 BonusMining gate lives.

Note on the labels: every line above printed HAND, including the driven ones.
The marker is set for the length of DriveNow and the drop resolves after that
returns, so it never covers the call. The `[loot] break` timestamps are what
separated them, within about 30 ms. A later probe should hold the marker open
for a window after the drive rather than for the call itself.

Also worth recording: the first probe logged everything and spent a sixty-line
budget in thirty seconds on calls with no collect key that returned one, before
the player reached a vein. This function is asked about far more than mining.
Filter on a real collect key, a result other than one, or the mod's own break.

