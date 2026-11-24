# Dropping an item on the ground

Research on 19 September 2026, build 2.03.00 (exe 1.0.0.2944), for leaving a
refused item from a body search on the ground instead of deleting it. Nothing
here has been sent from the mod yet.

## What the game uses when you drop something by hand

`TrocTrDiscardItemReq`. Private Storage Master's request capture of 17
September 2026 (the `RequestRun` hook in its commit `31b69e8`, output in
`bin64\PrivateStorageMaster.pushable-*.log`) caught it 73 times across four
sessions in which items were dropped by hand. That capture records the
descriptor each request runs under, not its payload, which is why all 73 rows
are the same bytes. Read against the pick-up and the body search in the same
files, the descriptor says:

| field | discard | pick-up (for comparison) | body search |
|-------|---------|--------------------------|-------------|
| +8, mask | 0x00000004 | 0x7FFFFFFE | 0x7FFFFFFE |
| +0xC, id | 0x0B21 | 0x0809 | 0x07E8 |
| +0x18, size | 5 | 0x0D | 7 |
| +0x1A, limit | 0x7FC6 | 0x0D | 7 |

So the discard is a variable-length request: a 5-byte header whose u16 at +3
is the body length, then a serialized body. It sits under mask 4, which is why
the mod's resolver, asking under the game's own `DESC_MASK`, has never named
it, and why the mask sweep of 10 September (issue #32) found only its Ack
(0x08B4) and Nak (0x08B5) under mask 1.

The body is read by the parser in slot 2 of the class's packet vtable
(`vtable.py find TrocTrDiscardItemReq` gives RVA 0x5B22DB0, parser 0x2A05800).
In order it reads 2 bytes, 2 bytes, 8 bytes, 12 bytes, a field through
0x141FB10, 4 bytes, a field through 0x141FBF0, a structure through 0x12AD950,
then single bytes. Against the delete event's key in INVENTORY.md, the first
three are most likely the inventory type key, the slot index and the slot's
u64 instance, and the 12 bytes a position, which fits an item landing on the
ground where the player stands. That reading is a guess until a payload is
captured. Private Storage Master's R6A notes name the routine it reaches
`discardfrominventory` and put it among the callers of the shared give and
remove helper.

## The captured payload

Captured on 19 September 2026 by the `1.6.30-dropcap` build, six drops by hand
from the bag. The id is 0x0AA5 under mask 4 in that session. The 0x0B21 read from the
Private Storage Master rows was something else, so find it by class name every time.
Nothing came through the event queue the mod spies on. Every capture came from
the hook on the parser, so a discard does not travel the way the mod's own
events do.

Every one is 235 bytes, the 5-byte header and a 230-byte body.

| offset | size | value in the six captures | reading |
|--------|------|---------------------------|---------|
| +0 | 2 | A5 0A | the id |
| +2 | 1 | 03 | always 3; the mod's own events carry FF here |
| +3 | 2 | E6 00 | body length, 230 |
| +5 | 2 | 2 | inventory type key: the bag, key 2 as in INVENTORY.md |
| +7 | 2 | 200, 84, 131, 32, 181, 18 | the slot index |
| +9 | 8 | 1, 1, 1, 21, 1, 3 | the amount; the 21 and the 3 were whole stacks |
| +0x11 | 12 | about -6548.6, 625.9, -1047.9 | where it lands, three floats |
| +0x1D | 16 | 0, 0.84, 0, 0.55 and similar | a rotation, a quaternion about the vertical |
| +0x2D | 185 | the same in all six | scale vectors of 1.0, a run of FF, and a u32 0x16BCE near the end |

The item is named by slot and not by row or instance, which is the delete
event's approach as well. The position is the player's feet in a frame offset
from the scan's. The scan had the body at -547.6, 627.4, -48.5, so the drop is
the player's position plus about -6000 on x and -1000 on z, within a metre.
Where that offset comes from is the open question before the mod can send one.

## The drop routine

The parser does no work of its own. It takes the sender actor from the
packet, asks GetInventoryHolder (`+0x212A0F0`, the function `kSig_InvHolder`
finds) for that actor's bag, and calls

    +0x2B64C40(holder, u32* error, actor, u16 inventoryKey,
               u16 slot, i64 amount, const float* transform)

where the transform is the position at +0 and the rotation quaternion at
+0xC, exactly as the payload carries them. An error of zero is success; any
other value makes the parser answer the sender with 0x3F5. So the mod would
not have to build a packet at all. It can call this routine on the game
thread the way Private Storage Master calls the game's move.

What the routine checks, in order: the key turns into an inventory type, the
slot is not 0xFFFF, the slot holds an item with a count of at least the amount
(an amount over the count is an error), and the holder passes a check at
`+0x2133840`. Then it refuses a position within 1.2e-7 of zero on all three
axes, so there is no default position to lean on. Past that it takes one of
two paths depending on the item. One loops once per unit through `+0x2B4EA50`,
which is why a dropped stack lands as several objects (the 1.6.28 finding).
The other drops the whole amount at once through `+0x2B4CCA0`.

## The frame of the position

Settled by the `dropcap2` build on 19 September 2026. The transform component
the mod already reads (actor +0x68 components, +0x1A0 transform) carries the
position twice more, at +0x324 and at +0x3D0, in the game's own world frame.
The +0xB4 the mod reads is the same point less a regional origin, and that
origin moves: it was -6000 and -1000 on x and z in the first capture and -9000
and -4000 in the second, a few kilometres away. So nothing fixed can convert
one to the other, and the drop reads +0x324 directly.

| drop | the drop asked for | transform +0x324 | transform +0xB4 |
|------|--------------------|------------------|-----------------|
| 1 | -9724.58 562.41 -4299.63 | -9726.01 561.97 -4299.81 | -726.01 561.97 -299.81 |
| 2 | -9715.02 563.27 -4293.46 | -9715.78 562.82 -4293.63 | -715.78 562.82 -293.63 |

A hand drop lands about a metre from the player, a little above the feet.
Both captures were sent by A0100001, the player actor, playing as Kliff.

## The body drop that nobody has seen

`TrocTrLootingDropFromDeadBodyReq`, id 0x0A18, header 5 bytes. Its parser
(packet vtable 0x5B22F30, slot 2 at 0x2A11630) reads a u32, a byte and a byte,
looks the u32 up as an actor, and hands the two bytes to a routine on that
actor's component at +0x68 then +0xC8 (0x2B81390), then answers with 0x3F5.
The u32 is very likely the body's entity id. The name says the body spills its
loot on the ground, which would let every item be judged on its own like a
person's dropped gear, but in 86,034 captured requests, seven of them body
searches, it never ran once. It may be the path for a full bag, or a server
path the single-player game never takes.

## It has to run on the server thread

The first test build called the routine from the mod's own game thread, and it
faulted every time at `+0x2BBCCCD` reading 0x168. The routine takes a context
pointer from the thread's TLS block at +0x250 (gs:[0x58], first entry, then
+0x250) and hands it on, and on that thread the pointer is null. The block's
byte at +0x1EC picks between two world roots. Private Storage Master's
research had already found that the game runs a server of its own on a
separate thread, with its own holder for the same bag, and the discard parser
runs there.

The movement tick the mod hooks as its pump fires on both threads. In the
19 September session one thread showed context 506E071DB00 and world byte 1,
the other context 0 and byte 0. The server also keeps its own actor: the
sender the server hands a parse was 506EA0E0200 where the client's player was
506B9527800, and the two holders differed the same way.

So the second build hooks the server's parse of the mod's own search (slot 2
of `TrocTrProcessLootingDeadDropOnceTimer`'s packet vtable, `+0x2C313B0` on
2.03.00), reads the server's bag before and after the original, and drops the
refused rise right there with the sender and the holder that parse has. It
worked on the first run: nine drops across eighteen searches with every class
off, each with error 0, none faulting. Every search that paid anything paid it
inside its own parse. The eleven that paid nothing during the parse put nothing
refused in the bag afterwards either, since the bag side queued nothing. The pick-up send's parser at
`+0x2C344F0` does not read the packet the way the others do and was not
hooked, which cost nothing here.

## The switch

Drop refused loot from bodies, under General, off by default. The mod reads
the carried bag slot by slot on either side of the server's parse of its
search. Only an instance that is new, or the amount a stack grew by, gets
judged and dropped. A copy the player already carried is never the one that
goes. It covers searches the mod queued in the last ten seconds
and searches the event queue saw a pet or a companion raise, and nothing the
player searched by hand. For a pet's search the sender is the pet, so the bag
is the one GetInventoryHolder gives for the pet and the drop lands where the
player stands, which the scan publishes each pass. That half has not been
played yet; its first three searches in a session log the sender and the
holder. The drop lands where the played body
stands: as Kliff that is the sender's own transform, and as Damiane or Oongka
it is the body's transform, read as the scan's enumeration hands the body
over, since the sender there is the identity standing somewhere else.

Played on 19 September 2026: four drops as Kliff, then eleven as the B0 body,
which read -6485.6 970.3 100.9 in the world against -485.6 970.3 100.9 in the
scan's frame. A Poison Arrow dropped at 12:56:14.037 was a world object 1.1 m
from the body 0.4 s later. No refusal and no fault from the drop in 35
searches.

## What the player drops

Built on 24 September 2026 and played the same morning. Until then the scan saw an item
dropped out of the bag as an ordinary world object and took it straight back.

The discard request is the only sign a hand drop gives, since nothing about it
reaches the event queue, so the mod detours the discard parser as well, on the
server thread and whatever the switch above says. `hkDiscardParse` reads the
packet the way the parser does and gives up on one whose length field at +3
disagrees with the packet's own at +0x10. `HandDropParse` then reads the slot
the payload names out of the bag GetInventoryHolder gives the sender. The slot
gives the item row; the payload gives the amount and the world position.

The note reaches the scan before the original parse runs, so nothing the
parse puts in the world can reach a scan that is not expecting it. After the
parse the slot is read again for the log only. A slot that still holds the
whole stack does not withdraw the note, because nothing yet shows that the
routine empties the slot before it returns; if it empties it a moment later,
withdrawing would give every drop back to the scan. A note for a drop the game
refused finds nothing and closes when its window runs out.

`ClaimHandDrops` runs on every scan pass between Fill and the verdicts. It
turns the payload's position into the scan's frame with the difference between
the played body's +0x324 and +0xB4, read on that pass or at most three seconds
before it, the pair the mod already reads to place refused body loot. Anything of the same item row within 5 m
of that point is a candidate, nearest first, up to the amount dropped, as long
as the scan first filled it on a later pass than the last one it had finished
when the drop was parsed. What already lay there is left to the ordinary rules.
When the converted point lands more than 10 m from the player on a note the
scan took at once, the frames are taken not to have converted and the player's
own position is used. A note stays open 15 seconds and closes 3 seconds after
its first match, because the pieces of one stack land together. Each match is
refused as "you dropped this" for the rest of the session, by entity id, and
by instance id for any other piece of the same stack with the same row.

A drop made while the scan is off, with auto-loot off and the menu shut, cannot
be tested for newness. Its note waits for a pass that can convert the spot,
takes the nearest objects of that row as the scan reaches them, and starts its
15 seconds once the player is within 5 m of the spot. A note made while the
scan ran falls back to the same rules if the scan stops before the note is
done, which shows as a pass more than two seconds after the one before it:
after a gap every object reads as new and the player may be anywhere. Notes
that are over are removed before a pass looks for anything, so a stale one
never claims on the pass that ends it.

The world object keeps the bag slot's instance id. The first session with the
watch in it, on 24 September 2026, dropped a Short Hair Hide and two Shovels,
and each object carried the id its slot had held (`000F4998`, `000F498D`,
`000F498E`), recognised about 0.2 s after the drop and 0.8 to 1.7 m from where
the game was asked to put it. So the slot's id and row are now refused the
moment the scan takes the note, wherever the object lands and however late it
turns up, and the log line for that is `known by its instance id`. An object
the id catches counts against its note, so the note does not go on to hand
something else of that row to the drop by position. The position match stays
behind it for any item that turns out to get a new id when it lands. The
opposite direction does not hold: a pick-up gives the bag a new id, which the
duplicate probe of 15 September 2026 found on all 133 of its tries.

## Chests the mod opens

Issue #89. Sov asked on 27 September 2026 whether the drop could happen on
opening a chest, and `DropRefusedChests=1` is the answer, off by default and
ini only. The mod opens a container with the pick-up request,
`TrocTrProcessPickUpItemOnceTimer`, in gather mode: the id, `FF`, `05` at +3
and the target's entity id at +4. Slot 2 of that descriptor is detoured the
same way the search's is, and `ChestParse` runs the search's bag diff either
side of it. Every pick-up anybody makes passes through that parser, the
player's own included, so the scan marks a container's id just before it
sends the open, and the parse judges only a target marked in the ten seconds
before. Anything else returns at once and writes nothing. A target counts as a
container when the node table's kind says so or when the verdict's kind does.

Unproved when it was built on 24 November 2026: whether a container pays
during the parse of its open, the way a body pays during its search. If it
pays later, every chest writes `put nothing new in the bag` and nothing is
dropped, and the next step is finding the request the payout rides on.
