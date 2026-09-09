# Toggling frame generation killed the game

Found 8 September 2026 from a user report, reproduced on a borrowed machine,
and settled after twelve builds, four of which were confidently wrong. Written
up because the wrong turns cost far more than the answer did.

## The report

Fyreon87, on the mod page: turning frame generation on or off in the game's
settings crashes Crimson Desert, but only with Master Looter loaded. They found
it by bisecting their load order for ninety minutes, ran the mod alone, ran
everything except the mod, and reproduced it on two versions.

It cannot be reproduced on a 3090 Ti. A borrowed 5060 Ti reproduces it every
time, within a couple of minutes of a toggle.

## The answer

**The render target views held references to back buffers of a swapchain the
game was destroying.**

`CreateRenderTargets` calls `GetBuffer` for every back buffer and keeps the
resources. While those views exist the swapchain has outstanding references to
its own buffers, and the game dropping its own reference does not end that.

Every other path in `dx12_hook.cpp` that touches those buffers retires the GPU
and drops the views first. `ReconcileSwapChain` does it. `PreResizeCleanup`
does it. Teardown did neither, and teardown is where the game dies.

The wrapper's destructor now calls `WaitForOverlayIdle`, `CleanupRenderTargets`
and `ReleaseOffscreenTarget` before releasing the inner swapchain, and only when
that wrapper owns the chain the views describe.

## What settled it

A build that wrapped the swapchain exactly as normal and then took nothing at
all: no ImGui init, no render targets, no descriptor heaps, not one reference on
a back buffer. It wrapped the first chain and five replacements across repeated
toggles and never crashed.

    wrapped + resources + drawing   crash, every time
    wrapped + nothing               survives five toggles
    not wrapped at all              survives

That is the whole diagnosis in three lines, and it took eight builds to think of
running the middle one. Reach for the experiment that splits a suspect in two
earlier than that.

The first attempt at it was placed wrong: it returned out of `RenderOverlay`
after `InitImGui` had already run, and `InitImGui` calls `CreateRenderTargets`.
It would have held the resources anyway and proved nothing while looking
conclusive. Check where a "do nothing" switch actually sits.

## What the game does when it dies

Two null pointer reads in `CrimsonDesert.exe`, milliseconds apart, on one
thread:

    0xC0000005 at +0x2B8B5CA  reading 0x0
    0xC0000005 at +0x3B57552  reading 0x30

The first:

    mov  rcx, qword ptr [rbx + 0x28]   ; an interface pointer out of the object
    mov  rax, qword ptr [rcx]          ; its vtable          <-- rcx is null
    call qword ptr [rax + 0x50]

Twenty bytes down the same function another path loads the same field and tests
it for null before using it. So the game knows that pointer can be absent and
the path that dies assumes it cannot be.

## Wrong turns, in order

**1. The queue pin.** The first theory was that the present queue got pinned to
Streamline's pacer and could never be corrected. Reading `PublishPresentQueue`
kills it: the queue is re-published on every authoritative call. The comment
that inspired the theory was documenting a bug that had already been fixed.

**2. Refuse to wrap while one of our wrappers is still alive.** Never fired
once. The game releases its swapchain before creating the replacement, so ours
is already gone by then.

**3. A handler around the scan.** Every log ended mid-scan so the scan looked
guilty. It was not: the scan runs on the mod's own worker thread and the last
line in a log is only where that thread had reached. The handler never fired.

**4. `SetUnhandledExceptionFilter`.** Also never fired. That slot is global and
singular and the game installs its own crash reporting after this plugin loads,
which replaces ours. **Use `AddVectoredExceptionHandler`**: it sits on a chain
and cannot be displaced. Skip faults whose address is inside this plugin, since
`mem::` raises those on purpose, and report the rest.

**5. Never wrap a replacement.** Stopped the crash and cost the menu entirely,
because the game recreates its chain two or three times during startup before
anything is drawn. Narrowing it to "only after the overlay is up" got the menu
back and still crashed, which is what finally showed the death is in the
teardown of the old chain rather than the creation of the new one: the refusal
line never printed before the fault. Both builds were aimed at the wrong
suspect and both are gone.

**6. `WrapSwapChain=0` as a diagnostic.** The trap worth remembering. It looks
like it isolates the wrapper while leaving the mod running. On the test machine
another mod had already detoured DXGI `Present`, so with wrapping off there was
no render callback from either source, and `loot::Start` was gated behind the
first rendered frame. The mod loaded, said "hooks installed", and did nothing
for the whole session without a word. A dead mod not crashing proves nothing.
Fixed separately: the engine starts on a watchdog now.

## Caught in passing

- The PE version resource had read 1.3.0 since that release. Five shipped
  versions told Windows the wrong number. It is built from `version.h` now.
- The loot engine only started from the first rendered frame, so on any machine
  where another overlay owns `Present` and wrapping is off, the whole mod
  switched itself off silently. It starts on a watchdog now.
- That watchdog then ran in every host, including the game's crash reporter,
  which never renders. It fired there, ran the whole late init, and claimed the
  log away from the real session, so one log had a `crashpad_handler.exe` header
  and two processes interleaved in it. Gated to the game, the way the log claim
  beside it always was.

## Not proven to be the reported bug

The machine this was reproduced on carries a SudoMaker Virtual Display Adapter,
which takes part in adapter enumeration and which Fyreon87 almost certainly does
not have. The trigger matches and the fix should cover both. That the two
crashes are the same fault is not proven.

Issue #30.
