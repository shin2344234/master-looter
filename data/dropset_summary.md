# Drop sets, build 2.02.00

17146 drop sets, 21698 entries, parsed from dropsetinfo.staticinfobody with every record ending on its byte boundary.

## Kinds of set

- inline item: 13917
- designed: 1335
- inline knowledge: 1310
- mercenary: 321
- inline friendly: 112
- sub-level exp: 88
- gameplay variable: 63

Inline sets are generated from scripts and quests (their name is literally `Item(Carrot,5)` or `Knowledge(...)`): one guaranteed target. Designed sets (`DropSet_*`) are the hand-made loot tables.

## What entries point at

- item: 19067
- knowledge: 1823
- character (mount): 355
- contribution: 169
- friendly points: 116
- gameplay variable: 99
- type15: 21
- gimmick: 20
- character (spawn): 15
- type16: 4
- knowledge group: 4
- character (mercenary): 3
- type14: 2

## Designed set families (first two name tokens)

- DropSet_KitchenTool: 330
- DropSet_Faction: 121
- DropSet_Reblockade: 77
- DropSet_Craft: 72
- DropSet_Operation: 47
- DropSet_AbyssGear: 42
- DropSet_DropItem: 33
- DropSet_Character: 28
- DropSet_DropKnowledge: 25
- DropSet_Contribution: 17
- DropSet_Legendary: 15
- DropSet_Fish: 14
- DropSet_DropMercenary: 14
- Drops_Reward: 13
- DropSet_Ocean: 13
- Affinity(NHW_Unique: 12
- DropSet_Camp: 11
- DropSet_RestArea: 11
- DropSet_Middle: 10
- DropSet_Farm: 10
- DropSet_ChestItem: 10
- DropSet_GameplayVariable: 10
- DropSet_BossRush: 10
- DropSet_Animal: 9
- DropSet_Money: 9
- DropSet_Recipe: 9
- DropSet_Affinity: 8
- DropSet_Gimmick: 8
- DropSet_GamePlayVariable: 8
- DropSet_Dungeon: 7
- DropSet_TreeWeapon: 6
- DropSet_MiniGame: 6
- DropSet_Big: 6
- DropSet_Random: 6
- DropSet_Riding: 6
- DropSet_Small: 5
- DropSet_Large: 5
- DropSet_Normal: 5
- Purchase_Reward: 5
- DropSet_Living: 4

## Items

- 3319 of 8374 items appear in at least one drop set; 1682 appear in a designed set.

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
