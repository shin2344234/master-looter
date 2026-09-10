# Master Looter

Auto-loot for Crimson Desert 2.01.00 with an in-game menu.

Walk past it and it is in your bag: dropped items, herbs and flowers, ore and stone chunks, timber, insects, fish, small animals and animal carcasses. Each kind has its own switch. Every item it picks up is checked against a database of 6,813 items with classes and tags, and the game's own Take-or-Steal check decides what is off limits. Skinning a carcass is the exception: the game hands the yield over without the mod seeing what it is, so that one switch is all or nothing. Everything is set from a menu inside the game.

[Nexus Mods page](https://www.nexusmods.com/crimsondesert/mods/3402) · [Releases](https://github.com/shin2344234/master-looter/releases) · [Plugin manual](mod/README.md) · [Data pipeline](scripts/README.md)

## What it does

- Fourteen switches for what to collect: ground items, carcasses, plants, crops, ore, stone, wood, unidentified nodes, insects, fish, small animals, containers, furniture nodes and water from wells.
- Class groups with one click (weapons and armor, damaged gear, food and drink, materials, books and papers, furniture, treasure and keepsakes, and more), a full class table, tag rules and per-item overrides with a live verdict.
- Quest items, memory chips, puzzle and mechanism parts, artifacts, recipes and your own equipment are protected by default. The Classes and Items tabs can lift that on purpose.
- Owned goods are skipped unless you opt in: the mod asks the same routine the game uses to decide between "Take" and "Steal".
- Gather nodes are armed from a distance, so bushes fill their data without you standing on them.
- Ore veins are broken rather than emptied, the way a pickaxe does it, so the contents drop on the ground and your tool's Mining Yield Up is applied by the game. Each vein is struck once.
- Water is lifted out of a well bucket while you turn the handle yourself. The mod drives the one state change the game makes for that and nothing else, so the bucket stays on the well and the handle stays in your hands. Off by default.
- Presets: every setting and every rule saved under a name, swapped in two clicks. Settings are also backed up each time the game starts and the last twelve are kept, so a version with different defaults is one button to undo. Anything that overwrites or discards asks first.
- A notice appears when the bag is full, read from the bag's own slot count, so it shows the moment it fills whether or not you are looting. Turn it off under General.
- The menu comes in 28 languages besides English, every language Steam offers, all built into the plugin and picked from one list under General. Brazilian Portuguese is from Kyo-70 and both Chinese menus from dofo7777. The other 25 are machine translations that no native speaker has read yet, and the menu says so whenever one is on. A corrected file beside the plugin overrides the built-in copy, no rebuild needed. The [template of every line](docs/MasterLooter.template.txt) is in the repository, and a partial translation leaves the rest in English.
- A Nearby tab lists the objects around you, nearest first, with the rule that decided each one and a note when there are more than it can show; a Status tab shows every hook and signature.
- Watch mode keeps the menu on screen while you play. Every key is rebindable, and each one can also be a two-button controller shortcut. The overlay works with DLSS frame generation and HDR.
- Nothing hardcoded: every game address comes from a byte pattern or a class name resolved at load, and `sigcheck.py` reports what a game patch broke without launching the game.

## Installing

### With Definitive Mod Manager

Import `MasterLooter-<version>-DMM.zip` from [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3402) or the [releases](https://github.com/shin2344234/master-looter/releases) (drag it onto the DMM window). DMM registers `MasterLooter.asi` as an ASI add-on, deploys it with its own loader and removes it on uninstall. Disable any other auto-loot mod first: two of them hook the same game functions and the second one to load does nothing.

### By hand

Ultimate ASI Loader (`winmm.dll`) must be in the game's `bin64` folder. Copy `MasterLooter.asi` from `MasterLooter-<version>.zip` into `bin64` next to it while the game is closed. The item database and the creature table are compiled into the plugin. Start the game and press Insert.

Uninstall by deleting the `MasterLooter.*` files and folders from `bin64`. The plugin writes `MasterLooter.ini` and `MasterLooter.log` next to itself, plus the eleven previous logs as `MasterLooter.01.log` through `MasterLooter.11.log`, `MasterLooter.presets` and `MasterLooter.backups`; no game file is modified and nothing is written to a save.

## Antivirus

Three scanners out of seventy flag `MasterLooter.asi`: Microsoft (`Trojan:Win32/Wacatac.B!ml`), Symantec (`ML.Attribute.HighConfidence`) and Webroot (`Win.Hacktool.Gen`). Two of them say what they are on the label, since `!ml` and `ML.Attribute` are both how a model reports a guess about the shape of a file. Webroot's label is its generic name for anything shaped like a trainer. None of the three matched a known family, and the sixty-seven others read the same file as clean. Webroot is also the only scanner to object to the archives, one of sixty-seven on each, for the same file inside them.

The number moves release to release without the code changing character: 1.2.1 scored four, 1.4.0 four, 1.5.0 two, 1.5.1 four, 1.6.0 two, 1.6.1 three, 1.6.2 three, 1.6.3 three, 1.6.4 four, 1.6.5 four, 1.6.6 four, 1.6.7 three. Microsoft and Symantec have flagged every one of them. Everything else comes and goes: Deep Instinct has left, CrowdStrike Falcon stayed three releases and dropped off on 1.6.6, Cynet appeared on 1.6.4, was gone by 1.6.5, came back on 1.6.6 and is gone again on 1.6.7, and Webroot arrived on 1.6.5, a build whose one change is that it no longer creates a Direct3D device at startup. That is what a model guessing looks like, as against a scanner recognising something.

Reports for 1.6.7: [the plugin](https://www.virustotal.com/gui/file/95f54c7e753374ea5a4278e77cf4aa068d887de9f3a32f3427470ded1fae1770) and [the package](https://www.virustotal.com/gui/file/66ddf0183d878ec9c63ae01c45b2daebe2113bf03a48d11a16472f21875cb239).

The guess is easy to explain. The plugin is an unsigned DLL that a loader puts inside the game, and once there it rewrites instructions in memory, searches the game's code for byte patterns, reads the keyboard before the game does and draws over Direct3D 12. A trainer does the same things, so a model trained on trainers answers trainer. Nothing about the file argues back: it carries no code signing certificate, and a release a day old has no install history behind it. Signing it is likely at some point, and an unsigned binary with no history is the single biggest thing these models react to, so a certificate should quiet most of this down.

What it does not do is reach the network. It imports no networking library, and the entire import list is `d3d12`, `dxgi`, `imm32`, `xinput9_1_0`, `kernel32`, `user32`, `gdi32`, `shell32` and `d3dcompiler_47`. It writes `MasterLooter.ini`, `MasterLooter.log` and its eleven rotated predecessors `MasterLooter.01.log` to `MasterLooter.11.log`, `MasterLooter.presets` and `MasterLooter.backups` beside itself and nothing else, reads and writes no registry key, and installs nothing that outlives the game process. Every line is in this repository, and `build.bat` will produce the file for you if you would rather not trust mine.

If Defender or your browser quarantines the download, restore it and exclude the game's `bin64` folder, or build from source and use your own binary. SHA-256 for 1.6.7:

    66ddf0183d878ec9c63ae01c45b2daebe2113bf03a48d11a16472f21875cb239  MasterLooter-1.6.7-DMM.zip
    60aada804a781c654792c341710ddcaae01d92b61827f7f2498fb14d69aa69f1  MasterLooter-1.6.7.zip
    f9e2d6c5b9bde0cd96401312e846019e97be1f39392b3f5c3318abd1c25049e6  MasterLooter.asi

## Controls

- Insert opens and closes the menu. Escape also closes it.
- F10 turns auto-loot on and off. F11 loots everything in range once.
- Home is watch mode: the menu stays up while the game keeps every input. Insert makes it interactive again.
- While the menu is interactive the game does not see the keyboard, mouse or controller. Key releases still pass so nothing sticks.

All four keys are rebindable in the menu under General, and they also sit in `MasterLooter.ini` beside the plugin as `MenuKey`, `KeyToggle`, `KeyBurst` and `KeyWatch`, written as virtual-key codes. Edit the file and save it and the change is picked up about a second later without a restart, which is the quickest way to clear a clash with another ASI mod. Each of the four also takes a controller shortcut of two buttons at once rather than one, since every single button already does something in this game.

If the menu opens but will not take a click, the cursor is the usual cause and not the menu. In borderless windowed mode the Windows pointer and the game's own pointer drift apart, so clicks land somewhere other than where you are pointing. Move the pointer to the top left corner once and the two snap back together. Thanks to LuxDragon for working that one out.

## How it decides

A worker thread finds the game's actor manager by its RTTI class name and reads every world object around the player. Each object gets a verdict of its own.

It works as Kliff, Damiane or Oongka. Playing anyone but Kliff, the actor the game raises your events under is not the character you are steering: it sits at a fixed spot with nothing near it. The mod finds the body you are steering by the gear parented to it and searches from there, so nobody has to be in the party. Verdicts that pass are queued, and a hook on the game's own per-frame tick sends the game's own loot events (pick up, gather, catch, search carcass), the same events the game sends when you press the interaction key.

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
- `docs/`: the [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3402) page text, header images, changelogs and the translation template.

## Licence and credits

MIT, see [LICENSE](LICENSE). Third-party terms are in [mod/THIRD_PARTY_NOTICES.md](mod/THIRD_PARTY_NOTICES.md).

- Trinity by XeTrinityz (MIT): the DirectX 12 present hook, swapchain wrapper and HDR compositing are adapted from it.
- CDLoot: the reverse engineering of the loot event protocol, the entity layout, the ownership check and node arming. Used as knowledge; the code here is new.
- Dear ImGui (MIT) and MinHook (BSD 2-Clause), fetched at build time. Ultimate ASI Loader, which every ASI mod depends on.
- dofo7777: the Simplified and Traditional Chinese menus, both done from the template within an hour of it being posted.
- Kyo-70: the Brazilian Portuguese menu.
- Lyntear and Fyreon87, who reported the thorn vines and named the puzzle they were blocking.
- Cmz4455 and Proud Wingman, whose logs showed the scan leaving Damiane for a ship, and LuxDragon, who ran three test builds in one evening to get Crimson Route and this mod drawing together.

## Discord and Patreon

Discord: [Shin234's Mods 'n Stuff](https://discord.gg/AZ2ztQYy74), for questions and for watching what is in progress. Bugs are still best filed as issues on this repo so they get tracked.

Patreon: [patreon.com/cw/Shin234](https://www.patreon.com/cw/Shin234), with the posts at [patreon.com/cw/Shin234/posts](https://www.patreon.com/cw/Shin234/posts) since the new page layout buries them. Everything published stays free, and nothing is held back for it.
