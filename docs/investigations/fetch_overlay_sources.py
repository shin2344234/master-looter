"""Fetch the sources OVERLAY.md was checked against, and print the lines it cites.

  py -3 docs/investigations/fetch_overlay_sources.py

Every file lands in docs/investigations/sources/, which .gitignore leaves out
of the repository because none of it is ours: ReShade's headers are BSD, the
NVIDIA guides and Trinity are MIT, the Microsoft pages are CC-BY from the
MicrosoftDocs/win32 mirror on GitHub. The mirror is used because
learn.microsoft.com itself was unreachable from the session that wrote the
document, and it is what the document quotes.

After downloading, each file is searched for the phrases OVERLAY.md relies on
and the hits are printed with line numbers, so a claim can be checked against
its source without opening anything. A phrase marked "expect none" is one the
document says is absent; a hit there means the source moved and the text is
out of date.

Only the standard library. Goes through HTTPS_PROXY when one is set, which is
how the cloud session reached GitHub.
"""
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "sources")

RAW = "https://raw.githubusercontent.com/"

# (local name, [candidate URLs in order], [(phrase, expect_none)])
#
# Two candidates where the default branch name was not confirmed; the first
# that answers 200 wins.
SOURCES = [
    ("reshade.hpp",
     [RAW + "crosire/reshade/main/include/reshade.hpp"],
     [("define RESHADE_API_VERSION", False),
      ("K32EnumProcessModules", False),
      ('"ReShadeRegisterAddon"', False),
      ("inline void register_overlay", False),
      ("when the overlay is visible", False)]),
    ("reshade_api.hpp",
     [RAW + "crosire/reshade/main/include/reshade_api.hpp"],
     [("block_input_next_frame", False),
      ("is_key_down", False),
      ("is_key_pressed", False),
      ("get_mouse_cursor_position", False),
      ("open_overlay", False)]),
    ("reshade_events.hpp",
     [RAW + "crosire/reshade/main/include/reshade_events.hpp"],
     [("reshade_overlay,", False),
      ("finish_present", False)]),
    ("reshade_overlay.hpp",
     [RAW + "crosire/reshade/main/include/reshade_overlay.hpp"],
     [("IMGUI_VERSION_NUM !=", False),
      ("PushFont", False),
      ("AddFont", True),
      ("ImFontAtlas", True)]),
    ("reshade_REFERENCE.md",
     [RAW + "crosire/reshade/main/REFERENCE.md"],
     [("register_overlay", False)]),
    ("sl_ProgrammingGuide.md",
     [RAW + "NVIDIA-RTX/Streamline/main/docs/ProgrammingGuide.md"],
     [("THIRD PARTY OVERLAYS", False),
      ("CreateSwapChainXXX", False),
      ("eUseDXGIFactoryProxy", False),
      ("ADEC44E2", False)]),
    ("sl_ProgrammingGuideDLSS_G.md",
     [RAW + "NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_G.md"],
     []),
    ("trinity_README.md",
     [RAW + "XeTrinityz/Trinity/main/README.md",
      RAW + "XeTrinityz/Trinity/master/README.md"],
     [("throwaway", False),
      ("MinHook", False),
      ("XInputGetState", False)]),
    ("win32_window-features.md",
     [RAW + "MicrosoftDocs/win32/docs/desktop-src/winmsg/window-features.md"],
     [("always above its owner", False),
      ("hidden when its owner is minimized", False),
      ("Hit testing of a layered window", False),
      ("can be used with child windows and top-level windows", False),
      ("The calling process is the foreground process", False)]),
    ("win32_extended-window-styles.md",
     [RAW + "MicrosoftDocs/win32/docs/desktop-src/winmsg/extended-window-styles.md"],
     [("WS_EX_NOACTIVATE", False),
      ("WS_EX_TOOLWINDOW", False),
      ("WS_EX_NOREDIRECTIONBITMAP", False)]),
    ("win32_dxgi-flip-model.md",
     [RAW + "MicrosoftDocs/win32/docs/desktop-src/direct3ddxgi/for-best-performance--use-dxgi-flip-model.md"],
     [("If other desktop contents come on top", False),
      ("effectively equivalent", False)]),
    ("imgui_software_renderer_README.md",
     [RAW + "emilk/imgui_software_renderer/master/README.md",
      RAW + "emilk/imgui_software_renderer/main/README.md"],
     [("milliseconds", False),
      ("font texture", False)]),
]


def fetch(urls):
    """The body of the first URL that answers, or None with the errors printed."""
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            print("    %s -> HTTP %d" % (url, e.code))
        except Exception as e:  # network, TLS, proxy
            print("    %s -> %s" % (url, e))
    return None


def cited_lines(text, phrase):
    return [(i + 1, line) for i, line in enumerate(text.splitlines()) if phrase in line]


def main():
    os.makedirs(OUT, exist_ok=True)
    missing = 0
    for name, urls, phrases in SOURCES:
        path = os.path.join(OUT, name)
        print("== %s" % name)
        body = fetch(urls)
        if body is None:
            print("    not fetched")
            missing += 1
            continue
        with open(path, "wb") as f:
            f.write(body)
        text = body.decode("utf-8", "replace")
        print("    %d lines -> %s" % (text.count("\n"), os.path.relpath(path, HERE)))
        for phrase, expect_none in phrases:
            hits = cited_lines(text, phrase)
            if expect_none:
                if hits:
                    print("    !! %r: %d hit(s); OVERLAY.md says there are none" % (phrase, len(hits)))
                else:
                    print("    ok %r: none, as the text says" % phrase)
                continue
            if not hits:
                print("    !! %r: no hit; the source moved or the text is wrong" % phrase)
                continue
            for n, line in hits[:3]:
                print("    %5d  %s" % (n, line.strip()[:120]))
    print()
    print("done, %d source(s) not fetched" % missing)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
