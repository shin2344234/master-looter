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

data/
- The outputs listed above, plus item_groups_tree.txt (the full item group hierarchy with English names).

## Regenerating

1. Extract group 0008 (`*.staticinfo*`) and group 0020 (all `.paloc`) from the game archives into extracted/0008 and extracted/0020 with `paz_unpack.py` from NattKh/CrimsonDesertModdingTools (needs `pip install lz4 cryptography`).
2. Run, in order: `cdtables.py summary`, `build_item_db.py`, `build_dropsets.py`, `build_sources.py`.
3. Render the pages: `make_review_page.py out.html` and `make_dropset_page.py out.html`.

Record layouts for 2.01.00 and the reasoning behind each field are in the module docstrings and in data/*_summary.md.
