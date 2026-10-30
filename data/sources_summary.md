# Drop sources, exe 1.0.0.3392

- characters: 7600; with reward drop sets: 6377; with equipment: 5127; with a catch/steal set: not derived on this build; catch_sets finds no anchor in the 1.0.0.3392 CharacterInfo layout, so the catch_sets column of character_drops.csv is empty
- gimmicks: 15563; with a drop block: 519
- drop sets used by at least one character or gimmick: 616 of 17130
- items with at least one known source (set via character or gimmick, worn equipment): 3027 of 8373

## How each source was found

Character records hold a reward list (count, then triples of drop set, reward flag, repeat count) and an equipment list (count, then 64-byte entries of item, unused set key and seven percent fields). Gimmick records hold a flagged list of drop set keys, a flagged list of inline entries and a flagged buyable entry; the roll count and drop socket sit elsewhere in the record on this build and are not read. Each block was located by requiring every referenced key to exist in its table.

## Equipment drops

NPC gear never points at a drop set: the worn item itself drops. The first percent field is the death-drop chance (3% to 5% for most gear, 0 for players), the second the throw-drop chance, then min/max endurance of the dropped piece (15% to 20%, which is why it arrives damaged), the enhanced chance (5%) and min/max enhanced endurance.

## Reward flag values by set family (top 20)

- 0x4 on DropContribution: 3094
- 0x409 on Character: 2430
- 0x409 on Human: 1829
- 0x1 on Human: 1705
- 0x1 on Faction: 1162
- 0x800 on Middleclass: 1086
- 0x401 on Character: 738
- 0x400 on Faction: 700
- 0x1 on Character: 696
- 0x800 on Soldier: 599
- 0x401 on Human: 560
- 0x9 on Faction: 294
- 0x1 on Middle: 270
- 0x800 on Worker: 247
- 0x2 on Arrow: 244
- 0x409 on Troll: 220
- 0x9 on Character: 216
- 0x800 on Upperclass: 187
- 0x409 on Orc: 182
- 0x1 on Animal: 176

Flags are the stored reward_tag_type_flag bitfields. 0x400 series co-occur with common death drops, 0x800 with favorite-item gifts; the exact bit meanings are not decoded.
