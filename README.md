# Master Looter

Auto-loot for Crimson Desert 2.03.00 with an in-game menu.

> [!IMPORTANT]
> **New in 1.6.28: loot can go straight into storage.** Install [Private Storage Master](https://www.nexusmods.com/crimsondesert/mods/3521) 1.1.0 or later beside Master Looter, and what Master Looter picks up can be moved into your storage as it lands. Nothing you pick up by hand is touched. It starts off; see [Storing loot](#storing-loot) for how to turn it on.

Walk past it and it is in your bag: dropped items, herbs and flowers, ore and stone chunks, timber, insects, fish, small animals, and the carcasses and bodies of anything you kill. Each kind has its own switch. Every item it picks up is checked against a database of 6,816 items with classes and tags, and the game's own Take-or-Steal check decides what is off limits. Searching a body or skinning a carcass is the exception. The game hands over the whole yield before the mod sees any of it, so what your rules refuse arrives anyway unless **Drop refused loot from bodies** under General is on, and then it goes straight back on the ground beside you. Everything is set from a menu inside the game.

[Nexus Mods page](https://www.nexusmods.com/crimsondesert/mods/3402) · [Releases](https://github.com/shin2344234/master-looter/releases) · [Plugin manual](mod/README.md) · [Data pipeline](scripts/README.md)

## What it does

- Fourteen switches for what to collect: ground items, carcasses, enemy bodies, plants, crops, ore and stone, wood, unidentified nodes, insects, fish, small animals, containers, furniture nodes and water from wells. To keep the ore and leave the stone, refuse the class stone on the Classes tab.
- Class groups with one click (weapons and armor, damaged gear, food and drink, materials, books and papers, furniture, treasure and keepsakes, and more), a full class table, tag rules and per-item overrides with a live verdict.
- Quest items, memory chips, puzzle and mechanism parts, artifacts, recipes and your own equipment are protected by default. The Classes and Items tabs can lift that on purpose.
- With [Private Storage Master](https://www.nexusmods.com/crimsondesert/mods/3521) 1.1.0 or later installed, what the mod picks up can be moved into your storage as it lands. See [below](#storing-loot).
- With [Stack Master](https://www.nexusmods.com/crimsondesert/mods/3548) or Private Storage Master 1.1.2 or later installed, the Stacks tab sets how much more every stackable item holds. See [Bigger item stacks](#bigger-item-stacks).
- Owned goods are skipped unless you opt in: the mod asks the same routine the game uses to decide between "Take" and "Steal".
- Gather nodes are armed from a distance, so bushes fill their data without you standing on them.
- Ore veins are broken rather than emptied, the way a pickaxe does it, so the contents drop on the ground and your tool's Mining Yield Up is applied by the game. Each vein is struck once.
- Water is lifted out of a well bucket while you turn the handle yourself. The mod drives the one state change the game makes for that and nothing else, so the bucket stays on the well and the handle stays in your hands. Off by default.
- Presets: every setting and every rule saved under a name, swapped in two clicks. Settings are also backed up when the game starts, whenever they changed since the last backup, and the last twelve are kept, so a version with different defaults is one button to undo. Anything that overwrites or discards asks first.
- A notice appears when the bag is full, read from the bag's own slot count, so it shows the moment it fills whether or not you are looting. Turn it off under General.
- The menu comes in 28 languages besides English, every language Steam offers, all built into the plugin and picked from one list under General. Brazilian Portuguese is from Kyo-70 and both Chinese menus from dofo7777. The other 25 are machine translations that no native speaker has read yet, and the menu says so whenever one is on. A corrected file beside the plugin overrides the built-in copy, no rebuild needed. The [template of every line](docs/MasterLooter.template.txt) is in the repository, and a partial translation leaves the rest in English.
- A Nearby tab lists the objects around you, nearest first, with the rule that decided each one and a note when there are more than it can show; a Status tab shows every hook and signature.
- Watch mode keeps the menu on screen while you play. Every key is rebindable, and each one can also be a two-button controller shortcut. The overlay works with DLSS frame generation and HDR.
- Nothing hardcoded: every game address comes from a byte pattern or a class name resolved at load, and `sigcheck.py` reports what a game patch broke without launching the game.

## Storing loot

New in 1.6.28. With [Private Storage Master](https://www.nexusmods.com/crimsondesert/mods/3521) installed beside it, whatever Master Looter picks up can be moved into your storage a moment after it lands in the bag, the same move you would make standing at the storage.

You need Master Looter 1.6.28 or later and Private Storage Master 1.1.0 or later, both in `bin64`. Playing as Damiane or Oongka takes Master Looter 1.6.29 and Private Storage Master 1.1.1 or later, because neither of them carries a bag of their own and the game fills the one it lends them. Master Looter only stores what it picks up itself, so it has to be looting: auto-loot on, or the loot-everything key.

To turn it on:

1. Press Insert to open the menu and go to the Storage tab. The tab is only there when Private Storage Master is installed.
2. Under Store loot, tick **Put what Master Looter picks up into storage**. It starts off. Setting `AutoStore=1` in `PrivateStorageMaster.ini` does the same.
3. Tick the storages that may receive loot. Each item goes to the first ticked storage in the list that takes it, in this order: Collectibles Chest, Abyss gear storage, Gatherables Chest, Kuku Cooler, Bird Feed, Camp Straw, Wardrobe and Private Storage. Wardrobe and Private Storage start unticked. Camp Provisions never receives loot.
4. If you like, add items to the **Never move** list under the checkboxes. Type three or more letters of a name and pick from the matches. Every currency is on it from the start, and since Private Storage Master 1.1.4 the Arrow is too.

**Only move what was picked up** is on by default. It moves just the amount that arrived, so the food and potions you were already carrying stay in the bag. Turned off, the whole stack goes.

What gets moved is what Master Looter itself took: auto-loot pickups, body and carcass searches, catches and gathers. A body or a carcass hands over everything it holds, so from a search only what your item rules allow is moved, and anything they refuse stays in the bag. What you pick up or gather by hand stays in the bag, and so does anything that arrives in a shop, a crafting screen, a menu or while a storage is open, which covers purchases, crafts and withdrawals. Keys, quest items and documents are never moved, and neither is anything on the never-move list, so a key a body drops stays in your bag. An item no ticked storage takes stays in the bag. Two short windows lean the same way: for twelve seconds after you take something by hand that the mod can't identify, and for about two seconds after you close a menu or a storage, nothing is stored.

**Store what your pet loots from bodies**, off by default, sends what a pet or a companion takes from a body to storage as well, as long as your item rules allow it. The game reports a pet picking up a loose item as your own pickup, so those stay in the bag.

Each batch shows a notice such as `Master Looter: Stored 3 items: Kuku Cooler 3`, at most one a second. **Show a notice when loot is stored**, just under the main switch, turns it off. With the verbose log on, every item offered and where it went is written to `MasterLooter.log` as a `[store]` line, along with the reason when one stayed in the bag.

With an older Private Storage Master the section reads "Storing loot needs a newer Private Storage Master." If Private Storage Master can't find the game's move on your game version, the section says so and the switches are greyed out.

## Bigger item stacks

New in 1.6.33. The Stacks tab sets how much more every stackable item holds, and it appears only when a mod that can do that is installed beside Master Looter: Stack Master, or Private Storage Master 1.1.2 or later. Master Looter changes nothing itself; the tab is a front end for whichever of those is installed, and with both there Stack Master does the work.

Pick Off, x2, x3, x5, x10, x20, x50 or x100. Most things the game stacks 100 of, so x10 holds 1000, and the deepest stacks in the game stop at the ceiling that mod reports, 999999 today. What you pick goes into that mod's own ini at once. A bigger multiplier than the one running takes hold immediately where that mod can manage it, which needs Private Storage Master loaded and you out in the world rather than in a storage screen. A smaller one always waits for the next launch, because a stack you already built cannot be shrunk under what is in it. The tab says which of the two you are looking at, and shows a restart line only when one is really needed.

The tab also shows which mod is applying the multiplier, how many item records were raised, the largest limit written, and, when nothing is being changed, that mod's own sentence saying why.

**Empty your big stacks before turning the multiplier down or off.** Lowering it does not shrink a stack you already built. A slot holding more than the game now allows keeps what is in it until you take some out, and anything above the new limit can be lost.

## Installing

**1.6.15 needs the 2.02.00 game update.** The tables are built from that
build's data and one of the functions the plugin hooks moved in the patch.
If the game is still on 2.01.00, use 1.6.10 from
[Releases](https://github.com/shin2344234/master-looter/releases) instead.

### With Definitive Mod Manager

Import `MasterLooter-<version>-DMM.zip` from [Nexus Mods](https://www.nexusmods.com/crimsondesert/mods/3402) or the [releases](https://github.com/shin2344234/master-looter/releases) (drag it onto the DMM window). DMM registers `MasterLooter.asi` as an ASI add-on, deploys it with its own loader and removes it on uninstall. Disable any other auto-loot mod first: two of them hook the same game functions and the second one to load does nothing.

### By hand

Ultimate ASI Loader (`winmm.dll`) must be in the game's `bin64` folder. Copy `MasterLooter.asi` from `MasterLooter-<version>.zip` into `bin64` next to it while the game is closed. The item database and the creature table are compiled into the plugin. Start the game and press Insert.

### When another mod ships its own `winmm.dll`

OptiScaler, some frame generation builds and some ReShade setups install a `winmm.dll` of their own. That file proxies winmm for that mod and loads no ASI plugins, so putting it in place of the loader means this mod and every other `.asi` never start. Nothing in the log says so, because there is no log: the plugin was never injected.

The two can share the folder on different names, because Ultimate ASI Loader answers to `version.dll` and `dinput8.dll` as well as `winmm.dll`, and `version.dll` is known to load on this game. Leave the other mod's `winmm.dll` where it is, rename the loader to `version.dll` in the same `bin64`, launch, and look for `MasterLooter.log` in `bin64`; that file appearing is the whole test. Afterwards, DMM may put its own `winmm.dll` back on the next Mount. That leaves two loaders in the folder, DMM's own Diagnose will say so, and the one to keep is whichever the other mod is not using.

### Linux

The plugin runs under Proton since 1.6.12, looting and menu both, with the same loader and the same files and nothing to set. A report from Linux is easiest to read with the Proton log beside `MasterLooter.log`; `PROTON_LOG=1` in the game's launch options writes it to `~/steam-3321460.log`.

Uninstall by deleting the `MasterLooter.*` files and folders from `bin64`. The plugin writes `MasterLooter.ini` and `MasterLooter.log` next to itself, plus the eleven previous logs as `MasterLooter.01.log` through `MasterLooter.11.log`, `MasterLooter.presets` and `MasterLooter.backups`; no game file is modified and nothing is written to a save.

## Antivirus

One scanner out of seventy-one flags `MasterLooter.asi` on 1.6.42. Webroot calls it `Win.Hacktool.Gen`, its generic name for anything shaped like a trainer, and has since 1.6.5. Microsoft's `Trojan:Win32/Wacatac.C!ml` guess, back for 1.6.25, left on 1.6.26 and has stayed away since; the `!ml` is a model reporting the shape of a file, not a match on anything. Deep Instinct, which reports nothing but `MALICIOUS`, flagged the early builds, stopped some time before 1.6.8, objected to 1.6.33 and 1.6.34, and has gone again. Symantec and Cynet stopped at the first signed build and have not come back. Webroot objects to the archives as well, one of sixty-seven, for the same file inside them.

The number moves release to release without the code changing character: 1.2.1 scored four, 1.4.0 four, 1.5.0 two, 1.5.1 four, 1.6.0 two, 1.6.1 three, 1.6.2 three, 1.6.3 three, 1.6.4 four, 1.6.5 four, 1.6.6 four, 1.6.7 three, 1.6.8 four, 1.6.9 four, 1.6.10, the first signed build, two, 1.6.12 one, 1.6.13 one, 1.6.14 one, 1.6.15 two, 1.6.16 two, 1.6.17 one, 1.6.18 two, 1.6.19 two, 1.6.20 one, 1.6.21 one, 1.6.22 one, 1.6.23 one, 1.6.24 one, 1.6.25 two, 1.6.26 one, 1.6.27 one, 1.6.28 one, 1.6.29 one, 1.6.30 one, 1.6.31 one, 1.6.32 one, 1.6.33 two, 1.6.34 two, 1.6.35 one, 1.6.36 one, 1.6.37 one, 1.6.38 one, 1.6.39 one, 1.6.40 one, 1.6.41 one and 1.6.42 one. Microsoft flagged every one of them up to the signature, went quiet for five releases, came back for two, dropped off for five more, returned on 1.6.25 and left again on 1.6.26; Symantec flagged every unsigned build and none since. Everything else comes and goes: Deep Instinct flagged early builds, went quiet, came back for 1.6.33 and 1.6.34 and left on 1.6.35, CrowdStrike Falcon stayed three releases and dropped off on 1.6.6, Cynet appeared on 1.6.4, was gone by 1.6.5, came back on 1.6.6, left on 1.6.7, returned on 1.6.9 and left again with the signature, Webroot arrived on 1.6.5, a build whose one change is that it no longer creates a Direct3D device at startup, and Elastic arrived on 1.6.8 and was gone again on 1.6.9. That is what a model guessing looks like, as against a scanner recognising something.

Reports for 1.6.42: [the plugin](https://www.virustotal.com/gui/file/b244686089c563809d741d3834a51510ee8f69b9aabc38f9480605ff15516774) and [the package](https://www.virustotal.com/gui/file/0220194140863d76f4b2e5708422870917b4e7af71812a1f642f3387b4bcd737).

The guess is easy to explain. The plugin is a DLL that a loader puts inside the game, and once there it rewrites instructions in memory, searches the game's code for byte patterns, reads the keyboard before the game does and draws over Direct3D 12. A trainer does the same things, so a model trained on trainers answers trainer, and a release a day old has no install history to argue back with.

What it does not do is reach the network. It imports no networking library, and the entire import list is `d3d12`, `dxgi`, `imm32`, `xinput9_1_0`, `kernel32`, `user32`, `gdi32` and `shell32`. It writes `MasterLooter.ini`, `MasterLooter.log` and its eleven rotated predecessors `MasterLooter.01.log` to `MasterLooter.11.log`, `MasterLooter.presets` and `MasterLooter.backups` beside itself, plus a `MasterLooter-overflow-*.dmp` crash dump if a thread in the game runs out of stack, and nothing else, reads and writes no registry key, and installs nothing that outlives the game process. Every line is in this repository, and `build.bat` will produce the file for you if you would rather not trust mine.

Since 1.6.10 the plugin is code signed: right-click `MasterLooter.asi`, Properties, Digital Signatures shows Seth Walker, issued through Microsoft's identity-verified signing service and timestamped. A signature carries reputation from one release to the next, where a false-positive report to a vendor clears one file only, so the numbers above should move over the coming releases; this section will say whether they do.

If Defender or your browser quarantines the download, restore it and exclude the game's `bin64` folder, or build from source and use your own binary. SHA-256 for 1.6.42:

    0220194140863d76f4b2e5708422870917b4e7af71812a1f642f3387b4bcd737  MasterLooter-1.6.42-DMM.zip
    b02a7d8ad7ba3b2967bb0dec2f170a317bdda759e180d5b8e29998d1fb2a623e  MasterLooter-1.6.42.zip
    b244686089c563809d741d3834a51510ee8f69b9aabc38f9480605ff15516774  MasterLooter.asi

## Controls

- Insert opens and closes the menu. Escape also closes it.
- F10 turns auto-loot on and off. F11 loots everything in range once.
- Home is watch mode: the menu stays up while the game keeps every input. Insert makes it interactive again.
- While the menu is interactive the game does not see the keyboard, mouse or controller. Key releases still pass so nothing sticks.

All four keys are rebindable in the menu under General, and they also sit in `MasterLooter.ini` beside the plugin as `MenuKey`, `KeyToggle`, `KeyBurst` and `KeyWatch`, written as virtual-key codes. Edit the file and save it and the change is picked up about a second later without a restart, which is the quickest way to clear a clash with another ASI mod. Every key but the menu key can also be left unbound, with the Clear button beside it or a 0 in the file. Each of the four also takes a controller shortcut of two buttons at once rather than one, since every single button already does something in this game.

Hold Ctrl or Alt and none of these keys fire, unless the key is Ctrl or Alt itself, so Rebind takes one key on its own and turns a combination down with a line saying why. That keeps Private Storage Master's Ctrl+F10 from toggling auto-loot as well. Shift is left alone because the game sprints on it, and controller shortcuts work as they always did.

The menu keeps a pointer of its own rather than borrowing the game's, so clicks land where you point in fullscreen and in borderless windowed mode alike. On builds before 1.6.16 the two could come apart, and dragging the pointer into the top left corner was the way to put them back. Thanks to LuxDragon for finding that workaround and to Sov for the log that explained it.

## How it decides

A worker thread finds the game's actor manager by its RTTI class name and reads every world object around the player. Each object gets a verdict of its own.

It works as Kliff, Damiane or Oongka. Playing anyone but Kliff, the actor the game raises your events under is not the character you are steering: it sits wherever the game parked it and never walks. The mod finds the body you are steering by the gear parented to it and searches from there, so nobody has to be in the party. Swapping characters is noticed by who walks: a few steps after the swap and the scan is on the new character. Verdicts that pass are queued, and a hook on the game's own per-frame tick sends the game's own loot events (pick up, gather, catch, search carcass), the same events the game sends when you press the interaction key.

A pet or a companion loots whatever the game lets it, and nothing in the game looks at the item. Three switches under General deal with that, all off by default. "Stop pets picking up loose items" and "Stop pets looting bodies" answer no to the two questions the game asks before a pet loots, so the pet leaves those alone. With "Pets and companions follow the filters" on, a pet is told no before it reaches for a loose item your item rules, tags, classes or value floor refuse, and whatever it takes from a body that they refuse is deleted from the inventory as it lands, through the same removal event the game uses for its own deletions, with a notice on screen saying what went. Quest and protected items are never deleted, nor is anything refused only as unsellable or below the value floor, nor anything you pick up yourself, whoever else is out with you.

Bodies and carcasses have a switch of their own, "Drop refused loot from bodies", off by default. The game runs its requests on a server thread of its own, and a search pays out while that thread handles it. The mod reads the bag on either side, judges each new item by the same rules a loose one gets, and hands whatever they refuse to the routine the game uses when you drop something yourself. Each stack lands as one pile beside you. A copy you already carried is never the one that goes. The switch covers the mod's own searches and any body a pet or a companion loots, but never one you search yourself, and money, documents, quest items and protected items stay in the bag. [DROP.md](docs/investigations/DROP.md) has how the drop was found.

Anything you drop out of your bag yourself stays where it lands. The mod reads the request the game raises when you drop something, which says what left the bag and where the game will put it, then recognises that item by the id it carried in your bag, or by where it lands, and leaves it alone for the rest of the session. The Nearby tab lists those as "you dropped this". You can still pick one up by hand, and the loot-everything key leaves it on the ground too.

Rule order for an identified item: item override, tag never, protected tags (memory fragments, mechanism parts), tag always, dev/quest/unsellable filters (currency never counts as unsellable), copper value floor, class rule, then loot. Built-in protections apply before any of that: quest and shop objects, cooking fires and crafting stations, locked nodes, your own equipment and bag contents, anything you dropped yourself, gear worn by others, mechanism parts, container stacks, memory triggers, and anything the game's Take-or-Steal check calls theft. The [plugin manual](mod/README.md) has the details, the safety notes and the known limits.

## Building

Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure, which fetches Dear ImGui and MinHook.

    cd mod
    build.bat
    powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Unsigned

`dist\` receives the plugin and the documents, and `package.ps1` builds the two release archives from them. Releases are signed first with `scripts\sign.ps1`, which is why a local build needs `-Unsigned`. The item database and the creature table in `mod\data` are current for 2.01.00 and are compiled into the plugin; regenerating them after a game patch is described in [scripts/README.md](scripts/README.md). After a patch, `py -3 mod\scripts\sigcheck.py` reports which signatures still resolve against the installed exe.

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
