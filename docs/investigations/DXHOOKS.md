# The startup crash with RenoDX, and the throwaway device behind it

Issue #34. Fixed in 1.6.5, 9 September 2026.

## The report

ardenness, on 1.6.3: the game dies six to fifteen seconds after launch, before
any input, with RenoDX and Crimson Route installed. Their own bisection was
better than most: removing RenoDX fixed it, swapping the RenoDX variant did
not, changing Crimson Route's renderer did not, and Crimson Weather (another
ReShade addon) was fine alongside. devikyn and kfen72 reported the same shape
on Windows, jacksterson under Proton.

Every log carried the same last line:

    [fault] 0xC0000005 at 00000001430314D2 (CrimsonDesert.exe+0x30314D2) thread 17232, reading 0000000000000030

A read of offset 0x30 off a null pointer, inside the game, about two seconds
after the swapchain was rebuilt as a two buffer waitable chain (flags 0x840).
Nothing of this mod's was on the stack, which is why three earlier guesses all
pointed at the swapchain wrapper.

## Reproducing it

The reporter's load order was installed here on 9 September: Crimson Route
6.9.6 with its default Present renderer, ReShade 6.8.0 with addon support (it
installs as `dxgi.dll` beside the game), and the "No DLSS 5 or RR 4.5" build of
`renodx-crimsondesert.addon64`. First launch with 1.6.4: crash, same address.
`docs/repro-34.txt` is the exact setup and the two-step test that was handed
over.

Seven launches, each changing one thing. The two switches that mattered:

- `WrapSwapChain=0`, which stops the mod touching the swapchain at all. Still
  crashed. That cleared the wrapper, which had been the suspect for a day.
- `HookDX12=0`, added for this hunt, which skips the whole DirectX layer and
  leaves looting running. Loaded fine. So the fault lived in the eighty lines
  that run before any hook exists, and there were only two things there: DRED
  being armed, and a throwaway device.

ReShade's own log at debug level (`ReShade.ini`, `[GENERAL] LogLevel=4`) gave
the rest. RenoDX writes `OnInitDevice(Hooking device: ...)` every time ReShade
hands it a new device. In the crashing run it wrote that line once, for a D3D12
device created one second after this mod started, and never again. The game's
own devices were created three seconds later and got no such line.

## What was happening

`InstallDX12Hooks` used to create a private D3D12 device, a command queue and a
swapchain on a hidden window, purely to read four vtable slots (Present,
ResizeBuffers, SetColorSpace1, ExecuteCommandLists), then release all three.
That is a common pattern in overlay code and it is fine on a clean machine.

With ReShade in front it is not. ReShade's `dxgi.dll` redirects every
`D3D12CreateDevice` in the process and offers each device to its addons. RenoDX
attaches its shader replacements to the first device it sees and keeps state
against it. The first device it saw was ours. We released it a moment later.
When the game created its real device, RenoDX did not re-attach; its 429 shader
replacements and 42 injections stayed bound to a device that no longer existed,
and the first frame that went through them dereferenced null. The crash was
the game's, on RenoDX's behalf, caused by us.

Whether it dies or hangs at the loading screen turned out to be timing. Both
happened here on the same setup, which is why the reports disagreed.

## The fix

No throwaway device, ever. The vtable slots are read from the game's own
swapchain and command queue the first time it creates them, inside the
`CreateSwapChainForHwnd` factory hook the mod already had, and the detours go in
at that moment (`InstallDetoursFrom` in `dx12_hook.cpp`). Vtables belong to the
class, so on a clean machine the addresses are the ones the throwaway would
have produced. On a machine with a proxy in front they belong to the proxy,
which brings in the second change.

Ownership is judged by path, never by module name. ReShade's proxy is called
`dxgi.dll`, the same as the system one, so a name check would have detoured
into ReShade's own code. `ModuleInSystemDir` resolves the module containing an
address and compares its directory to `GetSystemDirectoryA`; anything else with
a system name is another mod's and is left alone.

Two things came out of the hunt and stayed:

- DRED is off unless `EnableDred=1`. It was armed before the game had a device
  and nothing in looting needs it. Not the cause, but it was the other thing in
  the window and there was no reason to keep it on by default.
- `HookDX12=0` stays as a bisect switch. One launch splits a load order problem
  into the render side and everything else.

Confirmed on the reporter's load order: the game loads, the menu draws, and
RenoDX's log shows `OnInitDevice` against the game's device at 15:59:10, one
second after the game's `D3D12CreateDevice` and with none of ours before it.

## Reusable

- Never create a device, queue or swapchain you do not intend to keep. Anything
  that hooks device creation will bind to it.
- ReShade at log level 4 says which device every addon attached to and when.
  That is the fastest way to tell whether an addon crash is yours.
- When a crash is inside the game with nothing of yours on the stack, bisect by
  subsystem before reading disassembly. The switch that isolates the render
  layer cost twenty lines and answered in one launch what three days of
  stack-reading had not.
- The Steam overlay, Crimson Route and ReShade all detour Present. On the test
  machine Crimson Route was first on three of the four functions. The log's
  hook lines follow the jump and name the module, which is what a reporter
  should be asked to paste.
