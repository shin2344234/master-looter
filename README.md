# Master Looter

Auto-loot for Crimson Desert 2.01.00 with an in-game menu.

Walk past it and it is in your bag: dropped items, herbs and flowers, ore and stone chunks, timber, insects, fish, small animals and animal carcasses. Each kind has its own switch. Every item is checked against a database of 6,813 items with classes and tags, and the game's own Take-or-Steal check decides what is off limits. Everything is set from a menu inside the game.

[Releases](https://github.com/shin2344234/master-looter/releases) · [Plugin manual](mod/README.md) · [Data pipeline](scripts/README.md) · [Nexus description](docs/nexus-description.bbcode)

## What it does

- Twelve switches for what to collect: ground items, carcasses, plants, ore, stone, wood, unidentified nodes, insects, fish, small animals, containers and furniture nodes.
- Class groups with one click (weapons and armor, damaged gear, food and drink, materials, books and papers, furniture, treasure and keepsakes, and more), a full class table, tag rules and per-item overrides with a live verdict.
- Quest items, memory chips, puzzle and mechanism parts, artifacts, recipes and your own equipment are protected by default. The Classes and Items tabs can lift that on purpose.
- Owned goods are skipped unless you opt in: the mod asks the same routine the game uses to decide between "Take" and "Steal".
- Gather nodes are armed from a distance, so bushes fill their data without you standing on them.
- Presets: every setting and every rule saved under a name, swapped in two clicks. Settings are also backed up each time the game starts and the last twelve are kept, so a version with different defaults is one button to undo. Anything that overwrites or discards asks first.
- A notice appears when the bag is full, read from the bag's own slot count, so it shows the moment it fills whether or not you are looting. Turn it off under General.
- A Nearby tab lists every object around you with the rule that decided it; a Status tab shows every hook and signature.
- Watch mode keeps the menu on screen while you play, and every key is rebindable. The overlay works with DLSS frame generation and HDR.
- Nothing hardcoded: every game address comes from a byte pattern or a class name resolved at load, and `sigcheck.py` reports what a game patch broke without launching the game.

## Installing

### With Definitive Mod Manager

Import `MasterLooter-<version>-DMM.zip` from the [releases](https://github.com/shin2344234/master-looter/releases) (drag it onto the DMM window). DMM registers `MasterLooter.asi` as an ASI add-on, deploys it with its own loader and removes it on uninstall. Disable any other auto-loot mod first: two of them hook the same game functions and the second one to load does nothing.

### By hand

Ultimate ASI Loader (`winmm.dll`) must be in the game's `bin64` folder. Copy `MasterLooter.asi` from `MasterLooter-<version>.zip` into `bin64` next to it while the game is closed. The item database and the creature table are compiled into the plugin. Start the game and press Insert.

Uninstall by deleting the `MasterLooter.*` files and folders from `bin64`. The plugin writes `MasterLooter.ini` and `MasterLooter.log` next to itself, plus `MasterLooter.presets` and `MasterLooter.backups`; no game file is modified and nothing is written to a save.

## Controls

- Insert opens and closes the menu. Escape also closes it.
- F10 turns auto-loot on and off. F11 loots everything in range once.
- Home is watch mode: the menu stays up while the game keeps every input. Insert makes it interactive again.
- While the menu is interactive the game does not see the keyboard, mouse or controller. Key releases still pass so nothing sticks.

## How it decides

A worker thread finds the game's actor manager by its RTTI class name and reads every world object around the player. Each object gets a verdict of its own. Verdicts that pass are queued, and a hook on the game's own per-frame tick sends the game's own loot events (pick up, gather, catch, search carcass), the same events the game sends when you press the interaction key.

Rule order for an identified item: item override, tag never, protected tags (memory fragments, mechanism parts), tag always, dev/quest/unsellable filters, copper value floor, class rule, then loot. Built-in protections apply before any of that: quest and shop objects, locked nodes, your own equipment and bag contents, gear worn by others, mechanism parts, container stacks, memory triggers, and anything the game's Take-or-Steal check calls theft. The [plugin manual](mod/README.md) has the details, the safety notes and the known limits.

## Building

Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure, which fetches Dear ImGui and MinHook.

    cd mod
    build.bat
    py -3 scripts\package.py

`dist\` receives the plugin, the documents and the two release archives. The item database and the creature table in `mod\data` are current for 2.01.00 and are compiled into the plugin; regenerating them after a game patch is described in [scripts/README.md](scripts/README.md). After a patch, `py -3 mod\scripts\sigcheck.py` reports which signatures still resolve against the installed exe.

## Repository layout

- `mod/`: the plugin. `src/core` (paths, log, settings, item and creature databases, rules), `src/hooks` (DirectX 12 present hook and swapchain wrapper, window procedure, XInput), `src/gui` (menu), `src/loot` (signatures, guarded memory access, game structures, event protocol, hooks, engine), `data/` (the two tables), `scripts/` (sigcheck, packaging, the Trinity adaptation script).
- `scripts/`: the data pipeline that parses the game tables and tags every item.
- `data/`: the hand-made class overrides and the pipeline summaries. The full generated dumps stay local; see the pipeline README.
- `docs/`: the Nexus Mods description.

## Licence and credits

MIT, see [LICENSE](LICENSE). Third-party terms are in [mod/THIRD_PARTY_NOTICES.md](mod/THIRD_PARTY_NOTICES.md).

- Trinity by XeTrinityz (MIT): the DirectX 12 present hook, swapchain wrapper and HDR compositing are adapted from it.
- CDLoot: the reverse engineering of the loot event protocol, the entity layout, the ownership check and node arming. Used as knowledge; the code here is new.
- Dear ImGui (MIT) and MinHook (BSD 2-Clause), fetched at build time. Ultimate ASI Loader, which every ASI mod depends on.
