# Working out what a gimmick does

Written after the well, 8 September 2026. The ore work took days without this;
the well took one session with it. Read it before instrumenting anything.

## Three layers, and most questions die on the wrong one

**The event queue.** `SpyEnqueue` in `mod/src/loot/events.cpp`. Player actions
arrive here. Two filters used to hide things and both are lifted now: everything
the world raised about itself was dropped by the player-tag check, and a player
gather whose node type was unknown was never logged, which is exactly the case
worth hearing about. An empty log here is evidence now rather than an artefact.

**The gimmick state driver.** `hkStateDriver` in `mod/src/loot/hooks.cpp`. Every
state change in the world passes through it, which is where mechanisms live. It
logs `[gstate] event <id> target <eid> comp <ptr>`, three samples per id and
sixty-four ids a session, under the debug log. Raise that cap before attempting
to replay a sequence, or the repeating phases come out truncated.

**The game's own data.** The `.binarygimmick` files declare the state machine
outright. Check this first. It is free and it answers more than it looks.

## The workflow

Extract the definition. The unpacker wants the `.pamt` and its folder; the
`pycrimson` CLI could not read these packs.

    cd tools/crimson-desert-unpacker/python
    py -3 paz_unpack.py "<game>/0008/0.pamt" --paz-dir "<game>/0008" \
        -o <out> --filter "*gimmick_well_*.binarygimmick"

Read the declared states and their hashes:

    py -3 docs/ore-investigation/gimmick_states.py <out>/gamedata

Capture in game with the debug log on, then name what the driver saw:

    py -3 docs/ore-investigation/name_events.py

Python dependencies are vendored in `codex/python-deps` (capstone, lz4, tqdm,
cyclopts, bier); put that on `PYTHONPATH`. Extracted game assets are not
committed.

## The state names predict less than they look like they do

It is tempting to read a gimmick's declared states as a description of what it
will do. Resist it. A well bucket declares:

    Wait, GimmickOn, Clear, Lock, Deactive, MinAngle, PreGimmickOn, PreWait

and a peony declares the same core set, `GimmickOn`, `Lock`, `Deactive`,
`Clear`. A peony is an ordinary collection node that gathering consumes. So
`Clear` does not mean "emptied and refilled"; it is just the state a collection
gimmick enters when it has been taken. Reading it as the former is what led to
two attempts to draw water without losing the bucket, and the bucket behaved
like a picked flower both times because that is what it is.

`RemoteCatchPivot`, `GimmickOnPullOutDirection` and `PullOutDurationTime` look
like a special interaction and are not. They are schema keys present on every
gimmick, a peony included, most of them with no value.

**Diff against a known gimmick before drawing a conclusion from a key or a
state.** peony_01.binarygimmick is the useful control: it is the simplest thing
the mod already handles correctly.

## Traps

Transition ids are Jenkins lookup3 over the lowercase name (`statehash.py`) and
cannot be reversed. `name_events.py` matches forward against every identifier in
the exe and resolves about a third. The rest are nowhere to be found: not in the
exe, and not in the `.binarygimmick` files either, since hashing all twelve well
definitions matched none of the ids seen on a well. Do not repeat that search.
An unnamed id is still usable, because `DriveGimmickEvent` takes the number.

A `[why]` line prints once per entity per changed reason, so the distance in it
is where the object was first seen, not where it is now. A well first seen at
14.5 m still reads 14.5 m with the player standing on it. The Nearby tab's hover
tooltip is the live view, and it is what identified the well bucket as a gather
node when the log could not.

The log banner's build date lives in `mod.cpp`, which only recompiles when its
own dependencies change, so it goes stale while the rest of the plugin moves.
Check a staged binary by searching it for a string the new code added.
