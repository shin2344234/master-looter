# Drop sources, build 2.01.00

- characters: 7250; with reward drop sets: 6107; with equipment: 4934; with a catch/steal set: 1125
- gimmicks: 13906; with a drop block: 314
- drop sets used by at least one character or gimmick: 640 of 14744
- items with at least one known source (set via character or gimmick, worn equipment, or catch): 2937 of 6813

## How each source was found

Character records hold a reward list (count, then triples of drop set, reward flag, repeat count), an equipment list (count, then 64-byte entries of item, unused set key and seven percent fields) and, for catchable animals and searchable people, an inline drop set followed by 01 00 00 00 00. Gimmick records hold roll count, a list of 64-bit drop set keys, a socket name, a buyable item and inline entries. Each block was located by requiring every referenced key to exist in its table.

## Equipment drops

NPC gear never points at a drop set: the worn item itself drops. The first percent field is the death-drop chance (3% to 5% for most gear, 0 for players), the second the throw-drop chance, then min/max endurance of the dropped piece (15% to 20%, which is why it arrives damaged), the enhanced chance (5%) and min/max enhanced endurance.

## Reward flag values by set family (top 20)

- 0x4 on DropContribution: 3011
- 0x409 on Character: 2347
- 0x409 on Human: 1746
- 0x1 on Human: 1644
- 0x1 on Faction: 1123
- 0x800 on Middleclass: 1076
- 0x401 on Character: 712
- 0x1 on Character: 678
- 0x400 on Faction: 666
- 0x800 on Soldier: 592
- 0x401 on Human: 533
- 0x800 on Worker: 307
- 0x9 on Faction: 294
- 0x1 on Middle: 258
- 0x2 on Arrow: 231
- 0x409 on Troll: 220
- 0x9 on Character: 207
- 0x409 on Orc: 182
- 0x1 on Animal: 173
- 0x1 on Goblin: 171

Flags are the stored reward_tag_type_flag bitfields. 0x400 series co-occur with common death drops, 0x800 with favorite-item gifts; the exact bit meanings are not decoded.
