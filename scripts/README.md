# Data pipeline

Item, item group, drop set and drop source tables for Crimson Desert 2.01.00 (exe 1.0.0.2760), parsed from the game's own static-info files. This is the data layer behind the Master Looter plugin: it produces the item database, the creature table and the gather node table the plugin ships with.

## Scripts

- cdtables.py: readers for the .staticinfoheader/.staticinfobody tables and .paloc string tables, plus the ItemInfo record parser. `py -3 cdtables.py summary` must report `bad 0`.
- dropsets.py: DropSetInfo record parser. `py -3 dropsets.py` prints field statistics and must report `bad 0`.
- build_item_db.py: tags every item (group tree, name patterns, equip type, record flags, then the hand-made corrections in data/class_overrides.csv) and writes items_tagged.csv/.json, tag_summary.md, unmapped_groups.txt.
- build_dropsets.py: writes dropsets.json, dropset_entries.csv, item_drop_sets.csv, dropset_summary.md.
- build_sources.py: finds the drop blocks inside CharacterInfo and GimmickInfo records by validated signature scans and writes character_drops.csv, gimmick_drops.csv, dropset_sources.csv, item_sources.csv, sources.json, sources_summary.md.
- make_itemdb_tsv.py: exports items_tagged.csv as mod/data/MasterLooter.items.tsv, the table the plugin loads at runtime (row id, key, class, tags, tier, value).
- make_creatures_tsv.py: exports every catchable creature with the class of the item it becomes as mod/data/MasterLooter.creatures.tsv.
- make_nodes_tsv.py: exports every gather node as mod/data/MasterLooter.nodes.tsv, keyed on the basename of the prefab it is placed from. A node's runtime id changes between sessions, so the prefab is the only durable name it has; the gimmick row's own tags (collect_botany, collect_tree, collect_mine and the rest) give the kind, and the socket name gives the item where the two share a name.
- make_review_page.py, make_dropset_page.py: render single-file review pages from the data folder.
- make_translation_template.py: read the menu strings out of mod/src and write docs/MasterLooter.template.txt, the file a translator fills in. Re-run it whenever a menu string is added or the template goes stale.

## What is committed and what is not

data/class_overrides.csv and the three summaries are committed. The full outputs (items_tagged.csv/.json, character_drops.csv, the drop set and source tables, item_groups_tree.txt, unmapped_groups.txt) are generated locally from your own extracted game files and are not: they carry the game's text and tables. The three TSVs in mod/data are committed and current for 2.01.00; a plain build of the plugin does not need any of this.

## Regenerating after a game patch

1. Extract group 0008 (`*.staticinfo*`) and group 0020 (all `.paloc`) from the game archives into extracted/0008 and extracted/0020 with `paz_unpack.py` from NattKh/CrimsonDesertModdingTools (needs `pip install lz4 cryptography`).
2. Run `cdtables.py summary` and fix any layout drift until it reports `bad 0`.
3. Run, in order: `build_item_db.py`, `make_itemdb_tsv.py`, `make_creatures_tsv.py`, `make_nodes_tsv.py`. The drop set scripts (`build_dropsets.py`, `build_sources.py`) are only needed for the review pages.
4. Rebuild the plugin; the TSVs are compiled into it.

Record layouts for 2.01.00 and the reasoning behind each field are in the module docstrings and in data/*_summary.md.
