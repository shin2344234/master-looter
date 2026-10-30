# Data pipeline

Item, item group, drop set and drop source tables for Crimson Desert exe 1.0.0.3392 (Steam build 26308950, the DLC update of 29 October 2026), parsed from the game's own static-info files. This is the data layer behind the Master Looter plugin: it produces the item database, the creature table and the gather node table the plugin ships with.

## Scripts

- cdtables.py: readers for the .staticinfoheader/.staticinfobody tables and .paloc string tables, plus the ItemInfo record parser. `py -3 cdtables.py summary` must report `bad 0`. Both loaders add the DLC's own rows and strings from extracted-dlc_01 by default; see its docstring.
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

data/class_overrides.csv and the three summaries are committed. The full outputs (items_tagged.csv/.json, character_drops.csv, the drop set and source tables, item_groups_tree.txt, unmapped_groups.txt) are generated locally from your own extracted game files and are not: they carry the game's text and tables. The three TSVs in mod/data are committed and current for exe 1.0.0.3392, DLC rows included; a plain build of the plugin does not need any of this.

## Regenerating after a game patch

1. Extract group 0008 (`*.staticinfo*`) and group 0020 (all `.paloc`) from the game archives into extracted/0008 and extracted/0020. From exe 1.0.0.3392 on the pack index has a new format that `paz_unpack.py` from NattKh/CrimsonDesertModdingTools cannot read; `patch-dlc\tools\extract_flat.py` reads both and writes the same layout. The DLC's own tables and English strings (`dlc\dlc_01` in the install, files named `<table>.dlc_01.*`) go into extracted-dlc_01/0008 and extracted-dlc_01/0020 the same way. Move the previous extraction aside to extracted-<build> first.
2. Run `cdtables.py summary` and fix any layout drift until it reports `bad 0`.
3. Run, in order: `build_item_db.py`, `make_itemdb_tsv.py`, `build_dropsets.py`, `build_sources.py`, `make_creatures_tsv.py`, `make_nodes_tsv.py`. The creature table reads its names and item rows from the `character_drops.csv` that `build_sources.py` writes, so skipping it rebuilds creatures from the previous patch's data and nothing says so. The node table reads its `gimmick_drops.csv` to find the jars that roll a drop set, so the same goes for nodes. `build_sources.py` reads the `dropsets.json` that `build_dropsets.py` writes, so that script has to run first. On a fresh checkout `build_sources.py` stops on the missing file, and on an old one it reads the previous patch's sets.
4. Rebuild the plugin; the TSVs are compiled into it.

Items only the DLC carries are written to the item table with row -1. Nobody has yet seen where the game numbers them in a DLC owner's runtime table, so the plugin finds them by their live string key and never by row.

Record layouts for exe 1.0.0.3392 and the reasoning behind each field are in the module docstrings and in data/*_summary.md.
