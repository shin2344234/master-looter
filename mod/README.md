# Master Looter plugin

An ASI plugin for Crimson Desert 2.01.00: an auto-looter driven by the tagged item database, with an in-game settings menu.

## Installing

1. Install Ultimate ASI Loader as `winmm.dll` in the game's `bin64` folder if it is not there already (a mod manager such as DMM does this for you).
2. Remove any other auto-loot mod first. Two of them hook the same game functions and the second one to load does nothing.
3. Copy `MasterLooter.asi`, `MasterLooter.items.tsv` and `MasterLooter.creatures.tsv` into `bin64` next to `winmm.dll`, with the game closed.
4. Start the game. Press Insert for the menu; the Status tab shows whether every hook and signature resolved. `MasterLooter.log` next to the plugin says the same in more detail.

Uninstall by deleting the four `MasterLooter.*` files. No game file is modified and nothing is written to a save.

## What is in the box

- `MasterLooter.asi`: the plugin. Draws a Dear ImGui menu over the game through a DirectX 12 present hook (adapted from Trinity, see THIRD_PARTY_NOTICES.md), so it works with DLSS frame generation and HDR.
- `MasterLooter.items.tsv`: the item database (6,813 items with runtime row id, class, tags, tier and sell value), generated from `data/items_tagged.csv` by `scripts/make_itemdb_tsv.py`. Classes and tags come from the group rules in `scripts/build_item_db.py`; the hand-made corrections for items the rules get wrong (legendary fish, salamanders, ores, horse feed) live in `data/class_overrides.csv`.
- `MasterLooter.ini`: written next to the plugin on first run, rewritten whenever a setting changes in the menu, and reloaded within a second if edited by hand while the game runs.
- `MasterLooter.log`: rewritten every launch. Signature resolution, hook installation, the event self test, every item taken and, once per object, why anything within range was skipped.
- `MasterLooter.creatures.tsv`: every creature the player can catch with the class of the item it becomes, generated from the character data by `scripts/make_creatures_tsv.py`. It is how insects, fish, seafood and small animals are told apart; crabs, shrimp, squid, starfish and seahorses count as fish for the Fish toggle, and rows without an item (monsters, mounts) only serve name matching.
- `MasterLooter.learned.tsv`: what each gather node type yields, learned by watching the bag after a gather, yours or the mod's. It is what tells plants, ore, stone and wood apart; delete it or press Forget to relearn.

## Controls

- Insert opens and closes the menu (rebindable under General). Escape also closes it.
- F10 turns auto-loot on and off, F11 loots everything in range once. Both rebindable.
- Home is watch mode: the menu stays on screen but the game keeps every input, so the Status log or the Nearby list can be watched while playing. Home from a closed menu opens it straight into watch mode, Insert makes it interactive again, Home again closes it. The Watch button in the title strip does the same.
- While the menu is open the game does not see the keyboard, mouse or controller. Key releases still pass through so nothing sticks.
- Tabs: General (switches, keys, pace, filters), Looting (one toggle each for ground items, carcasses, plants, ore, stone, wood, unidentified nodes, insects, fish, small animals, containers and furniture nodes; ranges; node arming; ownership), Classes (one-click groups such as Weapons and armor or Food and drink above the full class table, plus quest, unsellable and protected-item switches), Tags, Items (search any item, set an override, see the live verdict), Nearby (every object around you with the rule that decided it), Status (signatures, hooks, event path, counters, recent loot, log).

## How looting works

A worker thread finds the game's actor manager by its RTTI class name, reads every world object around the player (position, components, node data), and decides per object. Decisions that pass are queued and the game-thread pump sends the game's own loot events (pick up, gather, catch, search carcass), exactly as the game does when you press the interaction key. Empty nodes such as ore veins are armed first so the game fills their data without you standing on them.

Rule order for an identified item: item override, tag never, protected tags (memory fragments, mechanism parts), tag always, dev/quest/unsellable filters, copper value floor, class rule, then loot. Built-in protections apply before any of that: quest and shop objects, locked nodes, your own equipment and bag contents, gear worn by others, mechanism parts, container stacks, memory triggers, and anything the game's own Take-or-Steal check calls theft (unless you opt in).

## Safety and known limits

- The mod sends the game the same events the game sends itself when you press the interaction key, and reads memory only to decide what to send. It never writes game memory apart from its own hooks.
- Everything the game would call stealing is skipped unless you opt in under Looting, Ownership. The game puts a bounty on you for it either way.
- Quest items, memory chips, puzzle and mechanism parts, artifacts, recipes and your own equipment are protected by default; the Classes and Items tabs can lift most of that, on purpose, per class or per item.
- A carcass is searched once per session and never again, whatever the retry setting: searching an empty carcass has been seen to duplicate items, and duplicates are how saves get corrupted.
- There is no line-of-sight check. The ranges are distances, not visibility; keep the arming range short or a node behind a thin wall can be gathered.
- Creatures the species table cannot name are only caught when every category they could belong to is on. Bugs are recognised by their model, fish by theirs; a few unusual creatures may stay unidentified.
- Gather nodes are identified by what lands in your bag. A node type the mod has never seen yield anything is left alone until you harvest one by hand or turn Unidentified nodes on.
- Chests and storage boxes open a window rather than hand over an item; they are off by default and rarely respond.

## Compatibility and patches

Every game address comes from a byte pattern or an RTTI name resolved at load; nothing is hardcoded. After a game patch run `py -3 scripts\sigcheck.py` against the new exe: it reports which patterns in `src/loot/signatures.h` still match uniquely. The Status tab shows the same list in game.

## Build

Needs Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure (Dear ImGui and MinHook are fetched).

    py -3 ..\scripts\build_item_db.py
    py -3 ..\scripts\make_itemdb_tsv.py
    py -3 ..\scripts\make_creatures_tsv.py
    build.bat

Output lands in `dist\`. Copy `MasterLooter.asi`, `MasterLooter.items.tsv` and `MasterLooter.creatures.tsv` into the game's `bin64\` next to the ASI loader (`winmm.dll`), or into the DMM mods folder, while the game is closed.

## Files

- `src/core`: paths, log, settings (INI load, debounced save, hot reload), item database, rules.
- `src/hooks`: DX12 present hook and swapchain wrapper, window procedure subclass, XInput neutraliser.
- `src/gui`: style, menu key polling, the menu and HUD.
- `src/loot`: signatures, guarded memory and pattern scanning, game structures, the event protocol, MinHook detours, and the engine itself.
- `scripts/adapt_trinity_dx12.py` regenerates the DX12 files from a Trinity checkout; `scripts/sigcheck.py` checks the signatures offline.

## Credits

- Trinity by XeTrinityz (MIT): the DirectX 12 present hook, swapchain wrapper and HDR composite are adapted from it, and its signature-first approach shaped the rest. See THIRD_PARTY_NOTICES.md.
- CDLoot: the reverse engineering behind the loot engine. The event protocol, the entity and component layout, the ownership oracle and node arming were worked out there and are used here by that knowledge, with the code written anew.
- Dear ImGui (MIT) and MinHook (BSD 2-Clause), fetched at build time.
