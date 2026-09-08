# Auto-loot as Damiane: what is known, and where it stops

Eight rounds of investigation on 8 September 2026, not solved. Written so the
next attempt starts from evidence instead of from the folklore.

## The claim being tested

Players say auto-loot only works with Kliff in the party. The other autoloot mod
ships `; - Supported characters: Kliff and Female CC Kliff.` in its ini and has a
log line about detecting a "CC puppet" for Damiane and Oongka, so it hit this and
drew a boundary rather than a fix.

## What is established

**The played character is not player-tagged.** As Damiane the only `0xA0` actor
is `A0100001`, and it is a fixture: byte-identical position `945.5 686.7 87.4` in
every session regardless of where the player stands or which save is loaded, with
the nearest world object 1200 to 1800 m away.

**She is findable by her gear.** A dressed character is the thing with items
parented to it. On a populated pass:

    [worn] B0100005:20@1752m/rt90100000 B0100002:10@1758m/rt90100000 ...

`B0100005` carries ten to twenty items, sits in the middle of the streamed world,
and is on the player's own route `90100000`. It is world-tagged, `0xB0`, which is
why no rule based on the player tag can find it.

**The game's own pointer is not the actor.** The take-or-steal routine gates on a
global, `[0x6C29760] -> +0x30 -> +0x58` (see WELL.md's method notes for how it was
found). Following it yields id `90100000` with tag `0x90`, which is the player's
route object, not a body: it has no transform, and using it left the engine
measuring from nothing.

## Where it stops

**The actor manager does not hand over the world.** That is the wall, and it sits
underneath everything above.

    8x   1 world objects
    1x  22 world objects

That is a whole session as Damiane. Earlier sessions occasionally saw 475 to 640,
never sustained. `ForEachEntity` walks `{count, cap, ptr}` triples between
`+0x100` and `+0x200` of the manager, and dumping them shows lists of one and two
entries against a capacity of sixteen.

So every rule tried for choosing the player was choosing from a list that does not
contain the world, and none of them could have worked:

1. First player-tagged actor enumerated. A coin toss with a party.
2. The actor the game names in its own ownership check. It asks about followers
   too, and this moved the scan to an actor with nothing within 40 m.
3. The player-tagged actor with the most world around it. Same starved list.
4. The biggest gear holders as extra candidates. They are absent from the pass
   that does the choosing.
5. Refusing to choose from a pass under fifty entities. There is no such pass.

## What the next attempt should do

Do not touch the player pick again. Find out why the manager is nearly empty for
this character, which is a question about how the game enumerates actors, not
about which actor is which. Two starting points, both cheap:

- Disassemble a game function that iterates actors and see which structure it
  walks. The vtable scan finds two or three `ClientActorManager` globals; the live
  one for this character may not be among them, or the world may hang off a
  different member than the `+0x100`..`+0x200` window assumes.
- Capture the same diagnostics as Kliff for comparison. Everything here is a
  Damiane reading with no Kliff control, so it is not yet known whether the
  manager is starved for Damiane specifically or intermittently for everyone and
  only noticed here.

The diagnostics are all in place and cost nothing when the debug log is off:
`[scan]` counts and tag census, `[mgr]` list dump, `[worn]` gear holders with
routes, `[player]` candidate comparison.
