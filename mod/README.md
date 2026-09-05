# Master Looter plugin

An ASI plugin for Crimson Desert 2.01.00 with an in-game settings menu. This build ships the menu, live settings and the loot rules engine; the game-side looting hooks come next.

## What is in the box

- `MasterLooter.asi`: the plugin. Draws a Dear ImGui menu over the game through a DirectX 12 present hook (adapted from Trinity, see THIRD_PARTY_NOTICES.md), so it works with DLSS frame generation and HDR.
- `MasterLooter.items.tsv`: the item database (6,813 items with class, tags, tier and sell value), generated from `data/items_tagged.csv` by `scripts/make_itemdb_tsv.py`.
- `MasterLooter.ini`: written next to the plugin on first run, rewritten whenever a setting changes in the menu, and reloaded within a second if edited by hand while the game runs.

## Controls

- Insert opens and closes the menu (rebindable under General). Escape also closes it.
- While the menu is open the game does not see the keyboard, mouse or controller. Key releases still pass through so nothing sticks.
- Tabs: General (switches, range, rate, filters), Classes (loot or skip each item class), Tags (always or never per tag), Items (search any item, set a per-item override, see the live verdict), Status (hooks, database, log).

## Rule order

Item override, then a tag marked never, then a tag marked always, then the quest, unsellable and dev filters, then the copper value floor, then the class rule, then loot.

## Build

Needs Visual Studio 2022 Build Tools with the C++ workload (CMake and Ninja come with it) and internet on the first configure (Dear ImGui and MinHook are fetched).

    py -3 ..\scripts\make_itemdb_tsv.py
    build.bat

Output lands in `dist\`. Copy `MasterLooter.asi` and `MasterLooter.items.tsv` into the game's `bin64\` next to the ASI loader (`winmm.dll`), or into the DMM mods folder, while the game is closed.

## Files

- `src/core`: paths, log (buffered until the rendering process claims it), settings (INI load, debounced save, hot reload), item database, rules.
- `src/hooks`: DX12 present hook and swapchain wrapper, window procedure subclass, XInput neutraliser.
- `src/gui`: style, menu key polling, the menu itself.
