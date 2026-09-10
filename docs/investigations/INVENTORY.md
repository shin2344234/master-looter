# Deleting from the inventory, the actor roster, and who is being played

Three things learned on 10 September 2026 while closing issue #32, each of
which cost several test rounds and none of which should be re-derived. Build
2760 throughout.

## The delete event

`TrocTrDeleteItemFromInventoryOnceTimer`, id 0x0807 on 2760, payload 25 bytes,
is the same OnceTimer family as the pick-up this mod already raises, so it
goes out through the same queue. The mod's `events::DeleteItem` builds it.
The id is taken from the descriptor walk by class name and only falls back to
0x0807 when the walk did not see it.

The handler at RVA 0x2B63CE0 reads the payload into a 0x28-byte item key:

| key    | from payload | meaning |
|--------|--------------|---------|
| +0     | (constant)   | item row, hardcoded 0xFFFF: this event never names a row |
| +8     | u64 at +3    | instance: the slot's own u64 at slot+0, or -1 for any |
| +0x10  | u16 at +0xB, through a table lookup | inventory type |
| +0x12  | u16 at +0xD  | slot index, or 0xFFFF to search |
| +0x18  | u64 at +0xF  | amount |
| +0x22  | (constant)   | dword 0xFFFF0000 |

The u16 at +0x17 is passed to the delete on the stack as its count; the mod
sends the amount there too and a stack of two went in one event.

The delete itself (RVA 0xFD169C0) groups keys by the inventory type at +0x10
and looks for an inventory on the holder whose u16 at +0x10 matches; none
matching ends the whole thing with `eErrNoInvalidInventory`, which is the
Jenkins hash 0x73353994, the error every early layout got. The per-inventory
step (RVA 0x234D9A0) then takes the slot index path: the index must be below
the u16 at inventory+0xC, the slot's type at +8 must not be 0xFFFF, its count
at +0x10 must be positive, the instance must be -1 or equal the slot's u64,
the amount must not exceed the count, and byte slot+0xA1 must be clear. The
search path (+0x12 == 0xFFFF) refuses a 0xFFFF row outright, so it is
unreachable from this handler.

The table that turns payload +0xB into an inventory type is the one the
resolver at RVA 0x432C20 serves, global 0x6C2A038, 21 rows named Money,
Character, PearlUser, PearlCharacter, Quest, Wagon, PetAndVehicle,
CampWareHouse, WareHouse, Bank, CampStraw, Recovery, Kuku,
InvisibleInventory, Housing_Dresser, Housing_Refrigerator, Housing_Symbol,
Housing_Collecting, Housing_GatheredMaterials, BirdFeed, Ship. The lookup at
RVA 0x8752A40 hashes the key over `[table+0x68]` buckets at `[table+0x78]`,
0x100 bytes each, `{u32 key, u32 index}` pairs from +8, into the row array at
`[table+0x80]`, and returns the row's u16 at +6 when its u16 at +4 matches.
On 2760 the key is the type plus one: the bag (type 1) is reached through key
2, Housing_Refrigerator (type 15) through 16. The mod does not assume that:
`game::InvTypeKeysFor` walks the buckets and returns every key whose row+6 is
the wanted type, and the table itself is found among the static tables by
its first two row keys, so nothing here is an address the next patch moves.

What did not work, so nobody tries it again: the u32 instance alone in the
first field (the comparison is against the full u64), the item row in +0xB
(it is not an item table), and every layout with a slot index of 0xFFFF.

`DeleteTestName=Hay` in `[MasterLooter]` with the verbose log on deletes two
of that item once a session through the production path and dumps the
buckets and the type table on the way. That is the check to run after a game
patch. Take the key out afterwards or it keeps eating hay. The verbose log
also places a hook on the delete routine, guarded by its first 38 bytes so it
is never placed on another build, which says how the game answered.

## The actor roster

For every version until now the scan read the actor manager's lists at
+0x180 and +0x190, sixteen slots each, which hold one entity or none on most
ticks and a few hundred in a burst every twenty seconds or so. The rolling
window of things seen was one second, so the Nearby list emptied between
bursts, a body was only found once its gear happened to be in a burst, and a
Damiane session could take seventeen seconds to start. Every older log,
Kliff sessions included, has the same shape.

The world is kept elsewhere: a dozen pools between manager +0x128 and
+0x2E0, each a pointer to an array of entity pointers followed by a packed
count and capacity word. The count in that word is not the live count; it
read zero over 246 entities. `ForEachEntity` takes no count from anywhere.
From the lowest pool pointer it walks forward while the entries are entities,
gives up after sixteen that are not, and skips pointers inside a run already
covered. That gives 550 to 990 world objects a tick where there was one, and
the body is chosen within the four-second barren clock. `[roster]` in a
verbose log prints each pool twice a session, with the raw words round the
pointer, which is what found them and is the first thing to read on a new
build if Nearby goes back to flipping.

## Who is being played

The rule from DAMIANE.md stands: the player actor A0100001 is the thing to
send events as, and the body being steered is found by its gear. Two things
were wrong in it. The identity is not a fixture at one fixed spot with
nothing near it: 0,1000,0 in one session, -485,970,100 and 945,686,87 in
the next, sometimes in a crowd, and it jumps between them on a load or a
swap. The other mistake was holding a body for as long as it kept being
enumerated. A parked body is enumerated for ever, so a swap from Damiane to
Kliff left the scan on her body for the rest of the session, Nearby empty.

One signal settles every swap: who walks. A walking step is half a metre to
fifteen metres between two sightings, which covers a horse and excludes every
jump. As Kliff the actor walks with the player; as Damiane or Oongka it never
does. So the actor walking drops any held body at once (`[player] ... walked,
so it is the body being played`), a played body that walked in the last ten
seconds is chosen without waiting for the barren clock, and between Damiane
and Oongka, whose bodies both carry the played bytes, the one that walked in
the last three seconds takes the centre from one that has stood for ten,
after the usual hold. The barren clock stays as the fallback for a session
in which nothing has walked yet, and it now tolerates two objects in range,
because a fixture with one stray beside it never let it start.

Two signals were tried first and each threw Damiane's body away once: a
crowd of world objects round the actor, because the actor stood in one; and
any movement of the actor at all, because it jumps on a load. Neither is
evidence. The coop project's "user actor at manager +0x28" is not an entity
on this build, and the take-or-steal global still lands on the route object.
