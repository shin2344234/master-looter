#include "menu.h"

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue for the group checkboxes
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>       // _stricmp, for the language buttons
#include <mutex>
#include <string>
#include <vector>

#include "../core/itemdb.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../core/rules.h"
#include "../core/settings.h"
#include "../core/state.h"
#include "../core/text.h"
#include "../hooks/input.h"
#include "../hooks/xinput_hook.h"
#include "../loot/engine.h"
#include "../loot/events.h"
#include "../loot/game.h"
#include "../loot/mem.h"
#include "../version.h"

namespace ml::gui
{
    static float   g_scale = 1.0f;
    static int     g_rebindTarget = -1; // 0 menu key, 1 toggle key, 2 burst key
    static ImFont* g_fontBody = nullptr;
    static ImFont* g_fontHead = nullptr;   // serif, for the title, tabs and section names

    // The game's own language: warm near-black panels, thin bronze rules, gold
    // for what is selected or important, crimson kept for the active tab.
    static const ImVec4 kBg      (0.055f, 0.050f, 0.045f, 0.96f);
    static const ImVec4 kPanel   (0.105f, 0.095f, 0.085f, 1.0f);
    static const ImVec4 kFrame   (0.150f, 0.135f, 0.118f, 1.0f);
    static const ImVec4 kFrameHi (0.205f, 0.185f, 0.160f, 1.0f);
    static const ImVec4 kFrameAct(0.255f, 0.225f, 0.190f, 1.0f);
    static const ImVec4 kBronze  (0.470f, 0.380f, 0.230f, 0.85f);
    static const ImVec4 kBronzeDim(0.330f, 0.270f, 0.170f, 0.70f);
    static const ImVec4 kGold    (0.820f, 0.690f, 0.440f, 1.0f);
    static const ImVec4 kGoldDim (0.640f, 0.540f, 0.350f, 1.0f);
    static const ImVec4 kCrimson (0.560f, 0.115f, 0.140f, 1.0f);
    static const ImVec4 kCrimsonHi(0.720f, 0.170f, 0.190f, 1.0f);
    static const ImVec4 kText    (0.885f, 0.855f, 0.795f, 1.0f);
    static const ImVec4 kTextDim (0.560f, 0.525f, 0.470f, 1.0f);
    static const ImVec4 kGood    (0.640f, 0.760f, 0.470f, 1.0f);
    static const ImVec4 kWarn    (0.860f, 0.560f, 0.360f, 1.0f);
    static const ImVec4 kMuted   (0.560f, 0.525f, 0.470f, 1.0f);
    static ImVec4 Accent(float a = 1.0f) { return ImVec4(kCrimsonHi.x, kCrimsonHi.y, kCrimsonHi.z, a); }

    // A section heading in the serif face with a bronze rule after it.
    static void Section(const char* label)
    {
        ImGui::Dummy(ImVec2(0, 4 * g_scale));
        ImGui::PushFont(g_fontHead);
        ImGui::TextColored(kGold, "%s", label);
        ImGui::PopFont();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, a.y + 1), ImVec2(a.x + w, a.y + 1), ImGui::GetColorU32(kBronzeDim), 1.0f);
        ImGui::Dummy(ImVec2(0, 6 * g_scale));
    }

    // ImGui builds one atlas holding a fixed set of characters and draws
    // anything outside it as the face's fallback glyph. That fallback is the
    // question mark 1.3.0 showed for every line of the Chinese translation.
    // Two things were missing and either alone was enough: the range was never
    // widened past the Latin default, and Segoe UI carries no Chinese glyph to
    // widen it with.
    //
    // Rather than ask for a language's whole block, which for Chinese is some
    // twenty thousand characters and an atlas to match, ask for exactly the
    // characters the loaded translation uses. The file is already in memory, a
    // menu spends a few hundred characters of it, and the same code covers a
    // language nobody has contributed yet.
    static ImVector<ImWchar> g_glyphRanges;   // ImGui keeps the pointer until it builds
    static bool              g_fontsDirty = false;

    static void AddGlyphText(const char* s, void* builder)
    {
        static_cast<ImFontGlyphRangesBuilder*>(builder)->AddText(s);
    }

    static void BuildGlyphRanges()
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        // The language list is drawn whatever is loaded, English included,
        // so its names belong in the atlas even when no translation is on.
        int n = 0;
        if (const Text::Lang* langs = Text::BuiltIn(n))
            for (int i = 0; i < n; ++i) b.AddText(langs[i].name);
        Text::ForEachTranslation(&AddGlyphText, &b);
        g_glyphRanges.clear();
        b.BuildRanges(&g_glyphRanges);
    }

    static std::string FontsDir()
    {
        char win[MAX_PATH] = {};
        GetWindowsDirectoryA(win, MAX_PATH);
        return std::string(win) + "\\Fonts\\";
    }

    // Every font load goes through here so a file that is not there is never
    // handed to ImGui. AddFontFromFileTTF reports a missing file as a user
    // error, and since 1.91.5 that error path tries to draw a tooltip, which
    // means Begin() before the first frame and a null window read. On Windows
    // segoeui.ttf always exists so it never showed; under Proton the prefix's
    // Fonts folder has none of Microsoft's faces and the game died on the
    // first frame with the menu renderer on (issue #15).
    static ImFont* LoadFont(const std::string& path, float px, const ImFontConfig* cfg, const ImWchar* ranges)
    {
        const DWORD attr = GetFileAttributesA(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return nullptr;
        return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px, cfg, ranges);
    }

    // Merged into the face just added, so Latin keeps the font the menu was
    // drawn around and only what that font cannot supply comes from here.
    // The atlas asks for exactly the characters in use, and a merged face
    // supplies only those it has that no earlier face had, so several can
    // be merged cheaply. Four groups, the first face of each that loads:
    // the script of the loaded language first, so Japanese kanji come from a
    // Japanese face rather than the Chinese forms Microsoft YaHei draws;
    // then Hangul, Thai and Chinese, which the language list needs in
    // every build (Yu Gothic has no glyph for the simplified character in
    // the Simplified Chinese entry's name, so a Chinese face follows the
    // Japanese one); then a face for whatever Segoe UI or Georgia lacks,
    // such as Vietnamese in the serif. Ordered within a group by what suits the
    // language first and what a Windows install is likely to hold second.
    // Oversampling off: a CJK glyph is large, and sampling it twice over
    // spends atlas room for nothing at this size.
    static void MergeFallback(const std::string& fonts, float px)
    {
        static const char* kTraditional[] = { "msjh.ttc", "msyh.ttc", "mingliu.ttc", "simsun.ttc", nullptr };
        static const char* kJapanese[]    = { "meiryo.ttc", "YuGothM.ttc", "msgothic.ttc", "msyh.ttc", nullptr };
        static const char* kKorean[]      = { "malgun.ttf", "msyh.ttc", nullptr };
        static const char* kRest[]        = { "msyh.ttc", "msjh.ttc", "simsun.ttc", "malgun.ttf", "msgothic.ttc", nullptr };
        static const char* kHangul[]      = { "malgun.ttf", nullptr };
        static const char* kThai[]        = { "leelawui.ttf", "leelawad.ttf", "tahoma.ttf", nullptr };
        static const char* kChinese[]     = { "msyh.ttc", "msjh.ttc", "simsun.ttc", nullptr };
        static const char* kLatinRest[]   = { "tahoma.ttf", "arial.ttf", nullptr };

        const char* lang = Text::Language();
        const char** script = kRest;
        if      (_stricmp(lang, "zh-tw") == 0) script = kTraditional;
        else if (_stricmp(lang, "ja") == 0)    script = kJapanese;
        else if (_stricmp(lang, "ko") == 0)    script = kKorean;

        const char** groups[] = { script, kHangul, kThai, kChinese, kLatinRest };
        const char*  merged[5] = {};
        int n = 0;
        for (const char** g : groups)
        {
            for (const char** f = g; *f; ++f)
            {
                bool again = false;   // a face one group already merged has nothing left to give
                for (int i = 0; i < n; ++i) if (_stricmp(merged[i], *f) == 0) again = true;
                if (again) break;
                ImFontConfig cfg;
                cfg.MergeMode   = true;
                cfg.OversampleH = 1;
                cfg.OversampleV = 1;
                if (LoadFont(fonts + *f, px, &cfg, g_glyphRanges.Data)) { merged[n++] = *f; break; }
            }
        }
        if (!n) LOG_ERR("No font carrying the non-Latin glyphs found in %s; that text draws as question marks.", fonts.c_str());
    }

    static void BuildFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();   // drops the old ImFont objects; both handles are reassigned below
        BuildGlyphRanges();
        const std::string fonts = FontsDir();

        // Segoe UI is the face the menu was drawn around. Tahoma and Arial are
        // what a Wine prefix has instead (Proton installs Liberation Sans as
        // arial.ttf), and ImGui's built-in face is the last resort.
        const float bodyPx = 16.0f * g_scale;
        static const char* kBody[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
        g_fontBody = nullptr;
        for (const char* f : kBody)
        {
            g_fontBody = LoadFont(fonts + f, bodyPx, nullptr, g_glyphRanges.Data);
            if (g_fontBody) { if (f != kBody[0]) LOG("Menu font: %s%s (segoeui.ttf is not there).", fonts.c_str(), f); break; }
        }
        if (!g_fontBody)
        {
            LOG("Menu font: none of Segoe UI, Tahoma or Arial in %s; using the built-in face.", fonts.c_str());
            ImFontConfig d; d.SizePixels = bodyPx; g_fontBody = io.Fonts->AddFontDefault(&d);
        }
        MergeFallback(fonts, bodyPx);

        const float headPx = 20.0f * g_scale;
        static const char* kSerifs[] = { "georgia.ttf", "constan.ttf", "cambria.ttc", "times.ttf" };
        g_fontHead = nullptr;
        for (const char* f : kSerifs)
        {
            g_fontHead = LoadFont(fonts + f, headPx, nullptr, g_glyphRanges.Data);
            if (g_fontHead) break;
        }
        if (g_fontHead) MergeFallback(fonts, headPx);
        else            g_fontHead = g_fontBody;

        g_fontsDirty = false;
    }

    bool FontsNeedRebuild() { return g_fontsDirty; }
    void RebuildFonts()     { BuildFonts(); }

    void InitStyle(float scale)
    {
        g_scale = std::clamp(scale, 0.8f, 3.0f);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        BuildFonts();

        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 1.0f; s.ChildRounding = 1.0f; s.FrameRounding = 1.0f; s.GrabRounding = 1.0f;
        s.TabRounding = 0.0f; s.PopupRounding = 1.0f; s.ScrollbarRounding = 1.0f;
        s.WindowBorderSize = 1.0f; s.FrameBorderSize = 1.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f; s.TabBorderSize = 0.0f;
        s.WindowPadding = ImVec2(16, 12); s.FramePadding = ImVec2(9, 5); s.ItemSpacing = ImVec2(10, 7); s.CellPadding = ImVec2(8, 4);
        s.ScrollbarSize = 12.0f; s.GrabMinSize = 10.0f;
        s.TabBarBorderSize = 1.0f;
        ImVec4* c = s.Colors;
        c[ImGuiCol_Text]                 = kText;
        c[ImGuiCol_TextDisabled]         = kTextDim;
        c[ImGuiCol_WindowBg]             = kBg;
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0.18f);
        c[ImGuiCol_PopupBg]              = ImVec4(kBg.x, kBg.y, kBg.z, 0.98f);
        c[ImGuiCol_Border]               = kBronze;
        c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg]              = kFrame;
        c[ImGuiCol_FrameBgHovered]       = kFrameHi;
        c[ImGuiCol_FrameBgActive]        = kFrameAct;
        c[ImGuiCol_TitleBg]              = kPanel;
        c[ImGuiCol_TitleBgActive]        = kPanel;
        c[ImGuiCol_TitleBgCollapsed]     = kPanel;
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0.25f);
        c[ImGuiCol_ScrollbarGrab]        = kBronzeDim;
        c[ImGuiCol_ScrollbarGrabHovered] = kBronze;
        c[ImGuiCol_ScrollbarGrabActive]  = kGoldDim;
        c[ImGuiCol_CheckMark]            = kGold;
        c[ImGuiCol_SliderGrab]           = kGoldDim;
        c[ImGuiCol_SliderGrabActive]     = kGold;
        c[ImGuiCol_Button]               = kFrame;
        c[ImGuiCol_ButtonHovered]        = kFrameHi;
        c[ImGuiCol_ButtonActive]         = kCrimson;
        c[ImGuiCol_Header]               = ImVec4(kCrimson.x, kCrimson.y, kCrimson.z, 0.40f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(kCrimson.x, kCrimson.y, kCrimson.z, 0.60f);
        c[ImGuiCol_HeaderActive]         = kCrimson;
        c[ImGuiCol_Separator]            = kBronzeDim;
        c[ImGuiCol_SeparatorHovered]     = kBronze;
        c[ImGuiCol_SeparatorActive]      = kGold;
        c[ImGuiCol_ResizeGrip]           = ImVec4(kBronze.x, kBronze.y, kBronze.z, 0.30f);
        c[ImGuiCol_ResizeGripHovered]    = kBronze;
        c[ImGuiCol_ResizeGripActive]     = kGold;
        c[ImGuiCol_Tab]                  = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabHovered]           = ImVec4(kCrimson.x, kCrimson.y, kCrimson.z, 0.55f);
        c[ImGuiCol_TabSelected]          = kCrimson;
        c[ImGuiCol_TabSelectedOverline]  = kGold;
        c[ImGuiCol_TabDimmed]            = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(kCrimson.x, kCrimson.y, kCrimson.z, 0.7f);
        c[ImGuiCol_TabDimmedSelectedOverline] = kGoldDim;
        c[ImGuiCol_TableHeaderBg]        = kPanel;
        c[ImGuiCol_TableBorderStrong]    = kBronzeDim;
        c[ImGuiCol_TableBorderLight]     = ImVec4(kBronzeDim.x, kBronzeDim.y, kBronzeDim.z, 0.35f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.025f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(kCrimson.x, kCrimson.y, kCrimson.z, 0.45f);
        c[ImGuiCol_NavCursor]            = ImVec4(kGold.x, kGold.y, kGold.z, 0.7f);
        s.ScaleAllSizes(g_scale);
    }

    // --- toggle -------------------------------------------------------------
    static bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

    void PollToggle()
    {
        Settings::Poll(); // every frame, even when nothing is drawn
        State& st = State::Get();
        const Config& c = Settings::Get();
        static bool s_keyWas = false, s_escWas = false;

        static bool s_watchWas = false;
        const bool front = State::ForegroundIsOurs();
        const bool key = front && (KeyDown(c.menuKey) || hooks::PadChordHeld(c.padMenu));
        if (key && !s_keyWas && !st.rebindCapture)
        {
            // Insert: closed -> interactive; watching -> interactive; interactive -> closed.
            if (!st.menuOpen) { st.menuOpen = true; st.menuWatch = false; }
            else if (st.menuWatch) st.menuWatch = false;
            else { st.menuOpen = false; st.textCapture = false; }
            // The order of a menu open against a swapchain replacement is what
            // a "hotkeys do nothing" report turns on, and nothing recorded it.
            LOG("[menu] key: menu %s", st.menuOpen ? (st.menuWatch ? "watching" : "open") : "closed");
        }
        s_keyWas = key;

        const bool watch = front && (KeyDown(c.keyWatch) || hooks::PadChordHeld(c.padWatch));
        if (watch && !s_watchWas && !st.rebindCapture && !st.textCapture)
        {
            // Home: closed -> watching; interactive -> watching; watching -> closed.
            if (!st.menuOpen) { st.menuOpen = true; st.menuWatch = true; }
            else if (!st.menuWatch) st.menuWatch = true;
            else { st.menuOpen = false; st.menuWatch = false; }
        }
        s_watchWas = watch;

        const bool esc = front && KeyDown(VK_ESCAPE);
        if (esc && !s_escWas && st.Captures() && !st.textCapture && !st.rebindCapture)
            st.menuOpen = false;
        s_escWas = esc;
    }

    bool WantsDraw()
    {
        const State& st = State::Get();
        if (st.menuOpen) return true;
        return (Settings::Get().showHud || st.noticeImportant) && st.notice[0] && static_cast<LONG>(st.noticeUntil - GetTickCount()) > 0;
    }

    // --- helpers ------------------------------------------------------------
    static std::string Lower(std::string s)
    {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    static bool Contains(const std::string& hay, const std::string& needleLower)
    {
        return needleLower.empty() || Lower(hay).find(needleLower) != std::string::npos;
    }
    // Where the last item ends, in the coordinates SameLine takes.
    static float ItemRight()
    {
        return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    }

    // A column as wide as the widest text drawn in it. The layout was drawn
    // around the English with fixed offsets, and a translation is often a
    // third longer and ran under the next column. The width is measured as
    // the rows are drawn and applied from the next frame on, so a group of
    // rows lines up, and the English offset stays the minimum so nothing
    // moves for a language that fits.
    struct FitColumn
    {
        float minPx;
        float widest = 0, pending = 0;
        int   frame = -1;
        // Call after drawing this column's text: it is measured, then the
        // cursor is placed at the start of the next column.
        void Next()
        {
            const int f = ImGui::GetFrameCount();
            if (f != frame) { widest = pending; pending = 0; frame = f; }
            const float r = ItemRight();
            if (r > pending) pending = r;
            const float a = minPx * g_scale, b = widest + 16 * g_scale;
            ImGui::SameLine(a > b ? a : b);
        }
    };
    static FitColumn s_rowLabel{ 260 }, s_rowValue{ 400 };   // key and pad rows
    static FitColumn s_statusLabel{ 220 };                   // the Status tab's rows
    static FitColumn s_linkLabel{ 110 };

    static void Help(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(TR(text));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    static bool TriState(const char* id, int& value)
    {
        bool changed = false;
        ImGui::PushID(id);
        if (ImGui::RadioButton(TR("default"), value == 0)) { value = 0; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton(TR("always"), value == 1)) { value = 1; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton(TR("never"), value == -1)) { value = -1; changed = true; }
        ImGui::PopID();
        return changed;
    }
    static void OnOff(const char* label, bool on, const char* onText = "yes", const char* offText = "no")
    {
        ImGui::TextUnformatted(TR(label));
        s_statusLabel.Next();
        ImGui::TextColored(on ? kGood : kWarn, "%s", on ? TR(onText) : TR(offText));
    }

    // One row of the key table: name, current key, rebind button, capture state.
    // Capturing a pad shortcut: hold two buttons, let go, and whatever was
    // held at the widest point is the shortcut. Waiting for the release is what
    // lets someone press them slightly apart, which everyone does.
    static bool PadRow(const char* label, unsigned& mask, int target)
    {
        State& st = State::Get();
        bool dirty = false;
        ImGui::PushID(target);
        ImGui::TextUnformatted(TR(label));
        s_rowLabel.Next();
        ImGui::TextDisabled("%s", hooks::PadChordName(mask));
        s_rowValue.Next();
        if (st.rebindCapture && g_rebindTarget == target)
        {
            static unsigned s_widest = 0;
            const unsigned held = hooks::PadButtons();
            if (held) s_widest |= held;
            int bits = 0;
            for (unsigned v = s_widest; v; v &= v - 1) ++bits;
            if (KeyDown(VK_ESCAPE)) { s_widest = 0; st.rebindCapture = false; g_rebindTarget = -1; }
            else if (!held && bits >= 2) { mask = s_widest; s_widest = 0; dirty = true; st.rebindCapture = false; g_rebindTarget = -1; }
            else if (!held && bits == 1) { s_widest = 0; }   // one button is not a shortcut, start over
            else if (bits) ImGui::TextColored(Accent(), TR("%s, let go to keep it"), hooks::PadChordName(s_widest));
            else ImGui::TextColored(Accent(), TR("hold two buttons (Escape cancels)"));
        }
        else if (!st.rebindCapture)
        {
            if (ImGui::SmallButton(TR("Set"))) { st.rebindCapture = true; g_rebindTarget = target; }
            if (mask) { ImGui::SameLine(); if (ImGui::SmallButton(TR("Clear"))) { mask = 0; dirty = true; } }
        }
        ImGui::PopID();
        return dirty;
    }

    static bool KeyRow(const char* label, int& vk, int target)
    {
        State& st = State::Get();
        bool dirty = false;
        ImGui::PushID(target);
        ImGui::TextUnformatted(TR(label));
        s_rowLabel.Next();
        ImGui::Text("%s", Settings::KeyName(vk));
        s_rowValue.Next();
        if (st.rebindCapture && g_rebindTarget == target)
        {
            ImGui::TextColored(Accent(), TR("press a key (Escape cancels)"));
            for (int k = 0x08; k < 0xFF; ++k)
            {
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue;
                if (!KeyDown(k)) continue;
                if (k != VK_ESCAPE) { vk = k; dirty = true; }
                st.rebindCapture = false; g_rebindTarget = -1;
                break;
            }
        }
        else if (!st.rebindCapture && ImGui::SmallButton(TR("Rebind"))) { st.rebindCapture = true; g_rebindTarget = target; }
        ImGui::PopID();
        return dirty;
    }

    // --- tabs ---------------------------------------------------------------
    // Two-click confirm for anything that overwrites or discards. The first
    // click arms the button, a second within four seconds does it, and only one
    // button is ever armed. Touching anything else disarms it.
    static std::string s_armed;
    static DWORD       s_armedUntil = 0;

    static bool Armed(const char* id)
    {
        return s_armed == id && static_cast<LONG>(s_armedUntil - GetTickCount()) > 0;
    }

    static bool ConfirmButton(const char* id, const char* label, const char* armedLabel, bool needsConfirm)
    {
        if (!needsConfirm)
        {
            if (!ImGui::Button(label)) return false;
            s_armed.clear();
            return true;
        }
        if (Armed(id))
        {
            ImGui::PushStyleColor(ImGuiCol_Button, kCrimson);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCrimsonHi);
            const bool go = ImGui::Button(armedLabel);
            ImGui::PopStyleColor(2);
            if (go) { s_armed.clear(); return true; }
            return false;
        }
        if (ImGui::Button(label)) { s_armed = id; s_armedUntil = GetTickCount() + 4000; }
        return false;
    }

    // Presets and backups. A preset is every setting and every rule under a
    // name; a backup is the settings file as it was at a moment in time, one
    // written automatically each time the game starts.
    static void DrawPresets()
    {
        Section(TR("Presets"));
        static std::string s_names[64];
        static int   s_count = -1;
        static int   s_sel = -1;
        static char  s_name[48] = "";
        static std::string s_baks[64];
        static int   s_bakCount = -1;
        static int   s_bakSel = 0;
        static char  s_said[128] = "";
        static DWORD s_saidUntil = 0;
        auto refresh = [&] {
            s_count = Settings::ListPresets(s_names, 64);
            if (s_sel >= s_count) s_sel = s_count - 1;
            s_bakCount = Settings::ListBackups(s_baks, 64);
            if (s_bakSel >= s_bakCount) s_bakSel = s_bakCount - 1;
            if (s_bakSel < 0 && s_bakCount > 0) s_bakSel = 0;
        };
        auto say = [&](const char* what) { snprintf(s_said, sizeof s_said, "%s", what); s_saidUntil = GetTickCount() + 5000; };
        if (s_count < 0) refresh();

        ImGui::SetNextItemWidth(240 * g_scale);
        const char* label = (s_sel >= 0 && s_sel < s_count) ? s_names[s_sel].c_str() : (s_count ? "pick one" : "none saved yet");
        if (ImGui::BeginCombo("##preset", label))
        {
            for (int i = 0; i < s_count; ++i)
                if (ImGui::Selectable(s_names[i].c_str(), i == s_sel)) { s_sel = i; s_armed.clear(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool have = s_sel >= 0 && s_sel < s_count;
        ImGui::BeginDisabled(!have);
        // Loading is cheap and undoable by loading another, so it just happens.
        // Saving over one cannot be undone, so that is the click that asks.
        if (ImGui::Button(TR("Load")))
        {
            s_armed.clear();
            if (Settings::LoadPreset(s_names[s_sel].c_str())) say("Preset loaded.");
            else say("That preset could not be read.");
        }
        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Replaces every setting and every class, tag and item rule with what the preset holds, and writes it to MasterLooter.ini. Nothing of the current settings is kept, so back them up first if you want them.")); ImGui::EndTooltip(); }
        ImGui::SameLine();
        if (ConfirmButton("preset.over", "Save over", "Overwrite it?", true))
        {
            if (Settings::SavePreset(s_names[s_sel].c_str())) say("Preset updated with the current settings.");
            else say("That preset could not be written.");
        }
        if (ImGui::BeginItemTooltip()) { ImGui::Text(TR("Writes the settings as they are now into \"%s\". Load it, change what you like, then save it back."), have ? s_names[s_sel].c_str() : ""); ImGui::EndTooltip(); }
        ImGui::SameLine();
        if (ConfirmButton("preset.delete", "Delete", "Delete for good?", true))
        {
            if (Settings::DeletePreset(s_names[s_sel].c_str())) { say("Preset deleted."); refresh(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(TR("Refresh"))) { refresh(); s_armed.clear(); }

        ImGui::SetNextItemWidth(240 * g_scale);
        if (ImGui::InputTextWithHint("##presetname", TR("name a new preset"), s_name, sizeof s_name)) s_armed.clear();
        ImGui::SameLine();
        const std::string clean = Settings::CleanPresetName(s_name);
        const bool exists = !clean.empty() && Settings::PresetExists(s_name);
        ImGui::BeginDisabled(clean.empty());
        if (ConfirmButton("preset.save", exists ? "Overwrite preset" : "Save as new preset", "Overwrite it?", exists))
        {
            if (Settings::SavePreset(s_name)) { say(exists ? "Preset overwritten." : "Preset saved."); s_name[0] = 0; refresh(); }
            else say("That name cannot be used.");
        }
        ImGui::EndDisabled();
        if (!clean.empty() && clean != s_name)
            ImGui::TextDisabled(TR("Saved as \"%s\": a preset name keeps letters, digits, spaces, dashes and underscores."), clean.c_str());
        else if (exists)
            ImGui::TextDisabled(TR("\"%s\" already exists. Saving over it asks first."), clean.c_str());

        Section(TR("Backups"));
        if (s_bakCount < 0) refresh();
        ImGui::SetNextItemWidth(240 * g_scale);
        const std::string bakLabel = (s_bakSel >= 0 && s_bakSel < s_bakCount)
                                   ? Settings::BackupLabel(s_baks[s_bakSel].c_str())
                                   : std::string(s_bakCount ? "pick one" : "none yet");
        if (ImGui::BeginCombo("##backup", bakLabel.c_str()))
        {
            for (int i = 0; i < s_bakCount; ++i)
            {
                const std::string one = Settings::BackupLabel(s_baks[i].c_str());
                if (ImGui::Selectable(one.c_str(), i == s_bakSel)) { s_bakSel = i; s_armed.clear(); }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool haveBak = s_bakSel >= 0 && s_bakSel < s_bakCount;
        ImGui::BeginDisabled(!haveBak);
        if (ConfirmButton("backup.restore", "Restore", "Discard current settings?", true))
        {
            if (Settings::RestoreBackup(s_baks[s_bakSel].c_str())) say("Settings restored from that backup.");
            else say("That backup could not be read.");
        }
        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Puts every setting and rule back to what it was at that moment.")); ImGui::EndTooltip(); }
        ImGui::SameLine();
        if (ConfirmButton("backup.delete", "Delete", "Delete for good?", true))
        {
            if (Settings::DeleteBackup(s_baks[s_bakSel].c_str())) { say("Backup deleted."); refresh(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        {
            const std::string stamp = Settings::NextBackupStamp();
            const bool over = Settings::BackupExists(stamp.c_str());
            if (ConfirmButton("backup.now", over ? "Back up again" : "Back up now", "Overwrite this minute's backup?", over))
            {
                if (Settings::BackupNow()) { say(over ? "Backup replaced." : "Settings backed up."); refresh(); }
                else say("The backup could not be written.");
            }
            if (ImGui::BeginItemTooltip()) { ImGui::Text(TR("Writes the settings as they are now, named %s."), Settings::BackupLabel(stamp.c_str()).c_str()); ImGui::EndTooltip(); }
        }

        if (s_said[0] && static_cast<LONG>(s_saidUntil - GetTickCount()) > 0) ImGui::TextColored(kGold, "%s", s_said);
        else ImGui::TextDisabled(TR("One backup is written each time the game starts and the last twelve are kept, in MasterLooter.backups next to the plugin. Presets sit in MasterLooter.presets. Copy either folder to keep it across a reinstall."));
    }

    static void TabGeneral(Config& c)
    {
        bool dirty = false;
        if (ImGui::Checkbox(TR("Auto-loot enabled"), &c.enabled)) { dirty = true; State::Get().Notify(c.enabled ? "Master Looter: auto-loot on" : "Master Looter: auto-loot off"); }
        Help(TR("The engine scans around you and takes what the rules allow. Off means nothing is taken automatically; the burst key still works."));
        {
            const float a = 300 * g_scale, b = ItemRight() + 16 * g_scale;
            ImGui::SameLine(a > b ? a : b);
        }
        if (ImGui::Button(TR("Loot everything in range now"))) loot::RequestBurst();
        dirty |= ImGui::Checkbox(TR("Show a brief notice when auto-loot is toggled"), &c.showHud);
        dirty |= ImGui::Checkbox(TR("Say so on screen when the bag stops taking things"), &c.notifyBagFull);
        Help(TR("How full the bag is is read from the bag itself, so the notice appears the moment it fills, whether or not you are looting. If those numbers ever stop making sense the mod falls back to watching what happens after a pick-up: three in a row that reach nothing raise the notice, and one landing clears it."));
        Help(TR("Nothing else is drawn while the menu is closed."));

        Section(TR("Keys"));
        dirty |= KeyRow("Open and close this menu", c.menuKey, 0);
        dirty |= KeyRow("Auto-loot on / off", c.keyToggle, 1);
        dirty |= KeyRow("Loot everything in range once", c.keyBurst, 2);
        dirty |= KeyRow("Watch mode (menu stays up, you keep playing)", c.keyWatch, 3);
        dirty |= KeyRow("Take goods that belong to someone, on / off", c.keyOwned, 4);
        Help(TR("Unbound until you set it. The game calls taking owned goods stealing and puts a bounty on you for it, so this is the switch you want on for a moment and off again, which is a poor fit for a menu."));

        Section(TR("Language"));
        {
            static char s_lang[16] = "";
            static bool s_init = false;
            if (!s_init) { s_init = true; snprintf(s_lang, sizeof s_lang, "%s", c.language.c_str()); }

            // The languages that ship with the mod are picked from a list by
            // their own names, since asking someone to type "zh-tw" to read
            // the menu in their own language is asking them to read the
            // English first.
            // The new language almost certainly needs characters the atlas was
            // not built with, so it is rebuilt before the next frame is drawn.
            auto pick = [&](const char* code) {
                snprintf(s_lang, sizeof s_lang, "%s", code);
                c.language = code;
                Text::Load(c.language.c_str());
                g_fontsDirty = true;
                Settings::MarkDirty();
            };
            const bool english = c.language.empty() || _stricmp(c.language.c_str(), "en") == 0;
            int nlangs = 0;
            const Text::Lang* langs = Text::BuiltIn(nlangs);
            // One dropdown, English first and the rest in the order of the
            // table. The closed box shows the language in use; a language
            // typed by hand that is not built in shows as its code, since
            // the mod has no name for it.
            const Text::Lang* cur = english ? nullptr : Text::Find(c.language.c_str());
            const char* preview = english ? TR("English") : cur ? cur->name : c.language.c_str();
            ImGui::SetNextItemWidth(240 * g_scale);
            if (ImGui::BeginCombo("##builtin", preview, ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable(TR("English"), english)) pick("");
                if (english) ImGui::SetItemDefaultFocus();
                for (int i = 0; i < nlangs; ++i)
                {
                    const bool on = _stricmp(c.language.c_str(), langs[i].code) == 0;
                    if (ImGui::Selectable(langs[i].name, on)) pick(langs[i].code);
                    if (on) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::SetNextItemWidth(120 * g_scale);
            ImGui::InputTextWithHint("##lang", TR("blank for English"), s_lang, sizeof s_lang);
            ImGui::SameLine();
            if (ImGui::Button(TR("Use this language")))
            {
                c.language = s_lang;
                Text::Load(c.language.c_str());
                g_fontsDirty = true;
                Settings::MarkDirty();
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Write translation template"))) Text::WriteTemplate();
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(TR("Writes MasterLooter.template.txt beside the plugin, holding every line the menu has shown this session. "
                                          "Open every tab first, since a line only appears once it has been drawn. Translate the right-hand side of each "
                                          "record, save it as MasterLooter.<language>.txt, and put that language here."));
                ImGui::EndTooltip();
            }
            if (Text::Language()[0])
            {
                ImGui::TextDisabled(TR("Language \"%s\": %d line(s) translated, %d seen this session."), Text::Language(), Text::Count(), Text::Seen());
                if (const Text::Lang* l = Text::Find(Text::Language()))
                {
                    // A contributed language names its translator. A machine
                    // one says so, because a reader who finds a wrong word
                    // should know nobody has read the file before them.
                    if (l->credit && *l->credit)
                        ImGui::TextDisabled(TR("%s translated by %s."), l->name, l->credit);
                    else
                        ImGui::TextDisabled(TR("%s is a machine translation that nobody has checked yet. Corrections are welcome on GitHub or the Nexus page."), l->name);
                }
                if (Text::FromFile())
                    ImGui::TextDisabled(TR("Read from the file beside the plugin, not the copy inside it."));
            }
            else
                ImGui::TextDisabled(TR("English. %d line(s) seen this session."), Text::Seen());
            if (Text::Rejected())
                ImGui::TextColored(kWarn, TR("%d line(s) were ignored: their %%d or %%s placeholders did not match the English, and using them would print the wrong values."), Text::Rejected());
        }

        Section(TR("Controller"));
        ImGui::TextDisabled(TR("Two buttons at once, never one: every single button already does something in this game. "
                            "The mod only reads the pad, so the game still sees both buttons. Pick a pair that does nothing together, "
                            "like the two shoulder buttons, or Back and a face button."));
        dirty |= PadRow("Open and close this menu", c.padMenu, 10);
        dirty |= PadRow("Auto-loot on / off", c.padToggle, 11);
        dirty |= PadRow("Loot everything in range once", c.padBurst, 12);
        dirty |= PadRow("Watch mode", c.padWatch, 13);
        dirty |= PadRow("Take owned goods on / off", c.padOwned, 14);

        Section(TR("Pace"));
        dirty |= ImGui::SliderInt(TR("Scans per second"), &c.scansPerSec, 1, 30);
        Help(TR("How often the scene is read. The scan runs on its own thread; only the final take costs the game a fraction of a millisecond."));
        dirty |= ImGui::SliderInt(TR("Objects per scan"), &c.perScan, 0, 64, c.perScan ? "%d" : "no limit");
        dirty |= ImGui::SliderInt(TR("Objects per burst press"), &c.burstPerKey, 0, 64, c.burstPerKey ? "%d" : "everything in range");
        dirty |= ImGui::SliderInt(TR("Retry the same object after (ms)"), &c.retryAfterMs, 500, 30000);
        Help(TR("The game removes taken objects with a delay. This stops the same object being sent twice while it fades."));

        Section(TR("Filters"));
        dirty |= ImGui::Checkbox(TR("Skip quest items"), &c.skipQuestItems);
        Help(TR("Items tagged quest are left alone, so puzzles and story pickups are not auto-taken. This reads the item's own tags, so it covers anything picked up, gathered or broken open. Skinning a carcass is outside it: that yield arrives without the mod ever seeing what it is."));
        dirty |= ImGui::Checkbox(TR("Skip items shops refuse to buy"), &c.skipNoSell);
        dirty |= ImGui::SliderInt(TR("Minimum value (copper)"), &c.minValueCopper, 0, 500, c.minValueCopper ? "%d" : "off");
        Help(TR("Items with an unknown value are never filtered by it."));
        dirty |= ImGui::Checkbox(TR("Take items the database cannot name"), &c.takeUnknownItems);
        Help(TR("Some world objects carry no readable item name. On: take them anyway. Off: leave anything unidentified."));
        dirty |= ImGui::Checkbox(TR("Pets and companions follow the filters"), &c.petFilter);
        Help(TR("A pet loots whatever it likes and the game has no switch for it. On: anything a pet or a companion picks up that your item rules, tags, classes or value floor would have refused is deleted from the inventory as it lands. Quest and protected items are never deleted. Something you pick up by hand in the same two seconds is judged by the same rules."));
        dirty |= ImGui::Checkbox(TR("Verbose log"), &c.debugLog);
        if (dirty) Settings::MarkDirty();
    }

    static void TabLooting(Config& c)
    {
        bool dirty = false;
        Section(TR("What to collect"));
        struct Toggle { const char* label; bool* value; const char* help; };
        const Toggle toggles[] = {
            { "Ground items",   &c.pickUpItems,    "Items lying in the world, including drops from enemies." },
            { "Carcasses",      &c.lootCorpses,    "Skinning the animals you kill, once per carcass. People go under Bodies. What skinning pays out is not checked against your filters. The mod never sees it before it lands, so this switch is all or nothing." },
            { "Bodies",         &c.searchBodies,   "Searching the people you kill, once per body. Animals go under Carcasses. What a search pays out is not checked against your filters, so this switch is all or nothing too." },
            { "Plants",         &c.gatherPlants,   "Herb, flower and mushroom nodes, and the same lying on the ground. Food crops have their own switch." },
            { "Crops",          &c.gatherCrops,    "Vegetables, fruit and grain: sweet potato, barley, cabbage, apples, grapes and the rest of the farmed and foraged food, on the plant or lying loose. These used to answer to Ground items, which is why turning Plants off still emptied a field: of the 72 collection sockets in the game, 44 are crops and only 28 are plants." },
            { "Ore",            &c.gatherOre,      "Ore chunks on the ground and any node that yields ore, veins included. A vein is broken where it stands and its contents picked up off the floor, which is what your pickaxe does and what makes a better pickaxe worth carrying: the tool's Mining Yield Up applies to the drop, not to the node. Each vein is struck once and left alone until the game brings it back. Reaching for veins starts as far out as the scan can see, because they take seconds to answer where a bush takes a fraction of one." },
            { "Stone",          &c.gatherStone,    "Stone on the ground and nodes that yield stone." },
            { "Wood",           &c.gatherWood,     "Timber and branches on the ground and nodes that yield them." },
            { "Unidentified nodes", &c.gatherUnknown, "Nodes the prefab table does not name and that have not yielded anything yet this session. Off (the default) leaves them alone. On makes the mod gather them to find out, which means a plant can be taken while Plants is off." },
            { "Insects",        &c.catchInsects,   "Butterflies, beetles, dragonflies, bees, spiders, scorpions, snails and the other small things the game files as insects. Species come from the creature table; a creature the table cannot name is only caught when every category it could belong to is on (a flyer could be a fish, an insect or a bird)." },
            { "Fish",           &c.catchFish,      "Fish, and whatever else you catch in the water: crabs, shrimp, squid, starfish and seahorses." },
            { "Small animals",  &c.catchAnimals,   "Rats, squirrels, birds, lizards, frogs and salamanders: anything else the game puts in the bag whole." },
            { "Containers",     &c.lootContainers, "Chests, crates and drop-set nodes. They rarely respond to the loot event. Off by default." },
            { "Furniture", &c.lootFurniture, "Tables, chairs, beds, carpets, lamps, candles, paintings, pots and the rest of a furnished room, whether you pick one up off the floor or take it from its own interaction node. Most of it is worth a copper or two, but the carpets and the luxury beds run to thousands, so turn this on before furnishing a house. Chests and the other things that hold something answer to Containers instead. Off by default." },
        };
        // Where to say this, because the Items tab is the answer and nobody
        // thinks to look for a live animal there. A creature that can be caught
        // becomes an item, and the catch is refused if that item is refused, so
        // Item_Iguana set to never on the Items tab leaves iguanas alone. A
        // separate Creatures tab was built for this in 1.6.4 and taken out
        // again: it could only act on creatures the game names, which is a
        // smaller set than the item rules already covered.
        ImGui::TextDisabled(TR("To leave one species alone, set what it becomes to never on the Items tab: Item_Tench, Item_Ricefish, Item_Iguana. The mod reads the creature's own row in the game's character table, so it knows which species it is looking at before it catches anything. A few unusual creatures still fall back to guessing from their model name and are caught by category only."));
        if (ImGui::BeginTable("collect", 3, ImGuiTableFlags_SizingStretchSame))
        {
            for (const Toggle& t : toggles)
            {
                ImGui::TableNextColumn();
                ImGui::PushID(t.label);   // the English, so the id survives a language change
                dirty |= ImGui::Checkbox(TR(t.label), t.value);
                if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f); ImGui::TextUnformatted(TR(t.help)); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        {
            const loot::Status s = loot::GetStatus();
            ImGui::TextDisabled(TR("%d node kinds learned this session. Nodes are named by the prefab they were placed from; anything that table misses is identified the first time it yields something. A node's type number changes between sessions, so nothing is remembered after you quit."), s.learned);
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Forget"))) loot::ForgetLearned();
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Clears what this session worked out from your bag. Nodes named by the prefab table are unaffected.")); ImGui::EndTooltip(); }
        }

        Section(TR("Ranges (metres)"));
        dirty |= ImGui::SliderFloat(TR("Scan radius"), &c.scanRange, 5.0f, 200.0f, "%.0f");
        Help(TR("What enters the working list at all. The ranges below are clamped to it."));
        dirty |= ImGui::SliderFloat(TR("Items on the ground"), &c.lootRange, 0.0f, 100.0f, c.lootRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat(TR("Gathering"), &c.gatherRange, 0.0f, 100.0f, c.gatherRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat(TR("Catching"), &c.catchRange, 0.0f, 100.0f, c.catchRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat(TR("Carcasses"), &c.corpseRange, 0.0f, 100.0f, c.corpseRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat(TR("Dead zone around you"), &c.minRange, 0.0f, 2.0f, "%.2f");
        Help(TR("Objects closer than this are treated as your own equipment. A safety net; your gear is also recognised by other means."));

        Section(TR("Reaching nodes"));
        dirty |= ImGui::Checkbox(TR("Arm nodes ourselves"), &c.autoArm);
        Help(TR("The game fills a node's data only when it thinks you can reach it. Arming asks it to do that from a distance, which is also what lets an ore vein be harvested where it stands. Veins take several seconds to answer where a bush takes a fraction of one."));
        dirty |= ImGui::SliderFloat(TR("Arming range"), &c.armRange, 0.0f, 60.0f, c.armRange > 0 ? "%.0f" : "same as gathering");
        Help(TR("Arming ignores walls. Keep it short or you will gather through the wall of the next room. Ore is the exception and is always reached for at up to 25 m, because a vein takes seconds to answer and asking only once you are on top of it means it opens too late to be any use."));
        dirty |= ImGui::Checkbox(TR("Mine ore veins for you"), &c.gatherVeins);
        dirty |= ImGui::Checkbox(TR("Draw water from wells"), &c.drawWells);
        Help(TR("Winds the handle for you and takes what comes up, by driving the same state transitions the game drives when you hold the interact button. A well raises no loot event at all, so this is a replay of a captured sequence rather than the game being asked for anything. Off by default, because a well it gets wrong is a well left standing without its bucket."));
        Help(TR("On, a vein is harvested where it stands and you never swing a pickaxe. Off, veins are left alone and only the chunks you knock loose are picked up. Needs arming, since asking the vein to open is the whole trick."));
        dirty |= ImGui::Checkbox(TR("Break veins open rather than emptying them"), &c.breakOre);
        Help(TR("Breaking drives the game's own state machine at the vein, the swing landing and then the break, so the node spills its contents on the ground and disappears exactly as it does under a pickaxe. Emptying instead lifts the ore straight out and skips the drop entirely. Neither route pays your tool's Mining Yield Up, which the game only grants to a swing it counts as yours, so set the extra below to make up the difference. Each vein is struck once and then left alone until the game respawns it."));
        dirty |= ImGui::SliderInt(TR("Extra ore per vein"), &c.oreBonus, 0, 10, c.oreBonus > 0 ? "+%d" : "off");
        Help(TR("The mod never swings a pickaxe. It drives the vein's own break sequence, and the game walks one drop row for that where a real swing walks two, so your mining tool's bonus pays nothing when the mod does the mining. This adds the missing ore back through the same drop path the game uses, so it is real ore spilled on the ground, not an item copied into your bag. Set it to what your tool pays by hand minus one: a drill that gives three by hand wants 2 here. It only applies to a vein this mod broke, so mining by hand is untouched and the two cannot stack."));
        dirty |= ImGui::Checkbox(TR("Arm mechanism containers too"), &c.armContainers);
        Help(TR("A well bucket and the like are never looted, but arming one makes the game offer its interaction so you can use it by hand."));

        Section(TR("Ownership"));
        dirty |= ImGui::Checkbox(TR("Take goods that belong to someone"), &c.lootOwned);
        if (c.lootOwned) ImGui::TextColored(kWarn, TR("The game treats this as stealing and will put a bounty on you."));
        else ImGui::TextDisabled(TR("Uses the game's own Take or Steal check; owned goods are skipped until it has been observed once."));

        DrawPresets();
        if (dirty) Settings::MarkDirty();
    }

    // Named groups of classes, so a whole family can be switched with one click.
    struct ClassGroup { const char* name; const char* classes; const char* help; };
    static const ClassGroup kGroups[] = {
        { "Weapons and armor",   "weapon shield helm body-armor gloves boots cloak armor mask eyewear", "Player equipment." },
        { "Damaged gear",        "damaged-gear", "Worn gear that enemies drop as they die. Cheap and heavy, but plentiful." },
        { "Accessories",         "accessory necklace ring earring bag", "" },
        { "Abyss and Kuku",      "abyss-gear abyss-gear-box abyss-item kuku-power-core kuku-core kuku-pot-item kuku-pot kuku-currency stat-boost", "Abyss gear, Kuku pots and their parts." },
        { "Food and drink",      "food field-cooked drink elixir potion store-food honey meat seafood fruit vegetable grain cooking-basic", "" },
        { "Materials",           "ore jewel stone wood hide fabric bone metal catalyst crafting-material alchemy-material herb ingredient seed trade-good goods bait", "Ore, gems, stone, timber, hides, herbs, trade goods and other crafting input." },
        { "Creatures",           "insect fish animal amphibian", "Caught creatures, live or lying around; a creature the table can name follows its class rule. Crabs, shrimp and squid are seafood under Food and drink." },
        { "Ammunition",          "arrow ammo ammo-bundle bullet magic-bullet cannonball explosive", "" },
        { "Books and papers",    "book document note poster skill-poster bounty-notice treasure-map legendary-animal-report skill-book recipe-book recipe-food recipe-potion recipe-furniture recipe-abyss-gear recipe-armor recipe", "" },
        { "Furniture and decor", "furniture household dye lamp light ornament painting flower-pot decoration cooking-facility storage container", "Household clutter and collectibles, most of it worthless." },
        { "Mounts and vehicles", "mount-gear mount-feed mount-utility pet-gear vehicle-part", "" },
        { "Treasure and keepsakes", "treasure sealed-artifact artifact keepsake currency chest", "Boss rewards, artifacts, memory items, coin pouches and reward chests." },
        { "Keys and tools",      "key key-item tool gimmick", "" },
    };

    // 1 all on, 0 all off, 2 mixed.
    static int GroupState(const Config& c, const ClassGroup& g)
    {
        int on = 0, off = 0;
        std::string cls;
        for (const char* p = g.classes;; ++p)
        {
            if (*p && *p != ' ') { cls += *p; continue; }
            if (!cls.empty())
            {
                auto it = c.classRule.find(cls);
                ((it == c.classRule.end() || it->second != 0) ? on : off)++;
                cls.clear();
            }
            if (!*p) break;
        }
        return off == 0 ? 1 : on == 0 ? 0 : 2;
    }
    static void SetGroup(Config& c, const ClassGroup& g, bool loot)
    {
        std::string cls;
        for (const char* p = g.classes;; ++p)
        {
            if (*p && *p != ' ') { cls += *p; continue; }
            if (!cls.empty()) { c.classRule[cls] = loot ? 1 : 0; cls.clear(); }
            if (!*p) break;
        }
    }

    static void TabClasses(Config& c)
    {
        Section(TR("Groups"));
        ImGui::TextDisabled(TR("One click per family. The table below still sets single classes; a group shows a dash when only some of it is on."));
        if (ImGui::BeginTable("groups", 3, ImGuiTableFlags_SizingStretchSame))
        {
            for (const ClassGroup& g : kGroups)
            {
                ImGui::TableNextColumn();
                const int st = GroupState(c, g);
                bool v = st == 1;
                if (st == 2) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                ImGui::PushID(g.name);
                if (ImGui::Checkbox(TR(g.name), &v)) { SetGroup(c, g, st != 1); Settings::MarkDirty(); }
                if (st == 2) ImGui::PopItemFlag();
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                    if (g.help[0]) ImGui::TextUnformatted(TR(g.help));
                    ImGui::TextDisabled("%s", g.classes);
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            // Cross-cutting switches that live on tags rather than classes.
            ImGui::TableNextColumn();
            bool quest = !c.skipQuestItems;
            if (ImGui::Checkbox(TR("Quest items"), &quest)) { c.skipQuestItems = !quest; Settings::MarkDirty(); }
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Anything tagged quest, across every class. Off keeps story pickups and puzzle pieces for your own hands.")); ImGui::EndTooltip(); }
            ImGui::TableNextColumn();
            bool nosell = !c.skipNoSell;
            if (ImGui::Checkbox(TR("Unsellable items"), &nosell)) { c.skipNoSell = !nosell; Settings::MarkDirty(); }
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Items no shop will buy, across every class.")); ImGui::EndTooltip(); }
            ImGui::TableNextColumn();
            auto mf = c.tagRule.find("memory-fragment"); auto gm = c.tagRule.find("gimmick");
            const bool mfOn = mf != c.tagRule.end() && mf->second > 0, gmOn = gm != c.tagRule.end() && gm->second > 0;
            bool prot = mfOn && gmOn;
            if (mfOn != gmOn) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox(TR("Memory chips and puzzle parts"), &prot))
            {
                if (prot) { c.tagRule["memory-fragment"] = 1; c.tagRule["gimmick"] = 1; }
                else { c.tagRule.erase("memory-fragment"); c.tagRule.erase("gimmick"); }
                Settings::MarkDirty();
            }
            if (mfOn != gmOn) ImGui::PopItemFlag();
            if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f); ImGui::TextUnformatted(TR("Protected by default: a memory chip starts a memory scene when taken, and a puzzle part breaks its puzzle. Turn on only if you want them auto-taken.")); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            ImGui::EndTable();
        }

        Section(TR("Classes"));
        static char filter[64] = "";
        ImGui::SetNextItemWidth(260 * g_scale);
        ImGui::InputTextWithHint("##classfilter", TR("filter classes"), filter, sizeof filter);
        ImGui::SameLine();
        if (ImGui::Button(TR("Loot all"))) { for (const auto& kv : ItemDb::Classes()) c.classRule[kv.first] = 1; Settings::MarkDirty(); }
        ImGui::SameLine();
        if (ImGui::Button(TR("Skip all"))) { for (const auto& kv : ItemDb::Classes()) c.classRule[kv.first] = 0; Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%d classes"), static_cast<int>(ItemDb::Classes().size()));

        const std::string f = Lower(filter);
        if (ImGui::BeginTable("classes", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(TR("Loot"), ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
            ImGui::TableSetupColumn(TR("Class"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("Items"), ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
            ImGui::TableHeadersRow();
            for (const auto& kv : ItemDb::Classes())
            {
                if (!Contains(kv.first, f)) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                auto it = c.classRule.find(kv.first);
                bool loot = (it == c.classRule.end()) || it->second != 0;
                ImGui::PushID(kv.first.c_str());
                if (ImGui::Checkbox("##loot", &loot)) { c.classRule[kv.first] = loot ? 1 : 0; Settings::MarkDirty(); }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                if (kv.first == "dev") ImGui::TextDisabled(TR("%s (never looted)"), kv.first.c_str());
                else ImGui::TextUnformatted(kv.first.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", kv.second);
            }
            ImGui::EndTable();
        }
    }

    static void TabTags(Config& c)
    {
        static char filter[64] = "";
        ImGui::SetNextItemWidth(260 * g_scale);
        ImGui::InputTextWithHint("##tagfilter", TR("filter tags"), filter, sizeof filter);
        ImGui::SameLine();
        if (ImGui::Button(TR("Reset all tags"))) { c.tagRule.clear(); Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled(TR("always beats the class rule; never beats always"));

        const std::string f = Lower(filter);
        if (ImGui::BeginTable("tags", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(TR("Tag"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("Items"), ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
            ImGui::TableSetupColumn(TR("Rule"), ImGuiTableColumnFlags_WidthFixed, 260 * g_scale);
            ImGui::TableHeadersRow();
            for (const auto& kv : ItemDb::Tags())
            {
                if (!Contains(kv.first, f)) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                auto it = c.tagRule.find(kv.first);
                const int cur = it == c.tagRule.end() ? 0 : it->second;
                if (cur) ImGui::TextColored(Accent(), "%s", kv.first.c_str()); else ImGui::TextUnformatted(kv.first.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", kv.second);
                ImGui::TableSetColumnIndex(2);
                int v = cur;
                if (TriState(kv.first.c_str(), v)) { if (v == 0) c.tagRule.erase(kv.first); else c.tagRule[kv.first] = v; Settings::MarkDirty(); }
            }
            ImGui::EndTable();
        }
    }

    static void TabItems(Config& c)
    {
        static char query[96] = "";
        ImGui::SetNextItemWidth(360 * g_scale);
        ImGui::InputTextWithHint("##itemsearch", TR("search item name or key (3+ letters)"), query, sizeof query);
        ImGui::SameLine();
        if (ImGui::Button(TR("Clear overrides"))) { c.itemRule.clear(); Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%d overrides"), static_cast<int>(c.itemRule.size()));

        const std::string q = Lower(query);
        std::vector<const Item*> rows;
        if (q.size() >= 3)
        {
            for (const Item& it : ItemDb::All())
                if (Contains(it.name, q) || Contains(it.stringKey, q) || it.klass == q) { rows.push_back(&it); if (rows.size() >= 250) break; }
        }
        else
        {
            for (const auto& kv : c.itemRule) if (const Item* it = ItemDb::Find(kv.first)) rows.push_back(it);
        }
        if (q.size() < 3) ImGui::TextDisabled(TR("Showing current overrides. Type to search all %d items."), ItemDb::Count());
        else
        {
            // Two strings rather than a plural suffix pasted in, so a
            // translation can word each one.
            const char* cap = rows.size() >= 250 ? TR(" (first 250)") : "";
            if (rows.size() == 1) ImGui::TextDisabled(TR("1 match%s"), cap);
            else                  ImGui::TextDisabled(TR("%d matches%s"), static_cast<int>(rows.size()), cap);
        }

        if (ImGui::BeginTable("items", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(TR("Item"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("Class"), ImGuiTableColumnFlags_WidthFixed, 120 * g_scale);
            ImGui::TableSetupColumn(TR("Copper"), ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
            ImGui::TableSetupColumn(TR("Verdict"), ImGuiTableColumnFlags_WidthFixed, 170 * g_scale);
            ImGui::TableSetupColumn(TR("Override"), ImGuiTableColumnFlags_WidthFixed, 260 * g_scale);
            ImGui::TableHeadersRow();
            for (const Item* it : rows)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(it->key));
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(it->Label());
                if (ImGui::BeginItemTooltip()) { ImGui::Text(TR("%s (key %u, row %d, tier %d)"), it->stringKey.c_str(), it->key, it->row, it->tier); ImGui::TextWrapped("%s", it->tags.c_str()); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(it->klass.c_str());
                ImGui::TableSetColumnIndex(2);
                if (it->value >= 0) ImGui::Text(TR("%lld"), it->value); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3);
                const Rules::Verdict v = Rules::Decide(*it, c);
                if (v.loot) ImGui::TextColored(kGood, TR("loot: %s"), v.rule); else ImGui::TextColored(kWarn, TR("skip: %s"), v.rule);
                if (!v.detail.empty() && ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(v.detail.c_str()); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(4);
                auto ov = c.itemRule.find(it->key);
                int cur = ov == c.itemRule.end() ? 0 : ov->second;
                if (TriState("ov", cur)) { if (cur == 0) c.itemRule.erase(it->key); else c.itemRule[it->key] = cur; Settings::MarkDirty(); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    static void TabNearby()
    {
        const loot::Status s = loot::GetStatus();
        if (!s.started) { ImGui::TextColored(kWarn, TR("Loot engine not started.")); return; }
        if (!s.resolved) { ImGui::TextColored(kWarn, TR("Game functions did not resolve; see Status.")); return; }
        if (!s.actorManager) { ImGui::TextDisabled(TR("Waiting for the world to load.")); return; }
        if (!s.playerFound) { ImGui::TextDisabled(TR("Player not found in the scene yet.")); return; }
        ImGui::Text(TR("%d objects within scan range, %d lootable now."), s.candidates, s.lootable);
        ImGui::SameLine();
        if (s.settling) ImGui::TextColored(kWarn, TR("paused: %s"), s.hold);
        else ImGui::TextDisabled(TR("scan %.1f ms"), s.lastScanMs);
        ImGui::TextDisabled(TR("Objects are listed nearest first, with the rule that decided each one. Empty nodes read as not ready until the game or arming fills them. What you are wearing and carrying is left out."));

        static loot::Nearby rows[loot::kNearbyRows];
        const int n = loot::CopyNearby(rows, loot::kNearbyRows);
        if (s.listed > n) ImGui::TextColored(kWarn, TR("Showing the nearest %d of %d. The rest are further away."), n, s.listed);
        if (ImGui::BeginTable("nearby", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, 50 * g_scale);
            ImGui::TableSetupColumn(TR("Object"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(TR("Class"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
            ImGui::TableSetupColumn(TR("Decision"), ImGuiTableColumnFlags_WidthFixed, 300 * g_scale);
            ImGui::TableHeadersRow();
            for (int i = 0; i < n; ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%.1f", rows[i].dist);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(TR(rows[i].name));
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::Text(TR("entity %08X"), rows[i].eid);
                    // What a rule can be written against, on the row where the
                    // verdict was seen: the class for the Classes tab, the tags
                    // for the Tags tab.
                    if (rows[i].klass[0]) ImGui::Text(TR("class %s"), rows[i].klass);
                    if (rows[i].value >= 0) ImGui::Text(TR("worth %lld copper"), rows[i].value);
                    if (rows[i].tags[0])
                    {
                        ImGui::TextDisabled(TR("tags, any of which can be set to never in the Tags tab:"));
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                        ImGui::TextUnformatted(rows[i].tags);
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::EndTooltip();
                }
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", TR(rows[i].klass));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(rows[i].loot ? kGood : kMuted, "%s", TR(rows[i].verdict));
            }
            ImGui::EndTable();
        }
    }

    // An address and a button that copies it. The overlay cannot show a web
    // page and pulling the player out to a browser mid-game would be worse, so
    // the clipboard is the useful half of a link here.
    static void LinkRow(const char* label, const char* url)
    {
        ImGui::TextDisabled("%s", label);
        s_linkLabel.Next();
        ImGui::TextColored(kGoldDim, "%s", url);
        ImGui::SameLine();
        ImGui::PushID(url);
        if (ImGui::SmallButton(TR("Copy"))) ImGui::SetClipboardText(url);
        if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Copy to the clipboard.")); ImGui::EndTooltip(); }
        ImGui::PopID();
    }

    static void TabStatus()
    {
        const State& st = State::Get();
        const loot::Status s = loot::GetStatus();
        ImGui::Text(TR("Master Looter v%s for game build %s"), ML_VERSION, ML_GAME_BUILD);
        LinkRow("Mod page", ML_MOD_PAGE);
        LinkRow("Source", ML_SOURCE_URL);
        ImGui::Text(TR("Settings: %ls"), Settings::Path().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Reload now"))) Settings::Load();
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Save now"))) Settings::Save();
        // Nobody thinks to look in bin64, and the log is the first thing asked
        // for in a report, so the whole path is here with a way to reach it.
        {
            const std::wstring logPath = ml::Paths::File(L"MasterLooter.log");
            ImGui::Text(TR("Log: %ls"), logPath.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Open folder"))) ShellExecuteW(nullptr, L"open", ml::Paths::Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Opens the folder in Explorer. The log is rewritten every launch and is what to attach to a report.")); ImGui::EndTooltip(); }
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Copy path"))) { char buf[512]; snprintf(buf, sizeof buf, "%ls", logPath.c_str()); ImGui::SetClipboardText(buf); }
        }

        Section(TR("Overlay"));
        OnOff("DirectX 12 hooks", st.hooksOk, "installed", "failed");
        {
            Config& c = Settings::Get();
            if (ImGui::Checkbox(TR("Wrap the swapchain"), &c.wrapSwapChain)) Settings::MarkDirty();
            ImGui::SameLine();
            ImGui::TextDisabled(TR("takes effect next launch"));
            if (ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(TR("On, the overlay draws under DLSS frame generation, which needs this mod to hand the game a wrapped swapchain.\n\n"
                                       "Off, the overlay draws through the present hook instead. Everything looks the same unless frame generation is on, "
                                       "and the mod stays off a path other overlay mods patch as well. Try this if the game will not start alongside another "
                                       "overlay mod; it can be set as WrapSwapChain in MasterLooter.ini without launching the game."));
                ImGui::EndTooltip();
            }
        }
        OnOff("Item database", ItemDb::Loaded(), "loaded", "missing MasterLooter.items.tsv");
        if (ItemDb::Loaded()) { ImGui::SameLine(); ImGui::TextDisabled(TR("%d items, %d classes, %d tags"), ItemDb::Count(), static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size())); }

        Section(TR("Loot engine"));
        OnOff("Engine", s.started, s.note, "not started");
        OnOff("Game functions", s.resolved, "resolved", "missing");
        OnOff("Game-thread pump", s.hooked, s.pump, "none");
        OnOff("World (actor manager)", s.actorManager, "found", "waiting");
        if (s.playerFound)
        {
            ImGui::TextUnformatted(TR("Player")); s_statusLabel.Next();
            if (s.bagSlots > 0) ImGui::TextColored(s.bagFull ? kWarn : kGood, TR("entity %08X, bag %d of %d slots, %d items across every store"), s.playerEid, s.bagUsed, s.bagSlots, s.inventoryItems);
            else ImGui::TextColored(s.bagFull ? kWarn : kGood, TR("entity %08X, %d items across every store"), s.playerEid, s.inventoryItems);
            if (s.bagFull) { ImGui::SameLine(); ImGui::TextColored(kWarn, TR("  bag full")); }
        }
        else OnOff("Player", false, "", "not found yet");
        OnOff("Event descriptors", s.descriptors == 3, "3 of 3", s.descriptors ? "incomplete" : "not resolved yet");
        OnOff("Sending events", s.sendAllowed, "allowed", "not yet");
        OnOff("Route id", s.routeKnown, "learned from the game", "using the player's own field");
        OnOff("Ownership oracle", s.ownerOracle, "captured", "waiting for the game to check an item");
        const char* tbl = s.itemTable == 1 ? "rows verified against our database" : s.itemTable == 2 ? "names readable, rows differ" : s.itemTable == -1 ? "unavailable" : "not probed yet";
        OnOff("Item table", s.itemTable > 0, tbl, tbl);
        ImGui::Text(TR("Scans %ld, events sent %ld, pump ticks %ld, guarded faults %ld, node kinds learned %d"), s.scans, s.sent, s.pumpTicks, s.faults, s.learned);
        ImGui::Text(TR("This session: %ld picked up, %ld gathered, %ld caught, %ld carcasses"),
                    loot::SessionCount(static_cast<int>(events::Action::Take)), loot::SessionCount(static_cast<int>(events::Action::Gather)),
                    loot::SessionCount(static_cast<int>(events::Action::Catch)), loot::SessionCount(static_cast<int>(events::Action::Search)));

        if (ImGui::TreeNode("Signatures"))
        {
            for (int i = 0; i < game::SigCount(); ++i)
            {
                const game::SigResult& r = game::Sig(i);
                if (r.addr) ImGui::TextColored(kGood, TR("%-18s +0x%llX"), r.name, static_cast<unsigned long long>(mem::Rva(r.addr)));
                else ImGui::TextColored(r.required ? kWarn : kMuted, TR("%-18s %s (%zu hits)%s"), r.name, r.hits ? "ambiguous" : "missing", r.hits, r.required ? "  required" : "  optional");
            }
            ImGui::TreePop();
        }

        Section(TR("Recent loot"));
        static loot::Recent recent[12];
        const int rn = loot::CopyRecent(recent, 12);
        if (!rn) ImGui::TextDisabled(TR("nothing taken yet this session"));
        for (int i = 0; i < rn; ++i) ImGui::TextUnformatted(recent[i].text);

        Section(TR("Log"));
        static std::vector<std::string> lines;
        Log::Snapshot(lines, 80);
        if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders))
        {
            for (const auto& l : lines) ImGui::TextUnformatted(l.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    // The only thing drawn while the menu is closed: a notice that fades out.
    static void DrawNotice()
    {
        const State& st = State::Get();
        const DWORD now = GetTickCount();
        if (!st.notice[0] || static_cast<LONG>(st.noticeUntil - now) <= 0) return;
        const LONG left = static_cast<LONG>(st.noticeUntil - now);
        const float alpha = left < 600 ? left / 600.0f : 1.0f;
        const ImVec2 size = ImGui::CalcTextSize(st.notice);
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2((disp.x - size.x) * 0.5f - 14 * g_scale, disp.y * 0.12f));
        ImGui::SetNextWindowBgAlpha(0.70f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g_scale, 9 * g_scale));
        if (ImGui::Begin("##mlnotice", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::PushFont(g_fontHead);
            ImGui::TextColored(kGold, "%s", st.notice);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    void Render()
    {
        State& st = State::Get();
        // Every tab edits the live Config; the loot worker copies it under this lock.
        std::lock_guard<std::recursive_mutex> lock(Settings::Mutex());
        Config& c = Settings::Get();

        // Language follows the config wherever the config came from.
        //
        // It used to be applied in exactly two places, both of them buttons on
        // the Language section, so a preset, a restored backup or a hand edit
        // picked up by the live reload all changed the setting and left the menu
        // in the old language. The Load-preset tooltip promises it replaces
        // every setting, and this was the one it silently did not. Worse, the
        // text box then showed the stale value, which is a one-click way to
        // overwrite what was just restored.
        //
        // Here rather than in Settings on purpose: every Text::Load call has
        // always run on the render thread, and the string table is read while
        // this function draws.
        static std::string s_appliedLang;
        static bool s_langInit = false;
        if (!s_langInit) { s_appliedLang = c.language; s_langInit = true; }
        else if (c.language != s_appliedLang) { s_appliedLang = c.language; Text::Load(c.language.c_str()); }

        ImGuiIO& io = ImGui::GetIO();
        const bool capt = st.Captures();
        io.MouseDrawCursor = capt;
        st.renderTid = GetCurrentThreadId();
        static bool s_wasCapt = false;
        if (capt != s_wasCapt) { s_wasCapt = capt; if (capt) input::MenuOpened(); else input::MenuClosed(); }
        if (capt) input::FeedMouse(io);
        else { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddFocusEvent(capt); } // nothing hovers or reacts while watching

        if (c.showHud || st.noticeImportant) DrawNotice();
        if (!st.menuOpen) { st.textCapture = false; if (st.rebindCapture) { st.rebindCapture = false; g_rebindTarget = -1; } return; }

        ImGui::SetNextWindowSize(ImVec2(900 * g_scale, 640 * g_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80 * g_scale, 80 * g_scale), ImGuiCond_FirstUseEver);
        bool open = true;
        const ImGuiWindowFlags watchFlags = st.menuWatch ? (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus) : 0;
        if (st.menuWatch) ImGui::SetNextWindowBgAlpha(0.72f);
        if (ImGui::Begin("Master Looter", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | watchFlags))
        {
            // A line of explanation that outgrows the window wraps instead
            // of running off its edge. The English was written to fit; the
            // translations were not.
            ImGui::PushTextWrapPos(0.0f);
            // Title strip: serif name in gold, version and close on the right, bronze rule.
            ImGui::PushFont(g_fontHead);
            ImGui::TextColored(kGold, TR("MASTER LOOTER"));
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "  v%s", ML_VERSION);
            const float closeW = ImGui::CalcTextSize("Close").x + ImGui::CalcTextSize("Watch").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeW);
            if (ImGui::SmallButton(TR("Watch"))) st.menuWatch = true;
            if (ImGui::BeginItemTooltip()) { ImGui::Text(TR("Keep the menu on screen while you play. %s brings it back, %s closes it."), Settings::KeyName(c.menuKey), Settings::KeyName(c.keyWatch)); ImGui::EndTooltip(); }
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Close"))) open = false;
            if (st.menuWatch)
                ImGui::TextColored(kGoldDim, TR("Watch mode: the game has your controls. %s to interact, %s to close."), Settings::KeyName(c.menuKey), Settings::KeyName(c.keyWatch));
            {
                const ImVec2 a = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(a.x, a.y + 2), ImVec2(a.x + w, a.y + 2), ImGui::GetColorU32(kBronze), 1.0f);
                dl->AddLine(ImVec2(a.x, a.y + 5), ImVec2(a.x + w * 0.35f, a.y + 5), ImGui::GetColorU32(kGoldDim), 1.0f);
                ImGui::Dummy(ImVec2(0, 8 * g_scale));
            }
            struct TabDef { const char* name; void (*fn)(Config&); };
            static const TabDef tabs[] = {
                { "General", TabGeneral }, { "Looting", TabLooting }, { "Classes", TabClasses }, { "Tags", TabTags },
                { "Items", TabItems }, { "Nearby", [](Config&) { TabNearby(); } }, { "Status", [](Config&) { TabStatus(); } },
            };
            if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_DrawSelectedOverline))
            {
                for (const TabDef& t : tabs)
                {
                    // The label changes with the language; the id after
                    // ### does not, so the open tab survives a switch.
                    char label[96];
                    snprintf(label, sizeof label, "%s###%s", TR(t.name), t.name);
                    ImGui::PushFont(g_fontHead);
                    const bool sel = ImGui::BeginTabItem(label);
                    ImGui::PopFont();
                    if (sel) { ImGui::Dummy(ImVec2(0, 4 * g_scale)); t.fn(c); ImGui::EndTabItem(); }
                }
                ImGui::EndTabBar();
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
        if (!open) { st.menuOpen = false; st.menuWatch = false; }
        st.textCapture = capt && io.WantTextInput;
    }
}
