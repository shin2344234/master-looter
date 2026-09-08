# Toggling frame generation killed the game

Found 8 September 2026 from a user report, reproduced on a borrowed machine,
and fixed by a two-line rule after four wrong builds. Written up because the
wrong turns cost more than the answer did, and because the diagnostics that
finally worked are the reusable part.

## The report

Fyreon87, on the mod page: turning frame generation on or off in the game's
settings crashes Crimson Desert, but only with Master Looter loaded. They found
it by bisecting their load order for ninety minutes, ran the mod alone, ran
everything except the mod, and reproduced it on two versions.

It cannot be reproduced on a 3090 Ti. A borrowed 5060 Ti reproduces it every
time, within a couple of minutes of a toggle.

## The rule

**Wrap the first swapchain. Never wrap a replacement.**

The mod wraps the swapchain so the menu can draw before frame generation
interpolates, which is why that code exists at all. Toggling frame generation
destroys the chain and builds a new one, in either direction, and the new one
came back through the patched factory slot and got wrapped too.

Six sessions, every one wrapping a replacement, every one dead. One session
with wrapping off entirely took repeated toggles both ways, before and after
loading a save, and did not care. The first wrap has never killed anything.

`g_wrapperActive` had been recording "we have wrapped one" since long before
any of this, and never gets cleared. The fix reads it.

## What the game actually does

Two null pointer reads in `CrimsonDesert.exe`, eight milliseconds apart, on one
thread, during the rebuild:

    0xC0000005 at +0x2B8B5CA  reading 0x0
    0xC0000005 at +0x3B57552  reading 0x30

The first:

    mov  rcx, qword ptr [rbx + 0x28]   ; an interface pointer out of the object
    mov  rax, qword ptr [rcx]          ; its vtable          <-- rcx is null
    call qword ptr [rax + 0x50]

Twenty bytes further down the same function, a different path loads the same
field and tests it:

    lea  rsi, [rbx + 0x28]
    mov  rcx, qword ptr [rsi]
    test rcx, rcx
    je   ...

So the game knows that pointer can be absent, and the path that dies assumes it
cannot be. The second fault is the same shape: a getter returns null and the
caller reads it at +0x30 without checking.

Something the game looks up during the rebuild is not found. Handing it our
wrapper in place of the real swapchain is the obvious way to make a lookup
keyed on that pointer miss. That last step is inference; the rest is measured.

## Four builds that did not work, and why

**1. Refuse to wrap while one of our wrappers is still alive.** The reasoning
was that a creation arriving while ours is live must belong to somebody else.
It never fired once. The game releases its swapchain before creating the
replacement, so by then ours is already gone. Whether a wrapper is alive is the
wrong question; whether we have ever wrapped one is the right one.

**2. A handler around the scan.** The logs all ended mid-scan, so the scan
looked guilty. It was not: the scan runs on the mod's own worker thread and the
last line in a log is only where that thread had reached, not where anything
went wrong. The handler never fired.

**3. `SetUnhandledExceptionFilter`.** Also never fired. That slot is global and
singular, and the game installs its own crash reporting after this plugin
loads, which replaces ours.

**4. `WrapSwapChain=0` as a diagnostic.** This is the trap worth remembering.
It appears to isolate the wrapper while leaving the mod running. On the test
machine another mod had already detoured DXGI `Present`, so with wrapping off
there was no render callback from either source, and `loot::Start` was gated
behind the first rendered frame. The mod loaded, said "hooks installed", and
did nothing at all for the whole session without a word. A dead mod not
crashing the game proves nothing. Fixed separately: the engine now starts on a
watchdog rather than waiting for a frame.

## What finally worked

`AddVectoredExceptionHandler`. It sits on a chain and cannot be displaced by
whatever the game installs later. It sees every first-chance exception,
including the ones `mem::` raises deliberately when it probes an address, so it
skips faults whose address is inside this plugin and reports the rest. It logs
and returns continue-search, so nothing is suppressed and the process still
dies exactly as it would have.

That is the tool to reach for first next time, not the top-level filter.

## The cost

After a toggle the menu stops drawing until the game is restarted, because the
chain it drew through is gone and the replacement is left alone. That is the
trade. A menu that stops beats a game that stops.

## Not the same bug as the report

Worth keeping straight. The machine this was reproduced on carries a SudoMaker
Virtual Display Adapter, which participates in adapter enumeration and which
Fyreon87 almost certainly does not have. The trigger matches and the fix should
cover both, but nothing here proves the reported crash and the reproduced one
are the same fault.

Issue #30.
