# Drawing the menu without hooking Direct3D

Research, 10 September 2026. Nothing in here is built. It answers one
question: what else could put the menu on the screen, now that four releases
in a row have gone into keeping the Direct3D 12 hooks alive next to other
mods' Direct3D 12 hooks.

Every claim below that comes from outside this repository is checked against
the source listed at the end. On the first pass learn.microsoft.com,
nexusmods.com and reshade.me were out of reach and the text said so at each
point. Later the same day a browser session got to all of them. What follows
carries what those pages actually say.

## What the menu costs today

The overlay sits on five things that every other overlay in the load order
also wants.

- The `CreateSwapChainForHwnd` slot in the DXGI factory's vtable, replaced by
  `InstallSwapChainCreationPatch` in `dx12_hook.cpp`. The vtable belongs to the
  class, so there is one slot for the whole process, and Crimson Route, Trinity
  and NVIDIA's frame generation layer `sl.interposer.dll` all patch the same
  one. Issue #47 is three days of finding out which order they can stack in.
- `Present`, `Present1`, `ResizeBuffers` and `SetColorSpace1` on the swapchain,
  and `ExecuteCommandLists` on the command queue, detoured with MinHook. Code
  detours are worse than vtable patches: two mods writing a jump into the same
  prologue is what the `ExistingDetour` and stacking logic exists to survive.
- The swapchain object itself, wrapped so the overlay can draw before frame
  generation interpolates. The wrapper is what Crimson Route keyed its own
  command queue on when it sat above this mod, and what killed the game on a
  frame generation toggle in issue #30.
- The game window's procedure, subclassed in `input.cpp`, plus a detour on
  `GetCursorPos` and a virtual cursor fed from raw input, because the game
  clips and recentres the OS cursor from more than one thread.
- `XInputGetState` in three DLLs, so the pad goes quiet while the menu is open.
  Trinity detours the same export.

The history is in the issues. #5 and #34 were the throwaway device: RenoDX
bound its shader replacements to a device this mod created and released, and
the game died on the first frame. #30 was the wrapper holding back buffer
references through a swapchain teardown. #47 was Crimson Route's 100 by 100
probe chain being taken for the game's, then Route keying the wrapper, then a
re-assert that climbed above `sl.interposer.dll` and ended the process without
an exception. #17, Crimson Weather, is still open with no log. Each fix has
been correct and each has been about one more neighbour.

The shape of the problem is fixed by NVIDIA's own rule. Section 5.2 of the
programming guide in the `NVIDIA-RTX/Streamline` repository says overlays
must not assume anything about swapchains and queues when frame generation is
active, and that they should intercept `IDXGIFactory::CreateSwapChainXXX` to
learn the real ones. This mod follows that rule and so does Route, which is
why they meet on the same slot. Trinity, from whose code this mod's hook
descends, still reads its addresses off a throwaway device and detours
`Present`, `ResizeBuffers` and `ExecuteCommandLists`, per its README. There is
no arrangement of hooks that makes N mods on one class vtable safe; there is
only handling the next neighbour.

There is a sixth neighbour the issues have not met yet. CrimsonDesertTelemetry,
Nexus 3374 by fabianviol, MIT and on GitHub, is the ASI that DesertLink and
any other companion map depends on for the player position, and since 2.0.0 it
draws a HUD of its own over Direct3D 12. Its `overlay_graphics.cpp` creates a
factory with `CreateDXGIFactory1`, reads slots 10 and 15 off it, and puts
MinHook detours on the functions they point to, which is a code patch on the
DXGI implementation of `CreateSwapChain` and `CreateSwapChainForHwnd` rather
than a slot replacement, so it sits under every vtable patch and behind
whichever detour reached the prologue first. From there it detours `Present`,
`Present1`, `ResizeBuffers`, `ResizeBuffers1` and `SetColorSpace1`, and
`render_capture.cpp` creates a probe queue on the game's device to read
`ExecuteCommandLists` and detours that too. Seven targets, the same seven.
It pins Dear ImGui 1.91.9b and carries an HDR compositor of its own. Its
validation notes say that other overlays, DXGI wrappers and frame generation
"need live coexistence testing", which is to say nobody has run it beside
this mod, Route or Trinity on purpose. No issue names it yet.

One dependency is easy to miss. The menu key is read inside the render path
and the capture gate in `state.h` opens only while frames are being drawn, so
when nothing draws there is no menu and no way to notice. The twenty second
watchdog in `mod.cpp` exists because of that.

## The candidates

### 1. A window of the mod's own, painted with GDI

Create a second top-level window, owned by the game window, and paint the
menu into it with per-pixel alpha. No Direct3D call is made at any point, so
there is nothing for ReShade, RenoDX, Route, Trinity or the frame generation
layer to hook, wrap, key or bind to.

How it would go together:

- `CreateWindowExW` with `WS_EX_LAYERED | WS_EX_TOOLWINDOW`, style `WS_POPUP`,
  and the game's HWND as the owner. The Win32 window features page says an
  owned window is always above its owner in the z-order, is hidden when the
  owner is minimised and is destroyed with it. No `WS_EX_TOPMOST` needed, and
  `WS_EX_TOOLWINDOW` keeps it out of the taskbar and the alt-tab list.
- The window follows the game's client rectangle. `SetWinEventHook` for
  `EVENT_OBJECT_LOCATIONCHANGE` on the game's thread is the clean way; a poll
  of `GetClientRect` at the redraw rate is the cheap way. Neither patches
  anything.
- Dear ImGui stays exactly as it is in `menu.cpp`. Only the backend changes:
  `imgui_impl_win32` runs on the mod's window instead of a subclass of the
  game's, `imgui_impl_dx12` goes, and the draw lists are rasterised on the
  CPU into a 32-bit premultiplied DIB that `UpdateLayeredWindow` hands to the
  compositor. emilk's `imgui_software_renderer` is public domain, two files,
  reports one to ten milliseconds for a complex GUI on a laptop, and paints
  only the font texture. This menu draws text and shapes and no images, so
  that is enough. It is from 2018 and will need a small update for the 1.91
  draw list layout.
- Redraw only while something is visible. The menu at 30 or 60 Hz while open,
  the bag-full notice once per change of text, nothing at all otherwise. The
  CPU cost is zero while the menu is closed, which is nearly always.
- Input becomes ordinary. Insert shows the window and calls
  `SetForegroundWindow`, which a process that already owns the foreground is
  allowed to do. Keyboard and mouse then arrive at the mod's window as normal
  messages. The game, no longer foreground, stops receiving keyboard messages
  without anyone withholding them. Escape or Insert hides the window and gives
  the foreground back. The subclass, the `GetCursorPos` detour, the raw input
  cursor and the 500 millisecond capture gate all go.
- Watch mode and the notice are the same window with `WS_EX_TRANSPARENT` and
  `WS_EX_NOACTIVATE` set. The window features page: hit testing of a layered
  window follows its shape and alpha, so zero-alpha pixels let the mouse
  through, and `WS_EX_TRANSPARENT` passes every mouse event to the windows
  underneath. That is the current watch mode with no code behind it.
- HDR takes care of itself. The window is an SDR surface composed by the
  desktop, and Windows decides how bright SDR content is on an HDR display.
  The 500 line composite shader in `hdr_composite_shader.h` has no job left.
- Frame generation takes care of itself. The compositor draws the window over
  whatever the game presents, interpolated frames included.

Two shipped mods already put a window over this game from outside the
process, so most of the questions below have been asked once. DesertLink is an
Electron app; its `app/main.js` makes a frameless `BrowserWindow` the size of
the display, sets it always on top at the `screen-saver` level, visible over
fullscreen windows and out of the taskbar, and shows and focuses it on a
global hotkey, End by default. Crimson Atlas, Nexus 3381, is a resizable
always-on-top window with the same kind of hotkey, and its page says: "For
the best experience, run Crimson Desert in windowed or borderless mode.
Exclusive fullscreen mode may cover external always-on-top windows." Neither
has a report on its first page of posts of the game losing input when the
window takes focus. Both are separate processes whose windows are topmost
strangers to the game; the one proposed here would be owned by the game
window from inside the process, which is the shape dofo7777 suggested to the
Atlas author on 3 September: "a borderless window attached to the game
process, so it can be shown or hidden with a hotkey."

What it costs:

- Independent flip. Microsoft's flip model page, on what happens when a
  window is covered:

  > If other desktop contents come on top, the DWM can either seamlessly
  > transition back to composed mode, efficiently "reverse compose" the
  > contents on top of the application before flipping it, or leverage MPO to
  > maintain the independent flip mode.

  PCGamingWiki says the same from the other side: direct flip holds as long as
  nothing external sits on top of the game, and overlays that inject into the
  game are fine. So while the menu is open the game may be composed by DWM,
  which is a frame of latency and a little GPU, and it returns to independent
  flip when the window hides. On hardware with multi-plane overlay support the
  DWM keeps independent flip and the cost is nothing. For the two and a half
  second notice this means a brief mode switch each time it appears, which is
  a reason to make the notice optional, as it already is.
- Exclusive fullscreen. A window cannot be seen over a true exclusive
  swapchain. Microsoft's own account is "Demystifying Fullscreen
  Optimizations" on the DirectX developer blog. Since Windows 10 an exclusive
  fullscreen game "believes that it is running in Fullscreen Exclusive, but
  behind the scenes, Windows has the game running in borderless windowed
  mode". When an overlay such as the Game Bar appears, "the DWM reassumes
  control of the display, and a slight performance overhead is incurred". The
  switch that turns this off is on the exe's compatibility tab. The game offers
  Fullscreen, Borderless and Windowed. A player who has ticked that box, or
  who is on Proton (issue #15), is the case to test. When the window cannot
  be seen, nothing draws and looting carries on, which is what happens today
  when another mod owns `Present`.
- Focus. The game keeps running when it is not the foreground window; the
  Steam threads on alt-tab agree on that. The thread to read is "Game loses
  focus during combat / Window unresponsive until resized", opened in April
  2026 with replies through July, and its shorter twin "Losing mouse focus
  when using Alt key" from August. Mid-combat the Windows cursor appears, the
  game keeps rendering and playing sound, and it ignores input; alt-tab and
  clicking the taskbar do not bring it back, dragging the frame or toggling
  Fullscreen and Windowed does. Every poster ties it to Alt combinations, one
  cured it by disabling Alt+W and Alt+A, S and D in PowerToys, and one had
  the same thing in Black Desert from the Start menu search taking focus. The
  trigger, then, is something else taking the foreground during play. The
  menu window would do exactly that. The recovery is a frame change, which
  the mod could send itself if it has to. Whether it has to is the first
  thing to test, and it is a test of the game: a window on a second monitor
  does the same to it.
- The cursor. `input.h` documents that the game clips and recentres the OS
  cursor every frame from more than one thread. If it keeps doing that while
  another window of the same process is foreground, the menu's cursor fights
  it. Windows routes mouse messages to the window under the pointer and
  keyboard messages to the window with focus, so the game would receive
  neither while the pointer is over the menu; whether it keeps clipping
  while unfocused is something nobody has watched for.
- Stray clicks. Zero-alpha pixels pass the mouse through to the game, which
  is right for watch mode and wrong for the interactive menu, where a click
  beside the window would reach the game. Size the window to the menu, or
  give the margin a faint background so the click stops at it.
- The pad. `EnsureRawInput` already logs the flags the game registered raw
  mouse input with; if `RIDEV_INPUTSINK` is among them the game gets mouse
  input while unfocused and that needs handling. Whether it reads the pad
  while unfocused is unknown. The XInput detours can stay for now; they are a
  smaller shared surface than the Direct3D ones and they are optional already.

What it removes: `dx12_hook.cpp` at 2,080 lines, the composite shader at 503,
`input.cpp` at 212, the `WrapSwapChain`, `HookDX12` and `EnableDred` switches,
the Trinity adaptation script and the DirectX 12 credits. What it adds is one
window, one rasteriser and a rect tracker, a few hundred lines.

### 2. A window of the mod's own, painted with Direct3D

The same window, but with a real GPU backend: a D3D11 device and a swapchain
created through `CreateSwapChainForComposition` with premultiplied alpha, or a
plain swapchain on a `WS_EX_NOREDIRECTIONBITMAP` window, the way Kenny Kerr
described in MSDN Magazine in June 2014. Dear ImGui's own D3D11 backend, no
CPU rasterisation, any texture.

It keeps the exposure that option 1 removes. Creating a device and a
swapchain is exactly the act that hooks bind to: `DXHOOKS.md` records RenoDX
attaching to the first device it saw, and #47 records Route's probe swapchain
being mistaken for the game's by this mod. A second swapchain from this mod
would go through the same patched factory slot that Route, Trinity and the
frame generation layer are watching, and one of them will key on it the way
this mod keyed on Route's. The lesson in `DXHOOKS.md` is never to create a
device you do not keep; this one would be kept, and ReShade would still see it
first if the mod's window came up before the game's device. The D3D12
variant, drawing into the mod's window from the game's own device and queue,
still needs the queue, which means the factory patch stays. Faster than
option 1, and not safer than today.

Two more things about it from the sources. Kerr's article says a
`WS_EX_NOREDIRECTIONBITMAP` window "hit tests uniformly", because the input
subsystem does not know the client area is transparent; per-pixel
fall-through is a layered-window feature, done on the CPU, which is the price
option 1 pays and option 2 does not. Watch mode and the notice would need
`WS_EX_TRANSPARENT` on the whole window instead. And Crimson Route already
ships this design as its fallback: `CrimsonRoute.ini` says "Present is
recommended; DirectComposition is the compatibility option", with a restart
to switch. Route is closed source. Whether that path creates a device of its
own, and how it has fared beside RenoDX and the interposer, only dofo7777 can
say. Ask before option 2 is ruled out on the reasoning above alone.

### 3. Draw through ReShade when ReShade is there

ReShade with add-on support exposes its own Dear ImGui to add-ons, and the
API turns out to be reachable from an ASI. From `include/reshade.hpp` in
crosire's repository, API version 20: `register_addon` finds ReShade by
walking the loaded modules with `K32EnumProcessModules` looking for the
`ReShadeRegisterAddon` export, so any module in the process can register, not
only DLLs ReShade loaded itself. `register_overlay(title, callback)` gives a
window drawn with ReShade's ImGui through a function table, and
`effect_runtime` offers `block_input_next_frame`, `is_key_down`,
`is_key_pressed`, `get_mouse_cursor_position` and `open_overlay`. Events
include `present`, `finish_present` and `reshade_overlay`.

The limits are hard ones.

- Only ReShade "with full add-on support" carries the API. RenoDX users have
  that build by definition, since RenoDX requires 6.8.0 or later with add-on
  support. Nobody else does.
- The function table is pinned to one ImGui: `reshade_overlay.hpp` refuses to
  compile unless `IMGUI_VERSION_NUM` is 19250, that is 1.92.5. This mod builds
  against 1.91.5-docking, so the build would carry two ImGui versions.
- The table has `PushFont`, `PopFont` and `GetFont` but nothing that adds a
  font to the atlas, so the serif face and the CJK fallback merge in
  `menu.cpp` cannot be done; the Chinese and Portuguese menus would depend on
  ReShade's glyph ranges. `imgui_internal.h` is not in the table either, and
  the group checkboxes use `ImGuiItemFlags_MixedValue` from it.
- The overlay callback runs "when the overlay is visible", which is ReShade's
  overlay session. `open_overlay` can open it from Insert, but the menu would
  appear beside ReShade's own windows.
- The internals are reachable, pinned to a commit. The reshade.me thread
  "ReShade API does not allow access to ImGui context" asked for the context
  to read window rectangles. crosire's answer: no official way, because he
  still hopes to support several ImGui versions through the table. The
  pointer can be recovered anyway by subtracting the offset of `IO` in
  `ImGuiContext` from what `GetIO` returns, provided the add-on is built
  against the exact docking commit ReShade was. The asker's struct was 224
  bytes off until he matched it. That is the version lock stated a second
  way: anything past the function table binds the mod to one ReShade build.

As a coexistence mode for the RenoDX crowd, behind a switch, it has a real
upside: on those machines ReShade already owns the present path and already
draws its own UI correctly under HDR and frame generation. As the only path it
would strand everyone else. It only makes sense next to option 1 or the
current code, never instead.

### 4. One overlay for the whole load order

The conflicts exist because four mods each bring their own Direct3D layer.
The fix that removes the cause is one layer that the others draw through.
There is precedent wherever a modding scene has grown up: ReShade's
`register_overlay`, REFramework's `on_draw_ui` and plugin API, UE4SS's
`add_gui_tab` behind `on_ui_init`. Each has one owner of the swapchain and
many clients drawing widgets into a shared context.

What it would be here: a `CrimsonOverlay.asi` that does the factory
interception NVIDIA prescribes, once, with every rule this repository has paid
for already built in: no throwaway device, the probe chain guard, the queue
pinned from creation, stacking under Steam and under `sl.interposer.dll`. A C
ABI of a handful of functions: register a draw callback with a version, an
ImGui function table in the ReShade style so clients need not share a build,
and input arbitration so one client at a time captures the keyboard. This
mod's `dx12_hook.cpp` is a plausible seed for it, since it is MIT and already
carries the scars.

dofo7777 has already done the cooperative thing once, and quickly. On 2
September 2026 he asked the Crimson Atlas author on the Atlas posts tab
whether a local API would be useful. Route 6.9.3 shipped it that evening at
`http://127.0.0.1:17893/`; 6.9.4 fixed the first bug report the next morning.
On 3 September he wrote that 6.9.5 "added an option for API mode to hide
screen overlays, so Crimson Route no longer needs to install graphics hooks."
Route's page carries no changelog, so that is his post and not a
release note, but it changes #47 on its own: a Route user who draws routes in
Atlas can already run Route with no Direct3D hooks at all.

The problem is not technical. It works only if Crimson Route and Trinity
adopt it, and two hosts in one load order is the original conflict again with
extra steps. The people are known: Route is dofo7777's, per the Nexus page,
and dofo7777 did the Chinese translations of this menu within an hour of the
template going up. Trinity is XeTrinityz's and MIT, and this mod's hook code
is theirs by descent. The build on Nexus today, 0.14.59, is a compatibility
fork maintained by slingblade2047. That makes two people for Trinity. That is a better starting position than most such
proposals have. It is also months, not days, and it does nothing for the
next release. Worth opening as a conversation alongside whichever of the
other options ships.

### 5. No menu in the game at all

`Settings::Poll` already reloads `MasterLooter.ini` within a second of a
change on disk, and a preset is an ini file in a folder. Everything on the
General, Looting, Classes, Tags and Items tabs is a write to that file. A
configurator outside the game, a small executable or a page in the browser,
could own all of it with no code inside the game process at all.

DesertLink shows the shape in this very game. Its page describes an App mode
for a second monitor and an Overlay mode above the game, and the source
confirms it is an Electron window fed by CrimsonDesertTelemetry over loopback
HTTP and WebSocket on port 27311, a `/v1/snapshot` and a `/v1/stream`, with
teleporting through a second ASI, DesertLinkCore, over local IPC. A status
file rewritten every second, or a local socket, would carry the Nearby and
Status tabs the same way. Three loopback ports are taken in this scene already:
27311 for Telemetry, 17893 for Route's API, and 7891 for Atlas, which one user
found by having something else on it.

What would stay in the game: the hotkeys, which are `GetAsyncKeyState` and
need no hook, and the bag-full notice, which needs a few words on screen and
would be the smallest possible form of option 1, or a sound. The plugin's
import list would lose `d3d12`, `dxgi` and `d3dcompiler_47`, and two of the
behaviours the README names as what the antivirus models react to, drawing
over Direct3D 12 and reading the keyboard before the game, would be gone.
Whether the count of three scanners moves is not something anyone can
predict.

The cost is the alt-tab. A menu you reach with one key while standing over an
ore vein, with the verdict for the item under your feet, is the thing people
praise; a configurator is the thing they set up once. The Nearby tab loses
its immediacy and the pad loses the menu entirely. A page served from the
plugin over localhost is the least of it, since it needs no second install,
but it is still a browser.

### 6. If the hooks stay

Two refinements from the sources, for completeness. Section 5.3 of the same
NVIDIA guide gives third parties a way to tell a frame generation proxy from
the real interface: `QueryInterface` with the GUID
`{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}` returns the native object from a
proxy and null from anything else. That would replace the module-name
heuristics in `LogHookTarget` with an answer from the layer itself. The
`eUseDXGIFactoryProxy` preference flag, which stops the layer patching the
factory vtable, is set by the game at `slInit` and is out of reach.

Nobody in the load order uses that GUID. The nearest thing is Telemetry's
`tools/StreamlineProbe`, an external debugger that watches `slSetConstants`
from outside the process, which is a research tool and not a coexistence
check.

Neither changes the arithmetic. Every refinement handles one more neighbour,
and the next mod on Nexus is one more neighbour.

### 7. Not researched

Drawing through the game's own UI system. The engine is Pearl Abyss's own,
per Wikipedia, and no public UI middleware has been found behind it. It would
mean reverse engineering the UI layer from the exe, which is a project of the
size of the loot engine, for a menu that would then break on every patch the
way the loot signatures do. Not pursued.

## Where this lands

Option 1 is the one to build. It removes every shared surface that the
issues in this repository were about, it keeps the menu inside the game with
the same key and the same code in `menu.cpp`, and it fails safe: a window
that cannot be seen costs the menu and nothing else, which is the failure
mode the watchdog already handles. Its risks are all testable in an afternoon
on the machine here, and none of them is a crash.

Keep the Direct3D path in the tree behind `HookDX12=1`, default off, for a
release or two. Somebody will want the in-frame overlay under frame
generation, and the code is paid for. Delete it when the log stops showing
anyone turning it on.

Open option 4 as a conversation with dofo7777 and XeTrinityz once option 1
has shipped and the mod is no longer part of the problem it is proposing to
fix. Option 3 is a switch for later if RenoDX users ask for it. Option 5 is
the fallback if option 1 fails its tests.

## Tests before option 1 is committed to

In this order, each one launch.

1. Borderless. Insert shows the window over the game, takes clicks, Escape
   returns focus, and the game responds to input again. Then the same in
   Fullscreen with fullscreen optimisations on, then with them disabled in
   the exe's compatibility properties.
2. The game's focus bug. Twenty open-and-close cycles in combat, with Alt
   held on some of them since every Steam report involves it. If the game
   stops taking input when focus comes back, that is the April thread. Try
   the players' own recovery from code, a `SetWindowPos` frame nudge on the
   game window as the menu closes, before calling option 1 dead on that
   machine.
3. The cursor. With the menu open, watch whether the OS cursor is being
   recentred by the game. `input.h` says it does this from more than one
   thread while foreground.
4. Raw input flags. Read the `[input] game registered raw mouse input` line
   the log already prints. `RIDEV_INPUTSINK` present means the game sees the
   mouse while the menu is up.
5. PresentMon with the menu open and closed, to see whether the game leaves
   independent flip and whether it comes back.
6. HDR on. Read the menu next to the game's highlights and decide whether the
   desktop's SDR brightness setting is acceptable or whether the notice needs
   its own alpha.
7. The full load order from #47: Route, Trinity, Steam's overlay, frame
   generation on, then RenoDX under ReShade from #34, then Telemetry with its
   HUD enabled, which no issue has covered. Expected result is that
   the mod's log has no `[hook]` lines at all and nothing to say about any of
   them.
8. Proton, if anyone on issue #15 will run it.

## Continuing this locally

This was written in a cloud session whose proxy refused the connection to
learn.microsoft.com, devblogs.microsoft.com, nexusmods.com and reshade.me,
and it listed four things for a local session with a browser to read. That
session ran on 10 September and read all four, and the findings are folded
into the sections above. For the record:

1. The reshade.me thread does not mention fonts at all; it is about reaching
   the ImGui context, and crosire's answer pins that to one commit. The
   font-atlas limit stands on the header alone, and nothing in the thread
   offers a way round it.
2. `CrimsonRoute.ini` in the game folder: `Renderer=Present` with
   `DirectComposition` as "the compatibility option", `HideOverlay=0`, and an
   `[API]` section, `Enabled=0`, `Port=17893`. On other overlays Route's page
   says nothing; its one compatibility note covers ReShade and OptiScaler. Trinity's Nexus page has
   no compatibility section; it is the slingblade2047 fork's install and
   changelog text.
3. DesertLink's Overlay mode is an Electron window, always on top, focused on
   its hotkey; Crimson Atlas is the same shape and its page warns about
   exclusive fullscreen. Neither posts tab reports a focus problem on its
   first page.
4. The DirectX blog post and Kerr's article are quoted above where they
   apply.

Still unread: the Atlas posts tab past its first page. Route's changelog is
not on its page at all. Route's DirectComposition path is closed source and
only its author can describe it.

The raw files that were read directly, the ReShade headers, both NVIDIA
guides, Trinity's README and the three Microsoft pages from the docs mirror,
can be fetched again with `fetch_overlay_sources.py` in this folder. It puts
them in `docs/investigations/sources/`, which is ignored by git, and prints
the lines the text above cites so they can be checked without opening each
file.

The eight tests in the previous section need the game and are the other half
of continuing this.

## Sources

- This repository: `mod/src/hooks/dx12_hook.cpp`, `input.cpp`, `input.h`,
  `xinput_hook.cpp`, `core/state.h`, `core/mod.cpp`, `core/settings.cpp`;
  `docs/investigations/DXHOOKS.md` and `FRAMEGEN.md`; issues #5, #14, #15,
  #17, #30, #34, #47.
- NVIDIA, `docs/ProgrammingGuide.md` in the `NVIDIA-RTX/Streamline` repository
  on GitHub, sections 5.2 "SL and third party overlays" and 5.3 "How to check
  if SL proxies are used", and the `eUseDXGIFactoryProxy` preference flag.
- Trinity by XeTrinityz, https://github.com/XeTrinityz/Trinity, README:
  throwaway device, MinHook on `Present`, `ResizeBuffers` and
  `ExecuteCommandLists`, `XInputGetState` detour, toggle polled from the render
  loop, no plugin API.
- Microsoft Learn, "Window Features" (Win32): owned windows (above the owner,
  destroyed with it, hidden when it is minimised), layered windows, and the
  hit-testing paragraph, which says that colour-keyed or zero-alpha areas let
  mouse messages through and that `WS_EX_TRANSPARENT` passes them under the
  whole window regardless of shape. "Extended Window Styles" for
  `WS_EX_NOACTIVATE`, `WS_EX_TOOLWINDOW`, `WS_EX_TRANSPARENT` and
  `WS_EX_NOREDIRECTIONBITMAP`. `SetForegroundWindow` remarks: a process may set
  the foreground window when it is the foreground process. All read directly
  on the second pass.
- Microsoft Learn, "For best performance, use DXGI flip model": DirectFlip,
  independent flip, and the sentence on other desktop content coming on top,
  quoted above as it stands on the page.
- PCGamingWiki, "Windows" and "Glossary: Windowed": fullscreen optimisations
  since Windows 10 1607, and direct flip holding while nothing external sits on
  top. Microsoft, "Demystifying Fullscreen Optimizations", DirectX developer
  blog, read directly: the primary source for the two quotations above.
- Kenny Kerr, "High-Performance Window Layering Using the Windows Composition
  Engine", MSDN Magazine, June 2014, read in the Microsoft Learn archive:
  uniform hit testing without a redirection surface; `IDXGIFactory2::CreateSwapChainForComposition`
  on Microsoft Learn, whose remarks say `SetFullscreenState`, `ResizeTarget`,
  `GetContainingOutput` and `GetHwnd` fail on such a chain.
- emilk, `imgui_software_renderer` on GitHub: dual-licensed to the public
  domain, font texture only, one to ten milliseconds for a complex GUI, seven
  on the author's laptop. Last commit 12 July 2018.
- crosire, ReShade, `include/reshade.hpp`, `reshade_api.hpp`,
  `reshade_events.hpp` and `reshade_overlay.hpp` at main, and `REFERENCE.md`.
  The reshade.me thread "ReShade API does not allow access to ImGui context",
  dfw and crosire, add-ons discussion, read in full.
- RenoDX wiki and Creepy's HDR guides: RenoDX requires ReShade 6.8.0 or later
  with full add-on support.
- REFramework documentation, `re.on_draw_ui`; UE4SS documentation, "GUI tabs
  with a C++ Mod".
- Nexus Mods pages for Crimson Route (3175), Crimson Atlas (3381), DesertLink
  (3444) and CrimsonDesertTelemetry (3374), read directly, with the first page
  of the Atlas posts tab for dofo7777's replies of 2, 3 and 5 September 2026.
  `app/main.js` in `DesertLink/DesertLink-Source` on GitHub for the overlay
  window. `overlay_graphics.cpp`, `render_capture.cpp` and
  `docs/OVERLAY_VALIDATION.md` in `fabianviol/CrimsonDesertTelemetry` for its
  hooks.
- `CrimsonRoute.ini` beside the game, Route 6.9.6, for the renderer and API
  keys. Trinity's Nexus page, 3273, docs tab.
- Steam community, Crimson Desert general discussions: "Game loses focus
  during combat / Window unresponsive until resized" (April to July 2026) and
  "Losing mouse focus when using Alt key" (August 2026).
- PCGamingWiki, "Crimson Desert": windowed and borderless both listed, HDR
  listed, DLSS, FSR and XeSS frame generation listed.
- Wikipedia, "Crimson Desert": developed in Pearl Abyss's proprietary engine.
