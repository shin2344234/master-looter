# Master Looter plugin

An ASI plugin for Crimson Desert 2.01.00: an auto-looter driven by the tagged item database, with an in-game settings menu.

## Installing

Downloads are on [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3402) and in the [GitHub releases](https://github.com/shin2344234/master-looter/releases).

With Definitive Mod Manager (DMM): import `MasterLooter-<version>-DMM.zip` (drag it onto the DMM window). DMM registers `MasterLooter.asi` as an ASI add-on, deploys it with its own loader and removes it again on uninstall. Disable any other auto-loot mod first: two of them hook the same game functions and the second one to load does nothing.

By hand:

1. Ultimate ASI Loader (`winmm.dll`) must be in the game's `bin64` folder.
2. Copy `MasterLooter.asi` into `bin64` next to `winmm.dll`, with the game closed. The item database and the creature table are compiled into the plugin.
3. Start the game. Press Insert for the menu; the Status tab shows whether every hook and signature resolved. `MasterLooter.log` next to the plugin says the same in more detail.

Uninstall by deleting the `MasterLooter.*` files and folders from `bin64` (the plugin writes `MasterLooter.ini` and `MasterLooter.log` next to itself, plus `MasterLooter.presets` and `MasterLooter.backups`). No game file is modified and nothing is written to a save.

## What is in the box

- `MasterLooter.asi`: the plugin. Draws a Dear ImGui menu over the game through a DirectX 12 present hook (adapted from Trinity, see THIRD_PARTY_NOTICES.md), so it works with DLSS frame generation and HDR.
- `MasterLooter.items.tsv`: the item database, compiled into the plugin (a copy next to the plugin overrides the built-in one, for trying a regenerated table): 6,813 items with runtime row id, class, tags, tier and sell value, generated from `data/items_tagged.csv` by `scripts/make_itemdb_tsv.py`. Classes and tags come from the group rules in `scripts/build_item_db.py`; the hand-made corrections for items the rules get wrong (legendary fish, salamanders, ores, horse feed) live in `data/class_overrides.csv`.
- `MasterLooter.ini`: written next to the plugin on first run, rewritten whenever a setting changes in the menu, and reloaded within a second if edited by hand while the game runs.
- `MasterLooter.log`, in the game's `bin64` folder beside the plugin, so `...\steamapps\common\Crimson Desert\bin64\MasterLooter.log` on a Steam install. The Status tab prints the full path with a button that opens the folder. It is created as the plugin loads rather than at the first frame, so a crash before anything is drawn still leaves one. Rewritten every launch. Signature resolution, hook installation, the event self test, every item taken and, once per object, why anything within range was skipped.
- `MasterLooter.creatures.tsv`: every creature the player can catch with the class of the item it becomes, compiled into the plugin the same way, generated from the character data by `scripts/make_creatures_tsv.py`. It is how insects, fish, seafood and small animals are told apart; crabs, shrimp, squid, starfish and seahorses count as fish for the Fish toggle, and rows without an item (monsters, mounts) only serve name matching.
- The gather node table, compiled in, is what tells plants, ore, stone and wood apart. A node is identified by the prefab it was placed from, which is the only name it keeps between sessions. Nodes the table does not cover fall back to watching the bag after a gather, yours or the mod's, and that part is forgotten when you quit.

## Controls

- Insert opens and closes the menu (rebindable under General). Escape also closes it.
- F10 turns auto-loot on and off, F11 loots everything in range once. Both rebindable.
- Each of the four also takes a controller shortcut of two buttons at once, never one, because every single button already does something in this game. The mod only reads the pad, so the game still sees both buttons: pick a pair that does nothing together, such as the two shoulder buttons, or Back and a face button.
- Home is watch mode: the menu stays on screen but the game keeps every input, so the Status log or the Nearby list can be watched while playing. Home from a closed menu opens it straight into watch mode. Insert then makes it interactive, and a second Home closes it. The Watch button in the title strip does the same.
- While the menu is open the game does not see the keyboard, mouse or controller. Key releases still pass through so nothing sticks.
- Tabs: General (switches, keys, pace, filters), Looting (one toggle each for ground items, carcasses, plants, ore, stone, wood, unidentified nodes, insects, fish, small animals, containers and furniture nodes; ranges; node arming; ownership), Classes (one-click groups such as Weapons and armor or Food and drink above the full class table, plus quest, unsellable and protected-item switches), Tags, Items (search any item, set an override, see the live verdict), Nearby (the objects around you with the rule that decided each one, nearest first, up to 96 rows, leaving out what you are wearing and carrying), Status (signatures, hooks, event path, counters, recent loot, log).

## How looting works

A worker thread finds the game's actor manager by its RTTI class name and reads every world object around the player (position, components, node data). Each object gets its own verdict. Verdicts that pass are queued and the game-thread pump sends the game's own loot events (pick up, gather, catch, search carcass), exactly as the game does when you press the interaction key. Empty nodes such as ore veins are armed first so the game fills their data without you standing on them.

Ore is the exception to the loot events. A gather lifts the ore straight out of the node and skips the drop, and the drop is the only place the game applies an equipped tool's Mining Yield Up, so a gathered vein pays the same whatever pickaxe you carry. The mod therefore breaks a vein instead, sending the game the same break it raises for itself when you swing: the contents spill on the ground and are picked up from there, with the tool bonus applied by the game rather than imitated. Each vein is struck once and then left alone until the game respawns it.

Rule order for an identified item: item override, tag never, protected tags (memory fragments, mechanism parts), tag always, dev/quest/unsellable filters, copper value floor, class rule, then loot. Built-in protections apply before any of that: quest and shop objects, locked nodes, your own equipment and bag contents, gear worn by others, mechanism parts, container stacks, memory triggers, and anything the game's own Take-or-Steal check calls theft (unless you opt in).

## When the game will not start

Several ASI mods drawing an overlay end up on the same DirectX code, and the order the loader picks decides whether they survive each other. The log names who owns each address before this mod touches it, on the first few lines. On a clean install every one reads `dxgi.dll` or `d3d12.dll`; anything else is another mod already there, and the line is written as an error so it is easy to find.

If the game will not launch alongside another overlay mod, set `WrapSwapChain=0` in `MasterLooter.ini`, which can be edited without starting the game. That leaves the swapchain alone and draws the overlay through the present hook instead. The only thing lost is the overlay while DLSS frame generation is on. The same switch is in the Status tab.

## Translating the menu

The menu ships in 28 languages besides English, the set Steam itself offers, all built into the plugin so they survive a mod manager that deploys the .asi on its own. Each has a button under General, Language. Three were done by people: Brazilian Portuguese by Kyo-70, Simplified and Traditional Chinese by dofo7777. The other 25 came out of machine translation and nobody who speaks them has checked them yet, which the menu says whenever one is selected. A wrong word in one of those is worth reporting, as an issue or as a corrected file.

A language not in that set, or a correction to one that is, is a text file. Every English string is its own key, so a translation covering half the menu leaves the other half in English rather than showing gaps.

1. Take [docs/MasterLooter.template.txt](../docs/MasterLooter.template.txt), which holds all 305 strings. It is generated from the source by `scripts/make_translation_template.py`, so it covers the whole menu.
2. Or make your own from the running game: play with the menu open, visit every tab, then General, Language, press `Write translation template`. That writes the file beside the plugin, but only the lines that have actually been drawn.
3. Each record is the English, a tab, then your translation. `
` is a line break, lines starting with `#` are ignored, and a record left empty after the tab stays English.
4. Keep every `%d`, `%s` and `%.1f` exactly as they appear and in the same order. They are replaced with numbers and names at runtime, and a line that changes them is refused at load rather than risked, since it would read the wrong values.
5. Save it as `MasterLooter.<language>.txt`, for example `MasterLooter.de.txt`, and put `de` in the Language box, or `Language=de` in `MasterLooter.ini`.

A file next to the plugin is read in preference to the copy inside it, so any shipped translation can be corrected, and a translation in progress can be reloaded with the Language button without a rebuild or a restart. `scripts/check_translations.py` holds every shipped file up against the template and says what is missing, what has gone stale, and what the plugin would refuse.

Send a finished file in and it can ship with the mod, credited.

## Safety and known limits

- The mod sends the game the same events the game sends itself when you press the interaction key, and reads memory only to decide what to send. It never writes game memory apart from its own hooks.
- Everything the game would call stealing is skipped unless you opt in under Looting, Ownership. The game puts a bounty on you for it either way.
- Quest items, memory chips, puzzle and mechanism parts, artifacts, recipes and your own equipment are protected by default; the Classes and Items tabs can lift most of that, on purpose, per class or per item.
- A carcass is searched once per session and never again, whatever the retry setting: searching an empty carcass has been seen to duplicate items, and duplicates are how saves get corrupted.
- There is no line-of-sight check. The ranges are distances, not visibility; keep the arming range short or a node behind a thin wall can be gathered.
- Creatures are named from the game's own data, not from their model name. A creature actor carries its row in the character table at `status+0x30`, which the mod resolves to a string key and looks up in the creature table, so it knows a Tench from a Ricefish before deciding. That means a per-item override refuses a catch: set `Item_Tench` to never on the Items tab and tenches are left alone while other fish are still taken. Model-name matching remains as the fallback for the few this cannot name, and those are caught by category only.
- Gather nodes are identified by their prefab, matched against a table of 966 node kinds built from the game's own data. A node outside that table is identified by what lands in your bag when it is gathered, and is otherwise left alone until you turn Unidentified nodes on.
- Chests and storage boxes open a window rather than hand over an item; they are off by default and rarely respond.
- How full the bag is is read from the bag itself. The inventory holder carries every store the player owns, 18 buckets on 2.01.00, of which bucket 0 is what you carry; its used count and its limit sit side by side as two u16 fields. Both are checked for sense before use, and the notice appears the moment the bag fills whether or not you are looting.
- The limit was read off one bag that never grew, so a bag expansion has never been watched. If that field turns out to be a base figure that does not move, an item arriving while the bag reads as full proves the limit is higher than it says, and the mod corrects itself and carries on. The correction is dropped as soon as the field itself moves.
- When those fields stop reading sensibly, after a game patch moves them, the mod falls back to inferring it: an item that is coming arrives in well under a second, so a pick-up that has reached nothing after 1.2 seconds counts against it and three in a row raise the notice, which one landing clears. Only pick-ups of items the database can name count, since an empty carcass, a node that gives nothing and an unnamed item would all read as a full bag otherwise. `DumpInventoryShape` in `loot/game.cpp` is what found the two fields and is how to find them again.

## Compatibility and patches

Every game address comes from a byte pattern or an RTTI name resolved at load; nothing is hardcoded. After a game patch run `py -3 scripts\sigcheck.py` against the new exe: it reports which patterns in `src/loot/signatures.h` still match uniquely. The Status tab shows the same list in game.

## Build

Needs Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure (Dear ImGui and MinHook are fetched).

    py -3 ..\scripts\build_item_db.py
    py -3 ..\scripts\make_itemdb_tsv.py
    py -3 ..\scripts\make_creatures_tsv.py
    build.bat
    py -3 scripts\package.py

Output lands in `dist\`, with the README, licence and notices alongside; `package.py` zips it as `MasterLooter-<version>.zip` (plugin and documents) and `MasterLooter-<version>-DMM.zip` (plugin only). Copy `MasterLooter.asi` into the game's `bin64\` next to the ASI loader (`winmm.dll`) while the game is closed, or import the DMM zip. The first two data scripts need the game tables extracted into `extracted\` (see the repository README); the committed `mod\data` TSVs are current for 2.01.00, so a plain `build.bat` is enough to build the plugin.

## Files

- `src/core`: paths, log, settings (INI load, debounced save, hot reload, presets and the session backup), item database, gather node table, creature table, rules.
- Next to the plugin at runtime: `MasterLooter.ini`, `MasterLooter.log`, `MasterLooter.presets\` holding one ini per preset, and `MasterLooter.backups\` holding one dated ini per backup. A backup is written every time the game starts, a migration leaves one of its own, and the last twelve are kept.
- `src/hooks`: DX12 present hook and swapchain wrapper, window procedure subclass, XInput neutraliser.
- `src/gui`: style, menu key polling, the menu and HUD.
- `src/loot`: signatures, guarded memory and pattern scanning, game structures, the event protocol, MinHook detours, and the engine itself.
- `scripts/adapt_trinity_dx12.py` regenerates the DX12 files from a Trinity checkout; `scripts/sigcheck.py` checks the signatures offline; `scripts/package.py` zips a release.

## Licence

MIT, see the LICENSE file in the repository root. Third-party terms are in THIRD_PARTY_NOTICES.md.

## Credits

- Trinity by XeTrinityz (MIT): the DirectX 12 present hook, swapchain wrapper and HDR composite are adapted from it, and its signature-first approach shaped the rest. See THIRD_PARTY_NOTICES.md.
- CDLoot: the reverse engineering behind the loot engine. The event protocol, the entity and component layout, the ownership oracle and node arming were worked out there. This engine uses that knowledge; its code is new.
- Dear ImGui (MIT) and MinHook (BSD 2-Clause), fetched at build time.
- dofo7777: the Simplified and Traditional Chinese menus.
- Kyo-70: the Brazilian Portuguese menu.
