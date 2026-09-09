# Naming a creature

How the mod learned to tell a Tench from a Ricefish, after a year of not being
able to. Issue #39, and the real answer to #29.

## What was wrong

`FindSpecies` worked out what a creature was by walking its memory and looking
for a species word inside a model or asset string. It walks four blocks, `ent`,
`actor`, `status` and `ai`, at 0x300 to 0x400 each, plus one dereference, and
reads every string it finds.

The game does not put species names in those strings. Across twelve recorded
sessions, every word it ever matched was a generic one:

```
325 effectmonster -> insect      27 fish   -> fish
 69 smallbird     -> animal      11 animalwallupwalldownnowater -> animal
 63 goose         -> animal       5 aironly -> insect
 43 bird          -> animal       4 duck    -> animal
 36 chicken       -> animal
```

All nine are entries in the generic word list in `creaturedb.cpp`, and generic
entries register with row `-1`. So `Match::row` stayed null and `c.species` was
null. In those twelve sessions it named an exact species three times: Fruit Fly
twice, Pond Loach once.

That mattered beyond the log being vague. The catch branch only reaches
`ItemDb::ByRow(c.species->itemRow)` when a species resolved, and that call is the
only place an item rule can refuse a live catch. So no per-species control had
ever worked, by any route, and a Creatures tab added in 1.6.4 was removed again
because it needed the same thing and covered less than the item rules already
did.

## The answer

`status + 0x30` is a 16-bit row into the game's `characterinfo` table. It names
the creature outright. Resolve the row to its string key with
`game::KeyInTable`, look the key up with `CreatureDb::ByKey`, and that is the
exact species, marked `exact` with trust 5.

The string walk stays as the fallback. In a full session after the change, two
creatures out of everything seen fell back to it.

## How it was found

A probe swept every field of the four blocks `FindSpecies` already walks, plus
one dereference, and reported a value only when it resolved to a creature the
mod's own table recognised. The cross-check is what makes a hit mean something: a random 16-bit value lands
on some row of some table constantly, and almost never on one that names a
creature.

Consistency is what settled it. `status+0x30` was the most frequent hit by a
clear margin, nine against five for the next, and the class
always agreed:

```
probed as fish   x3   status+0x30 = 4000   Small Rasbora (fish)
probed as insect x2   status+0x30 = 3860   Camel Cricket (insect)
probed as insect      status+0x30 = 3932   Firefly Colony (insect)
probed as animal      status+0x30 = 6657   Partridge (animal)
```

Every other offset that ever hit named a different animal each time, which is
what coincidence looks like. The two apparent class mismatches were creatures
the old string classifier had called animals, one of them the firefly cluster,
which is the exact failure being replaced.

## Two rounds that found nothing because they tested nothing

Neither probe could have found anything, and both reported a confident "nothing
found". A probe that fails silently costs more than one that fails loudly.

The first looked up the table by name and took the first match. There are 111
static tables in the image and more than one is called `characterinfo`: the one
it found has four rows against the real 7250. Its bounds test then discarded
every candidate value. Its own header line said "4 rows in the table", five seconds after the gimmick
probe had printed 7250 in the same session.

The second read only 16-bit fields. That can hold a row index, but `creatures.tsv`
is also keyed by `character_key`, and 517 of its 1004 keys exceed 65535 with the
largest at 3653044009. The more likely encoding was the one it structurally could
not see.

The lesson is the one already written elsewhere in this project: check what the
probe is actually testing before believing its negative.

## Confirmed in play

`Item_Ricefish` set to never produced six refusals reading
`Ricefish: item override: Food_Ricefish`, alongside thirteen Pond Loach that
reached `out of range` instead. The item rule is evaluated before the range
check, so a Pond Loach getting that far means it went through the rule and was
let past. Two species in the same water at the same distance, named separately, one
filtered and one let through.

Repeated on insects: a Centipede refused by rule while a Black Centipede was
caught. Two species sharing a word, resolved to different rows.

## Prior art, checked

Nobody else had solved this, verified by reading the code rather than search
results.

- `github.com/Hrecha/cdloot`, the rival autoloot mod, is open source. Its
  `core.cpp` has no mention of species, CharacterInfo or charKey, and it reaches
  item names through the same ItemData and GatherData path.
- `github.com/blizz3010/CrimsonDesertCoop`, the most serious public RE on this
  game at 76 commits, types entities through
  `candidate+0x48 -> +0x08 -> actor -> +0x88 -> +0x01`, which is the same byte
  this mod already reads as `cat2`. Nothing about species.
- Crimson Desert ESP, Nexus 3277, does label live actors but is closed source
  with hardcoded per-version offsets. Its author describes a `charKey` as
  "essentially the entity id", which is what this mod already has.
