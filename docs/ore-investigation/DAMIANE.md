# Auto-loot as Damiane and Oongka

Solved 8 September 2026, after eight rounds and one near-abandonment. Master
Looter is the first autoloot mod for this game that works while playing a
character other than Kliff. Written up because the wrong turns are as useful as
the answer, and because the community folklore is wrong in an instructive way.

## The folklore

Players say auto-loot only works with Kliff in the party, and treat it as a game
limitation. The other autoloot mod ships this in its ini:

    ;   - Supported characters: Kliff and Female CC Kliff.

and its binary carries a log line about detecting a "CC puppet" for Damiane and
Oongka. It found the same wall and drew its support boundary there.

## What is actually going on

**The player identity and the player's body are two different entities.**

The identity is the player-tagged actor, `A0100001`. The game raises its own
events under that id whatever character is on screen, so it is the right thing
to send loot events as. Playing as Damiane it is a fixture: byte-identical
position `945.5 686.7 87.4` in every session regardless of where the player
stands or which save is loaded, with the nearest world object 1200 to 1800 m
away. It is not standing anywhere.

The body is whatever the identity is currently wearing. It is a **world-tagged**
actor, `0xB0` like scenery, `B0100005` in the capture, standing 1750 m from the
identity with twenty items parented to it. The other mod's "puppet body" and
"playerEid" are exactly these two.

Playing as Kliff the identity *is* the body, so nothing ever surfaced this for
anyone.

## How the body is found

By its gear. A dressed character is the one thing in the world with a handful
of items parented to it, and gear is attached, so one item's world position is
the body's. The body itself never has to be enumerated for the scan to stand
where it stands.

The scan feeds a rolling record of gear holders every tick (`NoteHolder` in
`mod/src/loot/engine.cpp`) and remembers each for fifteen seconds. Once the
player actor has proved barren, meaning the scan finds nothing at all around it
while the world plainly has objects, the scan centres on the best holder
(`BestHolder`): fresh, carrying three or more items, on the player's own route
when that is known, carrying the most. Sticky while it stays fresh, or the first
populated scan would clear the barren clock and the centre would flap back to
the fixture.

"Yours" (`IsMine`) covers the body's gear as well as the identity's, in all four
places the engine asks, or the character's armour would be looted off her back.

Loot events keep going out as the identity, `g_meEid`. That is the id the game
itself uses, and it works: the first Damiane session with this in place gathered
lavender, wood and trees, and Oongka was confirmed working the same afternoon.

## How it is done, exactly

Four pieces, all in `mod/src/loot/engine.cpp`. Nothing else in the engine
changed.

**1. Record who is carrying things, every tick.** In the scan's entity walk,
before the range filter drops anything far away:

    if (const uint32_t par = game::ParentEid(e))
        NoteHolder(par, q, game::Route(e), now);

`q` is the child's world position, which is the parent's. `NoteHolder` keeps a
table of 32 holders with the most children seen in any one tick, the last
position, the route, and when it was last seen. Entries older than fifteen
seconds are ignored; the stalest slot is reused when the table is full.

**2. Pick the body.** `BestHolder(now, playerRoute)`: fresh, three or more
children, on the player's route when the route is known (`events::Route()`,
learned from the game's own first event), and carrying the most.

**3. Stand there.** Immediately after the scan reads the player actor's
position into `mp`:

    if (body && (g_barrenSince || g_bodyEid == body->eid))
        mp = body->pos;

`g_barrenSince` is set when a scan finds nothing in range while the world
plainly has objects. So as Kliff, whose actor is the body and is never barren,
this never fires. The `g_bodyEid == body->eid` half makes it sticky: the first
populated scan clears the barren clock, and without stickiness the centre
would flap back to the fixture on the next tick.

**4. Do not loot the body's own gear.** `IsMine(parent)` returns true for the
identity's eid or the body's, and replaces `parent == g_meEid` in all four
places the engine asks: the "worn or carried by you" verdict, the `own` flag
that keeps kit out of the Nearby list, the arm loop, and the record of what was
recently on the player that stops a thrown weapon being picked up.

Loot events are unchanged. They go out as `g_meEid`, the identity, because the
game raises the player's own events under that id whatever body is on screen.

## The one lesson under all the failures

**The actor manager hands the world over a few entities at a time.** One to
twenty-two on most ticks, 475 to 640 occasionally, never sustained. The scan has
always known this and keeps `g_seen`, "objects seen in the last second", for
exactly that reason. The player logic never got the same treatment.

Every one of the five rules below decided **from a single enumeration pass**,
and every one of them picked wrong for that reason and no other. The fix is not
a sixth rule. It is feeding a rolling record so that all the partial passes add
up, which is what the scan already did for everything except the player.

### Rules that failed, so nobody writes them again

1. **First player-tagged actor enumerated.** A coin toss with a party, and the
   fixture is usually first.
2. **The actor the game names in its own take-or-steal check.** It asks that
   question about followers too. Following it moved the scan to an actor with
   nothing within forty metres.
3. **The take-or-steal routine's own global**, `[0x6C29760] -> +0x30 -> +0x58`,
   found with the disassembler. It yields the player's *route* object, id
   `90100000` with tag `0x90` and no transform, not a body. Following it left
   the engine measuring from nothing at all.
4. **The player-tagged actor with the most world around it.** Same starved
   list. Also the fullest `ClientActorManager` global: both candidates read
   zero entities before the world loads, so the pick was a toss-up dressed as a
   measurement.
5. **Gear holders as extra candidates, decided from one pass.** The pass that
   chose never contained them.

## Known wrinkle

The centre can flap between holders. A wagon carries ten attached parts and a
horse carries tack, and both are usually beside the player, so they qualify and
sometimes win. In the first working session the centre moved between the body
(12 items) and a wagon (10) and something 50 m off carrying 15. It worked
because the wagon is next to the player, but a holder 50 m away shrinks what
the scan can reach. Hysteresis on the switch, and weighting `/equip/` children
over `/attach/` ones, are the obvious refinements.

## Diagnostics that stay

All cost nothing with the debug log off, and they are what any further work
starts from:

- `[scan]` census: world objects enumerated, how many in range, the centre's
  position, a tag census, and the nearest object with its prefab.
- `[mgr]` list dump when the world comes back nearly empty.
- `[worn]` gear holders with child counts, distances and routes.
- `[player]` every centre switch, with what was chosen and why.
