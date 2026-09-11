# Drop sets, build 2.02.00

14744 drop sets, 18044 entries, parsed from dropsetinfo.staticinfobody with every record ending on its byte boundary.

## Kinds of set

- inline item: 12140
- inline knowledge: 1174
- designed: 1140
- inline friendly: 112
- sub-level exp: 85
- mercenary: 65
- gameplay variable: 28

Inline sets are generated from scripts and quests (their name is literally `Item(Carrot,5)` or `Knowledge(...)`): one guaranteed target. Designed sets (`DropSet_*`) are the hand-made loot tables.

## What entries point at

- item: 15930
- knowledge: 1638
- contribution: 166
- friendly points: 115
- character (mount): 97
- gameplay variable: 54
- gimmick: 20
- character (spawn): 15
- knowledge group: 4
- character (mercenary): 3
- type16: 2

## Designed set families (first two name tokens)

- DropSet_KitchenTool: 290
- DropSet_Faction: 121
- DropSet_Reblockade: 77
- DropSet_AbyssGear: 42
- DropSet_Craft: 32
- DropSet_Operation: 31
- DropSet_DropItem: 31
- DropSet_Character: 28
- DropSet_DropKnowledge: 25
- DropSet_Contribution: 17
- DropSet_Legendary: 15
- DropSet_Fish: 14
- DropSet_DropMercenary: 14
- DropSet_Camp: 11
- DropSet_RestArea: 11
- DropSet_Middle: 9
- DropSet_ChestItem: 9
- DropSet_Money: 9
- DropSet_Animal: 8
- DropSet_Farm: 8
- DropSet_Gimmick: 8
- DropSet_GamePlayVariable: 8
- DropSet_Dungeon: 7
- Drops_Reward: 7
- DropSet_TreeWeapon: 6
- DropSet_MiniGame: 6
- DropSet_Random: 6
- DropSet_Small: 5
- DropSet_Large: 5
- DropSet_Riding: 5
- DropSet_GameplayVariable: 4
- DropSet_Abyss: 4
- DropSet_BoxBarrel: 4
- DropSet_Chest: 4
- DropSet_ExpensionInventory: 4
- DropSet_Collection: 4
- DropSet_Inventory: 4
- Purchase_Reward: 4
- Bilibili_Drops: 4
- DropSet_Friendly: 3

## Items

- 2765 of 6813 items appear in at least one drop set; 1416 appear in a designed set.

## Field semantics

- weight_pct: the entry's stored percent / 10,000 (1,000,000 = 100%). In every multi-entry set the entry percents sum exactly to the set's total_drop_rate, so they behave as weights.
- share_pct: weight_pct / total, the entry's share of one roll of the set. Whether a set with a total below 100% can also yield nothing is not decided by the data alone.
- total_rate_pct: the set's total_drop_rate / 10,000 (100% for 14,267 of 14,744 sets; 13% to 500% elsewhere).
- min/max: quantity range; negative for friendly-point penalties.
- enchant: enchant level rolled for gear entries (1 to 5), blank when the field is 65535.
- roll_type / roll_count: the set's drop_roll_type and drop_roll_count bytes as stored; 0/0 for inline sets.
- condition: conditioninfo string keys stored in the entry's condition slots (owner or player conditions).
- need_slot: false when nee_slot_count is 65535.
- tail: raw type-specific trailing bytes (mostly the target key repeated; friendly entries carry 32 bytes of reward data).
