# The game's item log

Issue #124. fraidnot asked for a way to switch off the game's
own pickup notifications on the right of the screen, and xICEMANx117 asked for
the same because of the stone pickups. `HideItemLog=1` in `MasterLooter.ini`
does it. Everything below was read statically from exe 1.0.0.3732 on 24
November 2026. None of it has been watched in play yet.

## The panel

The list on the right is `UIGamePlayControlRootItemLog@uiCommonScript`, vtable
+0x5A7DC80, and each line on it is an `ItemLogData@uiCommonScript`, vtable
+0x5A7E1E8, built by +0xF3CEF0 or +0xF3CF90.

An `ItemLogData` holds a byte at +8 that is 0 for an item line, the u16 item row
at +0x20 (0xFFFF when there is none), the count as an i64 at +0x28, a category
byte at +0xF0 and a u16 at +0xF2.

The panel keeps a queue of lines waiting to be shown at +0x158: head u32 at
+0x158, count u32 at +0x160, capacity u32 at +0x164 and the array of
`ItemLogData*` at +0x170. The flags at +0x1D0 and +0x1D1 decide whether it is
visible, and +0x1441F5B40(panel, bool) shows or hides it. The panel's update,
slot 36 at +0xF3DED0, takes one line off the queue per tick and plays
`03_ui_seq/gamestagegameplay/hud/itemlogview/cd_seq_ui_itemlog_appear`.

## Where a line is added

Seven builders (+0x11A5B80, +0x11A5F40, +0x11A6760, +0x11A6CB0, +0x11A7430,
+0x11A8150, +0x11ABF10) put a batch of lines together and call the thunk at
+0x11954F0, which jumps to +0xA478D10 in the `.text1` section. That routine takes
the batch in rcx (vector at +0x30, count at +0x38, capacity at +0x3C) and the UI
root in rdx, and walks the batch. For each line whose +8 byte is 0 and whose
item is not money (ItemInfo through +0x3DB320, money when its byte at +0x38 is
0x20) it looks up the panel by the name "ItemLogView" and calls the push at
+0xF43E70 with the panel's queue and a pointer to the batch slot. The call is at
+0xA478E96. Money goes to "MoneyLogView" through +0xF417B0 instead.

After the loop it calls +0x5363A0 on the batch, which runs the virtual
destructor of every slot that is not null and sets the count to 0.

## The push, and why skipping it is safe

+0xF43E70 is a generic queue push with about seventy callers. When it takes the
line it copies the pointer out of the slot, writes 0 into the slot so the clear
above leaves it alone, bumps the count and returns 1. When the queue is full
and cannot grow it returns 0 and touches nothing, and the line stays in the slot
for the clear to delete. When "ItemLogView" is not found the routine skips the
push altogether and the clear deletes the line the same way. So a line that is
never pushed is one the game already knows how to drop.

Straight after the call the game reads the queue count at +0x160 and the two
flags. With the queue empty and +0x1D0 clear it hides the panel, which is the
state an empty log is in anyway.

## What the mod does

`kSig_ItemLogPush` in `signatures.h` matches the `lea rcx, [rbx+0x158]; mov
rdx, rsi; call` and the count and flag tests after it, once in the exe.
`farhook::InstallCallSite` writes a fourteen-byte relay into int3 padding
within reach and changes only the call's rel32 to point at it, so the other
callers of the push never pass through the mod. `hkItemLogPush` reads
`HideItemLog` on every call. Off, it calls the push. On, it returns 0 and
writes an `[itemlog] hid the game's pickup line` line naming the item and the
count. Startup writes `[itemlog] item log call at +0x... redirected` or an
error saying the call was not found.

The detour reads nothing from its return address. A note in `hooks.cpp` says
that reading `_ReturnAddress` in a detour there once broke controller press and
hold, which is the other reason to redirect the call rather than hook the push
and filter by caller.

## Not known yet

Whether the pickup sound xICEMANx117 mentioned goes with the line. If it plays
from the appear sequence the update runs, it goes too. If the builders play it
themselves, it stays. A play test with the switch on answers it.

The game's string table has "Hide Notifications", "Toggle Notifications" and
"Hide the notification UI from the screen.", which may be a game option of its
own. Where they are used was not traced.
