"""Derive src/hooks/dx12_hook.* and xinput_hook.* from Trinity's MIT-licensed sources.

Trinity (https://github.com/XeTrinityz/Trinity) solved the hard parts for this game:
wrapping the swapchain so the overlay draws before DLSS Frame Generation, pinning the
present queue, rebuilding render targets on silent swapchain swaps, and compositing an
SDR overlay into HDR back buffers. This script copies those files and swaps Trinity's
menu, icon, logger and settings calls for Master Looter's, so the provenance stays
visible and the adaptation is repeatable.

Usage: py -3 adapt_trinity_dx12.py <path to Trinity checkout>
"""
import os
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "tools", "Trinity")
DST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "hooks")

HEADER = """// Adapted for Master Looter from Trinity (https://github.com/XeTrinityz/Trinity),
// MIT License, Copyright (c) 2026 XeTrinityz. See THIRD_PARTY_NOTICES.md.
// Changes: Master Looter namespaces, logger, settings and menu hooks; icon atlas code removed.
"""


def rd(name):
    return open(os.path.join(SRC, "src", "hooks", name), encoding="utf-8").read()


def wr(name, text):
    with open(os.path.join(DST, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("wrote", name, len(text), "bytes")


def rep(s, old, new, count=1):
    assert old in s, "missing: " + old[:70]
    return s.replace(old, new, count)


# ----------------------------------------------------------------------- dx12_hook.cpp
s = rd("dx12_hook.cpp")
s = rep(s, '#include "../core/logger.h"\n#include "../core/settings.h"\n#include "../core/state.h"\n#include "../gui/framework.h"\n#include "../gui/icons.h"\n#include "../gui/menu.h"\n',
        '#include "../core/log.h"\n#include "../core/mod.h"\n#include "../core/state.h"\n#include "../gui/menu.h"\n')
s = rep(s, "namespace trinity::hooks", "namespace ml::hooks")
s = rep(s, "static constexpr UINT             kSrvHeapSlots = 4096;", "static constexpr UINT             kSrvHeapSlots = 8;")
s = rep(s, "        Logger::EnableConsole();\n        Settings::ClaimOwnership();\n", "        ml::Mod::OnRenderProcess();\n")
s = rep(s, "ui::InitStyle(static_cast<float>(desc.BufferDesc.Height) / 1080.0f);", "gui::InitStyle(static_cast<float>(desc.BufferDesc.Height) / 1080.0f);")
# drop the icon atlas init block
s = re.sub(r"\n        // Load the game's UI icon atlases.*?ui::IconsInit\(g_device, g_srvHeap, inc, 2, kSrvHeapSlots\);\n        \}\n", "\n", s, flags=re.S)
assert "ui::IconsInit(" not in s
s = rep(s, "        ui::IconsRecordUploads(g_commandList);\n", "")
s = re.sub(r"\n        // Record any pending icon-texture uploads.*?safe to sample this same frame\.\n", "\n", s, flags=re.S)
s = rep(s, "        State& st = State::Get();\n        if (ui::PollMenuToggle())\n            st.menuOpen = !st.menuOpen;\n", "        gui::PollToggle();\n")
s = rep(s, "            ui::IconsShutdown();\n", "")
s = s.replace('L"TrinityDummyWnd"', 'L"MasterLooterDummyWnd"')
s = s.replace('L"TrinityOverlayOffscreen"', 'L"MasterLooterOverlayOffscreen"')
s = s.replace('L"TrinityOverlayCmdList"', 'L"MasterLooterOverlayCmdList"')
s = s.replace("Trinity is disabled for this session", "Master Looter's menu is disabled for this session")
assert "trinity" not in s.lower() or "Trinity" in HEADER
wr("dx12_hook.cpp", HEADER + s)

# ----------------------------------------------------------------------- dx12_hook.h
h = rd("dx12_hook.h")
h = rep(h, "namespace trinity::hooks", "namespace ml::hooks")
wr("dx12_hook.h", HEADER + h)

# ----------------------------------------------------------------------- shader header (verbatim)
wr("hdr_composite_shader.h", HEADER + rd("hdr_composite_shader.h"))

# ----------------------------------------------------------------------- xinput hook
x = rd("xinput_hook.cpp")
x = rep(x, "namespace trinity::hooks", "namespace ml::hooks")
x = rep(x, "TRINITY_XINPUT_DETOUR", "ML_XINPUT_DETOUR", 5)
# Neutralize the whole pad while the menu is open: Master Looter's menu is mouse and
# keyboard driven, so nothing on the controller should reach the game meanwhile.
x = re.sub(r"    static void Neutralize\(XINPUT_STATE\* s\)\n    \{.*?\n    \}\n",
           "    static void Neutralize(XINPUT_STATE* s)\n    {\n        const DWORD packet = s->dwPacketNumber;\n        ZeroMemory(&s->Gamepad, sizeof s->Gamepad);\n        s->dwPacketNumber = packet + 1;\n    }\n", x, count=1, flags=re.S)
assert "ZeroMemory(&s->Gamepad" in x
wr("xinput_hook.cpp", HEADER + x)
xh = rd("xinput_hook.h")
xh = rep(xh, "namespace trinity::hooks", "namespace ml::hooks")
wr("xinput_hook.h", HEADER + xh)
print("done")
