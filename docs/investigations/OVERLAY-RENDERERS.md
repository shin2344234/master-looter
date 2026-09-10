# Overlay renderers: what Crimson Route learned

Notes from dofo7777, the author of Crimson Route, sent on the Discord server
on 10 September 2026 after reading this repo's DirectX work. Route has shipped
both ways of putting an overlay on the game, a Present hook and a
DirectComposition window, and keeps both as a user setting. His observations
are quoted as sent; the framing around them is ours.

Why it matters here: since 1.6.7 this mod draws through a Present detour
stacked on other mods' detours whenever Route or any non-Steam overlay is
loaded, and that path has had less testing than the wrapper. Route's own
Present renderer is the same idea and has been through more machines.

## Present hook

> Overall, it's almost perfect.
>
> There is some route drifting when dragging the world map, but this only
> seems to happen when frame generation is enabled. I haven't found a way to
> solve it yet.
>
> Displaying a large amount of travel history, routes, or location data can
> affect FPS. I've tried to optimize this as much as possible, and under the
> default configuration, where only routes are displayed, the impact should be
> extremely small.
>
> However, if something goes wrong, it can potentially cause a severe
> performance drop. I've added safeguards so that if rendering takes too long
> or something goes wrong, the overlay will simply stop being displayed in
> order to prioritize the game's performance.
>
> It seems that very few users experience severe FPS drops with Present, and
> I'm not sure why. I've had users report that switching to DirectComposition
> completely resolves the issue, so there may be some kind of underlying
> compatibility conflict.
>
> HDR is supported.

## DirectComposition window

Route used this from 1.0.0 before moving to Present. It is a transparent,
click-through window sized to the game window.

> Performance seems to drop significantly when HDR is enabled. My guess is
> that Windows may be consuming additional resources for HDR color conversion
> and composition, although I haven't confirmed this.
>
> There is noticeably more latency. For example, when dragging the world map,
> the route visibly lags behind the map and then catches up after you stop
> dragging.
>
> It can conflict with the NVIDIA overlay and potentially other overlays,
> causing them to flicker heavily. There have also been user reports of
> conflicts with other overlays that I haven't been able to reproduce myself,
> which apparently resulted in severe performance degradation.
>
> The overlay does not appear in screenshots or recordings, depending on the
> recording software being used.

Lossless Scaling in particular breaks under DirectComposition, because the
overlay is a separate window and the frame generation runs on the game's.

## Compatibility history

Route's order of events, as he tells it: a Trinity report first, fixed; then
ReShade and OptiScaler reports, the OptiScaler one turning out to be an
OptiScaler bug; then the first Present renderer, a severe performance bug in
it, a return to DirectComposition, the bug found, and both renderers kept as
a choice since. Nobody has reported a compatibility problem to him in a long time.

One recent report is worth keeping for its ending. A user on Trinity #3273
saw Route's minimap route and HUD text fail under Present while the in-world
trail worked, and DirectComposition fixed Route but broke Lossless Scaling
whenever a route was active. dofo7777 could not reproduce it on the same
Trinity build. The user then turned off RTSS, MSI Afterburner and the NVIDIA
overlay, and everything worked under Present. The extra overlays on the
machine were the cause, not either mod. That is the same population of
programs this mod's stacked path now runs beside, and the reason a report of
"no other mods" still needs the log's `[hook]` lines naming who owns Present.

## The settings window

Route's settings have always been a separate Win32 window, now custom drawn.
Whether or not the overlay installs, the settings open. This mod's menu is
drawn inside the overlay and its keys are polled inside the overlay's draw,
so any failure of the draw path takes the keys with it, which is what every
"hotkeys do nothing" report since 1.6.5 has been. A menu that does not depend
on the game's swapchain is the structural answer, and Route is the proof it
works on this game.

## How Route's Present backend does it

dofo7777 sent the source of both backends later the same day. It is his and
stays out of this repo; the notes below are what it settles, without the
code.

Route hooks the game's chain the same way this mod's stacked path does, by
MinHook on the vtable slots for Present, Present1, ResizeBuffers,
ResizeBuffers1 and SetColorSpace1, installed from the factory callback on the
chain the game just created. One step comes first. When `sl.interposer.dll` is
loaded, Route unwraps the chain and the queue through its exported
`slGetNativeInterface`, so the hooks and the overlay are keyed on the native
DXGI objects underneath Streamline. That is the whole of why a wrapper handed
back through the factory broke it in 1.6.5: the object Route keyed at
creation was not the object it met in its Present detour.

A recreation is survived without any hook on Release. The game calls
ResizeBuffers with zero width and height before it replaces a chain, the
call fails, and the replacement follows a few milliseconds later; every
replacement in the 10 September logs shows that pair. Route's ResizeBuffers
detour releases its back buffer references before calling through, without
blocking on the GPU, and if the game's call fails it keeps them released and
rebinds on the next Present that carries a chain it does not know. This mod's
stacked path now reaches the same end from the factory hook, releasing before
the original creation runs, and rebuilds through `ReconcileSwapChain` on the
next drawn frame.

Two other habits worth copying. Route never creates a probe device, window or
swapchain of its own, which is the lesson of issue #34 arrived at
independently. Its draw path takes every lock with a try-lock and skips the
frame when it loses, so a slow overlay costs frames and never stalls the
game.

For DirectComposition, Route makes a layered, click-through window the size
of the game's, hands it a Direct2D surface, and lets Windows compose the two.
Nothing in that path touches the game's swapchain. A settings window built
the same way opens even when the overlay never installed, which is what his
users get. The trade is the one
he listed above: latency, HDR cost, and frame generation layers that only see
the game's own surface.
