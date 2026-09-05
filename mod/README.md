# Master Looter plugin

An ASI plugin for Crimson Desert 2.01.00: an auto-looter driven by the tagged item database, with an in-game settings menu.

## What is in the box

- `MasterLooter.asi`: the plugin. Draws a Dear ImGui menu over the game through a DirectX 12 present hook (adapted from Trinity, see THIRD_PARTY_NOTICES.md), so it works with DLSS frame generation and HDR.
- `MasterLooter.items.tsv`: the item database (6,813 items with runtime row id, class, tags, tier and sell value), generated from `data/items_tagged.csv` by `scripts/make_itemdb_tsv.py`.
- `MasterLooter.ini`: written next to the plugin on first run, rewritten whenever a setting changes in the menu, and reloaded within a second if edited by hand while the game runs.
- `MasterLooter.log`: rewritten every launch. Signature resolution, hook installation, the event self test and every item taken are logged there.

## Controls

- Insert opens and closes the menu (rebindable under General). Escape also closes it.
- F10 turns auto-loot on and off, F11 loots everything in range once. Both rebindable.
- While the menu is open the game does not see the keyboard, mouse or controller. Key releases still pass through so nothing sticks.
- Tabs: General (switches, keys, pace, filters), Looting (what to collect, ranges, node arming, ownership), Classes, Tags, Items (search any item, set an override, see the live verdict), Nearby (every object around you with the rule that decided it), Status (signatures, hooks, event path, counters, log).

## How looting works

A worker thread finds the game's actor manager by its RTTI class name, reads every world object around the player (position, components, node data), and decides per object. Decisions that pass are queued and the game-thread pump sends the game's own loot events (pick up, gather, catch, search carcass), exactly as the game does when you press the interaction key. Empty nodes such as ore veins are armed first so the game fills their data without you standing on them.

Rule order for an identified item: item override, tag never, tag always, dev/quest/unsellable filters, copper value floor, class rule, then loot. Built-in protections apply before any of that: quest and shop objects, locked nodes, your own equipment and bag contents, gear worn by others, mechanism parts, container stacks, memory triggers, and anything the game's own Take-or-Steal check calls theft (unless you opt in).

## Compatibility and patches

Every game address comes from a byte pattern or an RTTI name resolved at load; nothing is hardcoded. After a game patch run `py -3 scripts\sigcheck.py` against the new exe: it reports which patterns in `src/loot/signatures.h` still match uniquely. The Status tab shows the same list in game.

## Build

Needs Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure (Dear ImGui and MinHook are fetched).

    py -3 ..\scripts\make_itemdb_tsv.py
    build.bat

Output lands in `dist\`. Copy `MasterLooter.asi` and `MasterLooter.items.tsv` into the game's `bin64\` next to the ASI loader (`winmm.dll`), or into the DMM mods folder, while the game is closed.

## Files

- `src/core`: paths, log, settings (INI load, debounced save, hot reload), item database, rules.
- `src/hooks`: DX12 present hook and swapchain wrapper, window procedure subclass, XInput neutraliser.
- `src/gui`: style, menu key polling, the menu and HUD.
- `src/loot`: signatures, guarded memory and pattern scanning, game structures, the event protocol, MinHook detours, and the engine itself.
- `scripts/adapt_trinity_dx12.py` regenerates the DX12 files from a Trinity checkout; `scripts/sigcheck.py` checks the signatures offline.
