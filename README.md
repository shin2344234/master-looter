# Master Looter data

Item, item group, drop set and drop source tables for Crimson Desert 2.01.00 (exe 1.0.0.2760), parsed from the game's own static-info files. This is the data layer for the Master Looter mod.

## Layout

scripts/
- cdtables.py: readers for the .staticinfoheader/.staticinfobody tables and .paloc string tables, plus the ItemInfo record parser. `py -3 cdtables.py summary` must report `bad 0`.
- dropsets.py: DropSetInfo record parser. `py -3 dropsets.py` prints field statistics and must report `bad 0`.
- build_item_db.py: tags every item (group tree, name patterns, equip type, record flags) and writes items_tagged.csv/.json, tag_summary.md, unmapped_groups.txt.
- build_dropsets.py: writes dropsets.json, dropset_entries.csv, item_drop_sets.csv, dropset_summary.md.
- build_sources.py: finds the drop blocks inside CharacterInfo and GimmickInfo records by validated signature scans and writes character_drops.csv, gimmick_drops.csv, dropset_sources.csv, item_sources.csv, sources.json, sources_summary.md.
- make_review_page.py, make_dropset_page.py: render the two single-file review pages from the data folder.
- make_itemdb_tsv.py: exports items_tagged.csv as mod/data/MasterLooter.items.tsv, the table the plugin loads at runtime.

data/
- class_overrides.csv: the hand-made per-item corrections build_item_db.py applies (class, tags to add or remove).
- tag_summary.md, dropset_summary.md, sources_summary.md: the rule lists, counts and record layouts.
- The full outputs (items_tagged.csv/.json, character_drops.csv, the drop set and source tables, item_groups_tree.txt) are generated locally from your own extracted game files and are not committed: they carry the game's text and tables. The two TSVs the plugin ships with live in mod/data and are committed.

mod/
- The plugin itself: a C++ ASI with a Dear ImGui menu drawn through a DirectX 12 present hook, live INI settings and the loot rules engine. Build with `mod\build.bat` (needs Build Tools 2022). Details, controls and rule order are in mod/README.md.

## Regenerating

1. Extract group 0008 (`*.staticinfo*`) and group 0020 (all `.paloc`) from the game archives into extracted/0008 and extracted/0020 with `paz_unpack.py` from NattKh/CrimsonDesertModdingTools (needs `pip install lz4 cryptography`).
2. Run, in order: `cdtables.py summary`, `build_item_db.py`, `build_dropsets.py`, `build_sources.py`.
3. Render the pages: `make_review_page.py out.html` and `make_dropset_page.py out.html`.

Record layouts for 2.01.00 and the reasoning behind each field are in the module docstrings and in data/*_summary.md.

## Licence

MIT, see LICENSE. The plugin's third-party notices are in mod/THIRD_PARTY_NOTICES.md.
