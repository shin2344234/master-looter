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
#include "../loot/hooks.h"
#include "../loot/mem.h"
#include "../version.h"
#include "stack_link.h"
#include "storage_link.h"

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
        // Same rule as the loot hotkeys: a bare key fires only with Ctrl and Alt
        // up, the pad chord always does. See State::HotkeysFree.
        const bool menuPad = hooks::PadChordHeld(c.padMenu);
        const bool key = front && (KeyDown(c.menuKey) || menuPad);
        if (key && !s_keyWas && !st.rebindCapture && (menuPad || State::HotkeysFree(c.menuKey)))
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

        const bool watchPad = hooks::PadChordHeld(c.padWatch);
        const bool watch = front && (KeyDown(c.keyWatch) || watchPad);
        if (watch && !s_watchWas && !st.rebindCapture && !st.textCapture && (watchPad || State::HotkeysFree(c.keyWatch)))
        {
            // Home: closed -> watching; interactive -> watching; watching -> closed.
            if (!st.menuOpen) { st.menuOpen = true; st.menuWatch = true; }
            else if (!st.menuWatch) st.menuWatch = true;
            else { st.menuOpen = false; st.menuWatch = false; }
            // RevOGUwU, #82. Watch mode wrote nothing anywhere: this branch had
            // no line of its own, and State::Captures() is false while watching,
            // so input::MenuOpened() never runs and "[input] menu opened" never
            // appears either. A player who opens the menu with this key and then
            // reports that it ignores the mouse hands over a log with no trace
            // of the menu at all, which is the report and the log both.
            LOG("[menu] watch key: menu %s", st.menuOpen
                    ? (st.menuWatch ? "watching; it is on screen and the game keeps your clicks" : "open")
                    : "closed");
            // Both of RevOGUwU's logs show the menu on screen for half an hour
            // and never opened with the menu key, so it was this key, and the
            // dim line under the title did not tell them why their clicks went
            // to the game. Say it once where it cannot be missed.
            if (st.menuOpen && st.menuWatch)
            {
                const std::string use = Settings::KeyName(c.menuKey);
                const std::string shut = Settings::KeyName(c.keyWatch);
                char msg[200];
                if (c.keyWatch)
                    snprintf(msg, sizeof msg, "Master Looter: menu in watch mode, so the game keeps your mouse. %s to use it, %s to close it.",
                             use.c_str(), shut.c_str());
                else
                    snprintf(msg, sizeof msg, "Master Looter: menu in watch mode, so the game keeps your mouse. %s to use it.", use.c_str());
                st.Notify(msg, 5000, true);
            }
        }
        s_watchWas = watch;

        // While the menu has input, Private Storage Master ignores its storage
        // keys, so binding Ctrl+F1 on the Storage tab does not open storage too.
        if (st.Captures())
            if (const psm::Api* api = psm::Get()) api->pauseInput(300);

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

    // Rebind's state while it listens, reset each time Rebind is clicked so a
    // capture the menu closed on cannot hand a stale modifier to the next.
    struct Capture
    {
        int  mod = 0;        // a modifier held down on its own, bound when let go
        bool wait = false;   // a combination was refused: bind nothing until every key is up
        bool combo = false;  // a combination was tried: say why it was refused
    };
    static Capture s_cap;

    // One frame of listening. Returns the key to bind, 0 for Escape, or -1 to
    // keep listening. Every key here is bound bare, and State::HotkeysFree
    // keeps a key quiet while Ctrl or Alt is held, so a combination can never
    // fire. A modifier on its own can be bound (trowieuk1 put the menu on
    // Ctrl), which made the old loop bind Left Ctrl the instant anyone started
    // a Ctrl+key combination: Ctrl goes down a frame before the key. Ahplla
    // tried combinations on 25 September 2026 and none of them read. So a
    // modifier is bound only when it is let go with nothing pressed alongside
    // it, and a key pressed with Ctrl or Alt held, or a second modifier, is
    // refused. The refusal holds until every key is up, because the keys of a
    // combination come up in any order and letting Ctrl go first would
    // otherwise bind the key that was just refused.
    static int CaptureStep(Capture& c, bool (*down)(int))
    {
        if (down(VK_ESCAPE)) return 0;
        const bool ctrlAlt = down(VK_LCONTROL) || down(VK_RCONTROL) || down(VK_LMENU) || down(VK_RMENU);
        int key = 0, mods = 0, otherMod = 0;
        for (int k = 0x08; k < 0xFF; ++k)
        {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue;
            if (!down(k)) continue;
            const bool mod = k == VK_LSHIFT || k == VK_RSHIFT || k == VK_LCONTROL || k == VK_RCONTROL || k == VK_LMENU || k == VK_RMENU;
            if (!mod) { if (!key) key = k; continue; }
            ++mods;
            if (k != c.mod) otherMod = k;
        }
        if (c.wait)
        {
            if (!key && !mods) c.wait = false;
            return -1;
        }
        if (key)
        {
            if (!ctrlAlt) return key;
            c.combo = true; c.wait = true; c.mod = 0;
            return -1;
        }
        if (mods)
        {
            if (!c.mod && mods == 1) c.mod = otherMod;
            else if (otherMod) { c.combo = true; c.wait = true; c.mod = 0; }
            return -1;
        }
        const int m = c.mod;
        c.mod = 0;
        return m ? m : -1;
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
            if (s_cap.combo) ImGui::TextColored(kWarn, "%s", TR("One key on its own. Ctrl and Alt combinations are never read, because these keys stay quiet while Ctrl or Alt is held."));
            const int got = CaptureStep(s_cap, KeyDown);
            if (got > 0) { vk = got; dirty = true; }
            if (got >= 0) { st.rebindCapture = false; g_rebindTarget = -1; s_cap = Capture{}; }
        }
        else if (!st.rebindCapture)
        {
            if (ImGui::SmallButton(TR("Rebind")))
            {
                st.rebindCapture = true; g_rebindTarget = target;
                s_cap = Capture{};
            }
            // Every key but the menu's own can be left unbound, which is what
            // the pad rows below have always allowed. Ahplla asked for it on 25
            // September 2026: a controller player with every spare key taken by
            // other mods, and a 0 in the ini came back as the default. The
            // menu key stays, because the menu is where a key gets bound again.
            if (target != 0 && vk) { ImGui::SameLine(); if (ImGui::SmallButton(TR("Clear"))) { vk = 0; dirty = true; } }
        }
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
        else ImGui::TextDisabled(TR("A backup is written when the game starts with settings changed since the last one, and the last twelve are kept, in MasterLooter.backups next to the plugin. Presets sit in MasterLooter.presets. Copy either folder to keep it across a reinstall."));
    }

    static void TabGeneral(Config& c)
    {
        bool dirty = false;
        if (ImGui::Checkbox(TR("Auto-loot enabled"), &c.enabled))
        {
            dirty = true;
            LOG("[loot] auto-loot switched %s in the menu", c.enabled ? "on" : "off");
            State::Get().Notify(c.enabled ? "Master Looter: auto-loot on" : "Master Looter: auto-loot off");
        }
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
        Help(TR("One key on its own. Ctrl and Alt combinations are never read, because these keys stay quiet while Ctrl or Alt is held."));
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
        Help(TR("Items tagged quest are left alone, so puzzles and story pickups are not auto-taken. It reads the item's own tags, so it covers anything you pick up and every node whose contents the mod can name. Two things fall outside it. A node it cannot name is decided by the switch for its kind, because there is no item there to read a tag from. And skinning a carcass pays out without the mod ever seeing what arrived."));
        dirty |= ImGui::Checkbox(TR("Skip quest equipment"), &c.skipQuestGear);
        Help(TR("Tools and gear the game marks important that no shop will buy: the fertilizer and lubricant sprayers, the Kuku spears, the scout rings and necklaces. 180 items, nearly all worth a single copper, and taking one off the floor early can put a quest step out of order.\n\nIt reads the two marks together, so an item carrying only one of them is not covered. The Field Sprayer is unsellable and not marked important, and still comes in."));
        dirty |= ImGui::Checkbox(TR("Skip items shops refuse to buy"), &c.skipNoSell);
        dirty |= ImGui::SliderInt(TR("Minimum value (copper)"), &c.minValueCopper, 0, 500, c.minValueCopper ? "%d" : "off");
        Help(TR("Items with an unknown value are never filtered by it."));
        dirty |= ImGui::Checkbox(TR("Take items the database cannot name"), &c.takeUnknownItems);
        Help(TR("Some world objects carry no readable item name. On: take them anyway. Off: leave them alone.\n\nThis covers loose items only. A pick-up the node table vouches for, such as a coin pile or a stack of gold bars, is taken whichever way this is set: the table naming the prefab is what identifies it, and the item inside it having no name is a separate question. Its own category switch still applies."));
        dirty |= ImGui::Checkbox(TR("Drop refused loot from bodies"), &c.dropRefused);
        Help(TR("Searching a body or a carcass hands over everything it holds, and the mod only sees it once it is in the bag. On: whatever of that your item rules, tags, classes or value floor refuse is put straight back on the ground beside you, through the same drop the game uses when you drop something yourself, one pile per stack. It covers the mod's own searches and the bodies a pet or a companion loots, whose refused items land beside you as well. A body you search yourself keeps what it paid. Money, documents, quest items and protected items stay in the bag."));
        dirty |= ImGui::Checkbox(TR("Stop pets picking up loose items"), &c.stopPetLooting);
        Help(TR("The game asks before a pet takes an item lying on the ground, and this answers no. The pet walks past it and nothing needs deleting afterwards. Whether a mercenary asks the same question has not been tested."));
        dirty |= ImGui::Checkbox(TR("Stop pets looting bodies"), &c.stopPetBodies);
        Help(TR("The game asks separately before a pet loots a body, and this answers no. Turn on one of the two and not the other if you want a pet stripping corpses but not collecting loose items, or the other way round."));
        dirty |= ImGui::Checkbox(TR("Pets and companions follow the filters"), &c.petFilter);
        // Two strings, so the first keeps its translations when the second changes.
        Help((std::string(TR("A pet loots whatever it likes and the game has no switch for it. On: a pet is told no before it reaches for a loose item your item rules, tags, classes or value floor refuse, and whatever it takes from a body that your item rules, tags or classes refuse is deleted from the inventory as it lands, with a notice saying what went. Quest and protected items are never deleted, and nothing already in your bag is touched.")) + "\n\n" + TR("Nothing you pick up yourself is ever deleted, whether or not a pet or a mercenary is out.")).c_str());
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
            { "Carcasses",      &c.lootCorpses,    "Skinning the animals you kill, once per carcass. People go under Bodies. The mod never sees what skinning pays out before it lands, so your filters cannot stop it arriving. To have what they refuse put back on the ground, turn on Drop refused loot from bodies under General." },
            { "Bodies",         &c.searchBodies,   "Searching the people you kill, once per body. Animals go under Carcasses. What a search pays out arrives before your filters see it, the same as a carcass, and Drop refused loot from bodies under General puts back down what they refuse." },
            { "Plants",         &c.gatherPlants,   "Herb, flower and mushroom nodes, and the same lying on the ground, seeds included. Food crops have their own switch. To keep the herbs and leave the seeds, or the other way round, refuse the class on the Classes tab: herb and seed are separate there. That sorts what is lying on the ground item by item, and it reaches the node as well wherever the game says what the node holds: refuse herb and a herb patch is left alone. Plant nodes are a mixed lot, paying grain and crafting materials too, and for the ones the game does not describe this switch is the only control." },
            { "Crops",          &c.gatherCrops,    "Vegetables, fruit and grain: sweet potato, barley, cabbage, apples, grapes and the rest of the farmed and foraged food, on the plant or lying loose. These used to answer to Ground items, which is why turning Plants off still emptied a field: of the 72 collection sockets in the game, 44 are crops and only 28 are plants." },
            { "Ore and stone",  &c.gatherOre,      "Every rock: ore and stone chunks on the ground, veins, boulders, rubble piles and quarry stone. A vein is broken where it stands and its contents picked up off the floor, which is what your pickaxe does and what makes a better pickaxe worth carrying: the tool's Mining Yield Up applies to the drop, not to the node. Each vein is struck once and left alone until the game brings it back. Reaching for veins starts as far out as the scan can see, because they take seconds to answer where a bush takes a fraction of one.\n\nStone used to have a switch of its own and the split never held: the game files most quarry stone under mining, so turning Stone off left it arriving anyway. To keep the ore and leave the stone, refuse the class stone on the Classes tab. That covers Stone, Fine Stone, Flawless Stone and Stalactite, and every rock the game says holds nothing else is then left standing, the mine rocks and the stalactites included. A vein that also holds ore or a jewel is still broken, since refusing the stone is not refusing what sits beside it. Where the game describes a node not at all, it takes ore, jewels and stone all being refused before that one is passed over." },
            { "Wood",           &c.gatherWood,     "Timber and branches on the ground, and the nodes that yield them. What most of these hold is read out of the game data itself, so refusing the class wood on the Classes tab leaves the trees standing as well as the timber under them. Where the game says nothing about a node, this switch is the only control it has." },
            { "Unidentified nodes", &c.gatherUnknown, "Nodes the prefab table does not name and that have not yielded anything yet this session. Off (the default) leaves them alone. On makes the mod gather them to find out, which means a plant can be taken while Plants is off." },
            { "Insects",        &c.catchInsects,   "Butterflies, beetles, dragonflies, bees, spiders, scorpions, snails and the other small things the game files as insects. Species come from the creature table; a creature the table cannot name is only caught when every category it could belong to is on (a flyer could be a fish, an insect or a bird)." },
            { "Fish",           &c.catchFish,      "Fish, and whatever else you catch in the water: crabs, shrimp, squid, starfish and seahorses." },
            { "Small animals",  &c.catchAnimals,   "Rats, squirrels, birds, lizards, frogs and salamanders: anything else the game puts in the bag whole." },
            { "Containers",     &c.lootContainers, "Two things that hold things. The chests, crates and drop-set nodes you open, which rarely answer the loot event. And anything on the ground whose job is to hold something: bottles, jars, clay pots, waterskins, vases and the storage boxes you place. Off by default, and off means a shelf of pottery is left where it stands. Reward boxes are not covered, since those are loot in their own right and belong to Treasure and keepsakes on the Classes tab.\n\nWhat comes out of a chest is not checked against your filters. The game hands the contents over in one go and the mod never sees them as objects, so for those this switch is all or nothing." },
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
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                    ImGui::TextUnformatted(TR(t.help));
                    // Sov1737, 24 September 2026: fireflies kept coming in with
                    // the insect class and the alchemy-material tag both
                    // refused. They were Firefly Colonies, which the creature
                    // table gives no item row, so only the catch switch could
                    // stop them until Decide's Catch case learned to ask the
                    // class rule for the creature table's own class. Tags and
                    // item rules still cannot reach them. A paragraph of its
                    // own, so the three help strings above keep their
                    // translations.
                    if (t.value == &c.catchInsects || t.value == &c.catchFish || t.value == &c.catchAnimals)
                    {
                        ImGui::Spacing();
                        ImGui::TextUnformatted(TR("A creature the game gives no item of its own, such as a Firefly Colony, is judged by this switch and by the "
                                                  "Classes tab, where a Firefly Colony counts as insect. No tag or item rule can reach it, because there is "
                                                  "no item to check one against."));
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
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
        dirty |= ImGui::SliderFloat(TR("Carcasses and bodies"), &c.corpseRange, 0.0f, 100.0f, c.corpseRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat(TR("Dead zone around you"), &c.minRange, 0.0f, 2.0f, "%.2f");
        Help(TR("Objects closer than this are treated as your own equipment. A safety net; your gear is also recognised by other means."));

        Section(TR("Reaching nodes"));
        dirty |= ImGui::Checkbox(TR("Arm nodes ourselves"), &c.autoArm);
        Help(TR("The game fills a node's data only when it thinks you can reach it. Arming asks it to do that from a distance, which is also what lets an ore vein be harvested where it stands. Veins take several seconds to answer where a bush takes a fraction of one."));
        dirty |= ImGui::SliderFloat(TR("Arming range"), &c.armRange, 0.0f, 60.0f, c.armRange > 0 ? "%.0f" : "same as gathering");
        Help(TR("Arming ignores walls. Keep it short or you will gather through the wall of the next room. Ore is the exception and is always reached for at up to 25 m, because a vein takes seconds to answer and asking only once you are on top of it means it opens too late to be any use."));
        dirty |= ImGui::Checkbox(TR("Mine ore veins for you"), &c.gatherVeins);
        Help(TR("On, a vein is harvested where it stands and you never swing a pickaxe. Off, veins are left alone and only the chunks you knock loose are picked up. Needs arming, since asking the vein to open is the whole trick."));
        dirty |= ImGui::Checkbox(TR("Draw water from wells"), &c.drawWells);
        Help(TR("Winds the handle for you and takes what comes up, by driving the same state transitions the game drives when you hold the interact button. A well raises no loot event at all, so this is a replay of a captured sequence rather than the game being asked for anything. Off by default, because a well it gets wrong is a well left standing without its bucket."));
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
        { "Materials",           "ore jewel stone wood hide fabric bone crafting-material alchemy-material herb ingredient seed trade-good bait", "Ore, gems, stone, timber, hides, herbs, trade goods and other crafting input." },
        { "Creatures",           "insect fish animal amphibian", "Caught creatures, live or lying around; a creature the table can name follows its class rule. Crabs, shrimp and squid are seafood under Food and drink." },
        { "Ammunition",          "arrow ammo ammo-bundle bullet magic-bullet cannonball explosive", "" },
        { "Books and papers",    "book document note poster skill-poster bounty-notice treasure-map legendary-animal-report skill-book recipe-book recipe-food recipe-potion recipe-furniture recipe-abyss-gear recipe-armor", "" },
        { "Furniture and decor", "furniture household dye lamp light ornament painting flower-pot decoration cooking-facility storage container", "Household clutter and collectibles, most of it worthless." },
        { "Mounts and vehicles", "mount-gear mount-feed mount-utility pet-gear vehicle-part", "" },
        { "Treasure and keepsakes", "treasure sealed-artifact artifact keepsake currency chest", "Boss rewards, artifacts, memory items, coin pouches and reward chests." },
        { "Keys and tools",      "key key-item tool", "" },
    };

    // Every name in a group has to be a class the database knows, and five of
    // them were not: metal is nothing at all, and catalyst, goods, recipe and
    // gimmick are tags. A rule written under one of those names filters
    // nothing, and the Loot all button walks the database, so it could never
    // clear them: one click of a group off left the group reading mixed for the
    // rest of that install, with the square that Seth reported on 11 September
    // 2026 on Materials, Books and papers, and Keys and tools. Both walkers go
    // through here now and skip anything the database does not carry, so a name
    // that goes stale is ignored instead of jamming a checkbox.
    // Classes() is a vector of pairs, so this is the lookup it does not have.
    static bool KnownClass(const std::string& cls)
    {
        for (const auto& kv : ItemDb::Classes()) if (kv.first == cls) return true;
        return false;
    }

    static void ForEachClass(const ClassGroup& g, void (*fn)(const std::string&, void*), void* ctx)
    {
        std::string cls;
        for (const char* p = g.classes;; ++p)
        {
            if (*p && *p != ' ') { cls += *p; continue; }
            if (!cls.empty())
            {
                if (KnownClass(cls)) fn(cls, ctx);
                cls.clear();
            }
            if (!*p) break;
        }
    }

    // 1 all on, 0 all off, 2 mixed.
    static int GroupState(const Config& c, const ClassGroup& g)
    {
        struct Tally { const Config* c; int on, off; } t{ &c, 0, 0 };
        ForEachClass(g, [](const std::string& cls, void* p) {
            Tally& t = *static_cast<Tally*>(p);
            auto it = t.c->classRule.find(cls);
            ((it == t.c->classRule.end() || it->second != 0) ? t.on : t.off)++;
        }, &t);
        if (!t.on && !t.off) return 1;      // a group with nothing left in it
        return t.off == 0 ? 1 : t.on == 0 ? 0 : 2;
    }
    static void SetGroup(Config& c, const ClassGroup& g, bool loot)
    {
        struct Set { Config* c; bool loot; } s{ &c, loot };
        ForEachClass(g, [](const std::string& cls, void* p) {
            Set& s = *static_cast<Set*>(p);
            s.c->classRule[cls] = s.loot ? 1 : 0;
        }, &s);
    }

    // Said once, the first time the tab is drawn with the database loaded. A
    // stale name costs nothing now, but it means a group quietly covers less
    // than its tooltip claims, and nothing else would ever say so.
    static void CheckGroupNames()
    {
        static bool done = false;
        if (done || !ItemDb::Loaded()) return;
        done = true;
        std::string bad;
        for (const ClassGroup& g : kGroups)
        {
            std::string cls;
            for (const char* p = g.classes;; ++p)
            {
                if (*p && *p != ' ') { cls += *p; continue; }
                if (!cls.empty())
                {
                    if (!KnownClass(cls)) { bad += bad.empty() ? "" : ", "; bad += cls; }
                    cls.clear();
                }
                if (!*p) break;
            }
        }
        if (!bad.empty())
            LOG_ERR("[menu] the class groups name %s, which the item database does not carry, so those parts of a group "
                    "switch nothing.", bad.c_str());
    }

    static void TabClasses(Config& c)
    {
        CheckGroupNames();
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
            bool qgear = !c.skipQuestGear;
            if (ImGui::Checkbox(TR("Quest equipment"), &qgear)) { c.skipQuestGear = !qgear; Settings::MarkDirty(); }
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(TR("Gear marked important that no shop will buy, across every class. Off leaves quest tools where they lie.")); ImGui::EndTooltip(); }
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
        ImGui::Text(TR("Master Looter v%s for game build %s"), ML_VERSION_FULL, ML_GAME_BUILD);
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
        // The pet-looting switch is the one setting in the mod that cannot
        // work at all when its hook is missing, and the line saying so goes
        // in at startup, long out of the log panel by the time anyone looks.
        OnOff("Stop pets looting", loot::hooks::PetLootingHooked(), "hooked, both switches work",
              "not available on this build, neither switch does anything");
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
            // Every line is "[HH:MM:SS.mmm] [level] text", so the level sits at
            // a fixed offset and the panel can colour by it. Only the two lines
            // anybody scrolls this panel looking for are coloured: something
            // that failed, and something that came up that had to. The rest
            // stays plain so the colours mean something.
            for (const auto& l : lines)
            {
                const char* lvl = l.size() > 21 ? l.c_str() + 16 : nullptr;
                if (lvl && !strncmp(lvl, "error", 5))    ImGui::TextColored(kWarn, "%s", l.c_str());
                else if (lvl && !strncmp(lvl, "ok", 2))  ImGui::TextColored(kGood, "%s", l.c_str());
                else                                     ImGui::TextUnformatted(l.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    // --- Storage tab --------------------------------------------------------
    // Private Storage Master's settings, when that plugin is installed. It has
    // no menu of its own and owns its ini; this tab reads and writes through
    // its exported interface (storage_link.h) and never touches the file.
    static PsmSettings s_psm{};
    static bool        s_psmFetched = false;
    static char        s_psmWhy[256] = "";
    static unsigned    s_psmPadWidest = 0, s_psmPadLast = 0, s_psmPadPrev = 0;
    constexpr int      kPsmKeyTarget = 200;   // g_rebindTarget for key rows, 200..210
    constexpr int      kPsmBlockRow  = PSM_STORAGES + 1;   // the key-block toggle, after the dump key
    // PSM 1.0.1's key block, which PsmSettings has no room for. Valid only
    // when the plugin exports both halves and the last read succeeded.
    static PsmKeyBlock s_psmBlock{};
    static bool        s_psmBlockOk = false;
    // Auto-store, from a Private Storage Master that exports it. Valid only
    // when the last read succeeded.
    static PsmAutoStore s_psmAuto{};
    static bool         s_psmAutoOk = false;
    // Items auto-store never moves, -1 until read.
    static uint16_t     s_psmNever[PSM_NEVER_MOVE_MAX] = {};
    static int          s_psmNeverN = -1;
    // The order it offers an item to the storages, so the list reads as it works.
    static const int kPsmAutoOrder[] = { 4, 8, 1, 3, 6, 5, 2, 0 };
    constexpr int      kPsmPadTarget = 220;   // pad rows, 220..228

    static const char* const kPsmHelp[PSM_STORAGES] = {
        "Private Storage, the storage box at camp.",
        "The Gatherables Chest in your house.",
        "The Wardrobe in your house.",
        "The Kuku Cooler in your house.",
        "The Collectibles Chest in your house. It takes one of each collectible.",
        "Camp Straw, the camp feed bin.",
        "Bird Feed, the bird feeder at camp.",
        "Camp Provisions, the warehouse for packaged trade goods kept at town warehouse keepers.",
        "Abyss gear storage, which the game calls the Kuku Pot bag.",
    };

    static void PsmLogApply(const char* what, bool ok, const char* why);

    static void PsmFetch(const psm::Api* api)
    {
        s_psm.size = sizeof s_psm;
        if (api->getSettings(&s_psm)) s_psmFetched = true;
        s_psmBlock.size = sizeof s_psmBlock;
        s_psmBlockOk = api->getKeyBlock && api->getKeyBlock(&s_psmBlock, 0);
        s_psmAuto.size = sizeof s_psmAuto;
        s_psmAutoOk = api->getAutoStore && api->getAutoStore(&s_psmAuto, 0);
        s_psmNeverN = api->getNeverMove ? api->getNeverMove(s_psmNever, PSM_NEVER_MOVE_MAX, 0) : -1;
    }

    static bool PsmApplyNever(const psm::Api* api, const uint16_t* items, int n, const char* what)
    {
        char why[256] = {};
        const bool ok = api->applyNeverMove(items, n, why, sizeof why) != 0;
        if (Settings::Get().debugLog)
            LOG("[storage] never-move list %s %s: %d items%s%s", what, ok ? "saved" : "refused", n, why[0] ? " | " : "", why);
        if (ok) s_psmWhy[0] = 0;
        else snprintf(s_psmWhy, sizeof s_psmWhy, "%s", why[0] ? why : TR("Private Storage Master refused the change"));
        PsmFetch(api);
        return ok;
    }

    static const char* PsmItemName(uint16_t row)
    {
        const Item* it = ItemDb::ByRow(row);
        return it && !it->name.empty() ? it->Label() : nullptr;
    }

    // Items auto-store always leaves in the bag. PSM keeps the list and saves it;
    // this edits a copy and hands the whole list back on every change.
    static void PsmNeverMoveList(const psm::Api* api)
    {
        if (!api->getNeverMove || s_psmNeverN < 0) return;
        ImGui::TextUnformatted(TR("Never move"));
        // Two strings, so the first keeps its translations. The Arrow joined
        // Private Storage Master's defaults in its 1.1.4 after Fyreon87's log
        // of 24 September 2026 showed arrows picked back up going to storage.
        Help((std::string(TR("Items auto-store always leaves in the bag. The default is every currency: silver, coin pouches, gold bars, camp funds and supplies, tokens and bonds. Reset to defaults below puts that list back."))
              + "\n\n" + TR("Since Private Storage Master 1.1.4 the Arrow is on it by default as well, so arrows you pick back up stay in the quiver.")).c_str());
        int drop = -1;
        if (ImGui::BeginChild("##nevermove", ImVec2(0, 150 * g_scale), ImGuiChildFlags_Borders))
        {
            if (!s_psmNeverN) ImGui::TextDisabled("%s", TR("Empty: auto-store may move anything a storage takes."));
            for (int i = 0; i < s_psmNeverN; ++i)
            {
                ImGui::PushID(500 + i);
                if (ImGui::SmallButton(TR("Remove"))) drop = i;
                ImGui::SameLine();
                if (const char* name = PsmItemName(s_psmNever[i])) ImGui::TextUnformatted(name);
                else ImGui::TextDisabled(TR("item %u"), static_cast<unsigned>(s_psmNever[i]));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (drop >= 0)
        {
            uint16_t next[PSM_NEVER_MOVE_MAX];
            int n = 0;
            for (int i = 0; i < s_psmNeverN; ++i) if (i != drop) next[n++] = s_psmNever[i];
            PsmApplyNever(api, next, n, "remove");
        }

        static char query[64] = "";
        if (s_psmNeverN >= PSM_NEVER_MOVE_MAX)
        {
            ImGui::TextDisabled(TR("The list is full at %d items."), PSM_NEVER_MOVE_MAX);
            return;
        }
        ImGui::SetNextItemWidth(320 * g_scale);
        ImGui::InputTextWithHint("##neveradd", TR("add an item: type 3+ letters of its name"), query, sizeof query);
        const std::string q = Lower(query);
        if (q.size() < 3) return;
        int shown = 0;
        for (const Item& it : ItemDb::All())
        {
            if (it.row < 0 || it.row > 0xFFFF || it.name.empty() || !Contains(it.name, q)) continue;
            bool listed = false;
            for (int i = 0; i < s_psmNeverN; ++i) if (s_psmNever[i] == it.row) listed = true;
            if (listed) continue;
            if (++shown > 8) { ImGui::TextDisabled("%s", TR("more matches; type more of the name")); break; }
            ImGui::PushID(600 + shown);
            if (ImGui::SmallButton(TR("Add")))
            {
                uint16_t next[PSM_NEVER_MOVE_MAX];
                for (int i = 0; i < s_psmNeverN; ++i) next[i] = s_psmNever[i];
                next[s_psmNeverN] = static_cast<uint16_t>(it.row);
                if (PsmApplyNever(api, next, s_psmNeverN + 1, "add")) query[0] = 0;
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(it.Label());
            ImGui::PopID();
        }
        if (!shown) ImGui::TextDisabled("%s", TR("no item by that name that is not listed already"));
    }

    static bool PsmApplyAuto(const psm::Api* api, const char* what = "auto-store")
    {
        s_psmAuto.size = sizeof s_psmAuto;
        char why[256] = {};
        const bool ok = api->applyAutoStore(&s_psmAuto, why, sizeof why) != 0;
        if (Settings::Get().debugLog)
            LOG("[storage] %s %s: on %d, storages %d %d %d %d %d %d %d %d %d, only what was picked up %d%s%s", what,
                ok ? "saved" : "refused", s_psmAuto.enabled, s_psmAuto.storages[0], s_psmAuto.storages[1], s_psmAuto.storages[2],
                s_psmAuto.storages[3], s_psmAuto.storages[4], s_psmAuto.storages[5], s_psmAuto.storages[6], s_psmAuto.storages[7],
                s_psmAuto.storages[8], s_psmAuto.onlyGained, why[0] ? " | " : "", why);
        if (ok) s_psmWhy[0] = 0;
        else snprintf(s_psmWhy, sizeof s_psmWhy, "%s", why[0] ? why : TR("Private Storage Master refused the change"));
        // Read back what it kept.
        PsmFetch(api);
        return ok;
    }

    static bool PsmApplyBlock(const psm::Api* api, const char* what = "key block")
    {
        s_psmBlock.size = sizeof s_psmBlock;
        char why[256] = {};
        const bool ok = api->applyKeyBlock(&s_psmBlock, why, sizeof why) != 0;
        PsmLogApply(what, ok, why);
        if (ok) s_psmWhy[0] = 0;
        else snprintf(s_psmWhy, sizeof s_psmWhy, "%s", why[0] ? why : TR("Private Storage Master refused the change"));
        // It turns a clashing toggle off, so show what it kept.
        PsmFetch(api);
        return ok;
    }

    // Every save the tab makes, what it sent and what came back. Private
    // Storage Master logs its own saves only while its detailed log is on, and
    // a reset to defaults turns that off, so a reset that worked left nothing
    // in either log on 18 September 2026 and could not be told from one that
    // never ran.
    static void PsmLogApply(const char* what, bool ok, const char* why)
    {
        if (!Settings::Get().debugLog) return;
        LOG("[storage] %s %s: on %d, detailed log %d, sizes %d %d %d %d %d %d %d %d %d, leave alone %d, expansions %d, "
            "key block %s, toggle vk %02X mods %u%s%s",
            what, ok ? "saved" : "refused", s_psm.enabled, s_psm.debugLog,
            s_psm.slots[0], s_psm.slots[1], s_psm.slots[2], s_psm.slots[3], s_psm.slots[4], s_psm.slots[5], s_psm.slots[6],
            s_psm.slots[7], s_psm.slots[8], s_psm.leaveCapacityAlone, s_psm.privateStorageExpansions,
            s_psmBlockOk ? (s_psmBlock.on ? "on" : "off") : "not offered", s_psmBlock.toggleKey.vk, s_psmBlock.toggleKey.mods,
            why && why[0] ? " | " : "", why && why[0] ? why : "");
    }

    static bool PsmApply(const psm::Api* api, const char* what = "settings")
    {
        s_psm.size = sizeof s_psm;
        char why[256] = {};
        const bool ok = api->applySettings(&s_psm, why, sizeof why) != 0;
        PsmLogApply(what, ok, why);
        if (ok) s_psmWhy[0] = 0;
        else snprintf(s_psmWhy, sizeof s_psmWhy, "%s", why[0] ? why : TR("Private Storage Master refused the change"));
        // It turns a duplicate binding off, so show what it kept.
        PsmFetch(api);
        return ok;
    }

    static int PsmBits(unsigned v)
    {
        int n = 0;
        for (; v; v &= v - 1) ++n;
        return n;
    }

    // Who else uses this key: another storage, the dump key, the key-block
    // toggle, or one of Master Looter's own.
    static const char* PsmKeyClash(const psm::Api* api, int row, PsmKey k, const Config& c)
    {
        if (!k.vk) return nullptr;
        const int last = s_psmBlockOk ? kPsmBlockRow : PSM_STORAGES;
        for (int i = 0; i <= last; ++i)
        {
            if (i == row) continue;
            const PsmKey o = i < PSM_STORAGES ? s_psm.keys[i] : i == PSM_STORAGES ? s_psm.dumpKey : s_psmBlock.toggleKey;
            if (o.vk == k.vk && o.mods == k.mods)
                return i < PSM_STORAGES ? api->storageName(i) : i == PSM_STORAGES ? TR("the size dump") : TR("the key-block toggle");
        }
        // Master Looter's keys are bare and stay quiet while Ctrl or Alt is
        // held (State::HotkeysFree), so only a bare key or a Shift one can
        // reach them, and a Shift one only while the key block is off.
        if (k.mods & (PSM_MOD_CTRL | PSM_MOD_ALT)) return nullptr;
        if (k.mods && s_psmBlockOk && s_psmBlock.on) return nullptr;
        if (k.vk == c.menuKey || k.vk == c.keyToggle || k.vk == c.keyBurst || k.vk == c.keyWatch || k.vk == c.keyOwned)
            return TR("a Master Looter key");
        return nullptr;
    }

    static const char* PsmPadClash(const psm::Api* api, int row, PsmPad p, const Config& c)
    {
        if (!p.press) return nullptr;
        for (int i = 0; i < PSM_STORAGES; ++i)
            if (i != row && s_psm.pads[i].press == p.press && s_psm.pads[i].hold == p.hold) return api->storageName(i);
        const unsigned mask = static_cast<unsigned>(p.hold | p.press);
        if (mask == c.padMenu || mask == c.padToggle || mask == c.padOwned || mask == c.padBurst || mask == c.padWatch)
            return TR("a Master Looter shortcut");
        return nullptr;
    }

    // Key with Ctrl, Shift or Alt. Master Looter's own KeyRow takes a bare key;
    // storage keys are usually Ctrl+F-something, so this one keeps the modifiers.
    static bool PsmKeyRow(const psm::Api* api, const char* label, const char* help, PsmKey& key, int target, int row, const Config& c)
    {
        State& st = State::Get();
        bool changed = false;
        ImGui::PushID(target);
        ImGui::TextUnformatted(label);
        if (help) Help(help);
        s_rowLabel.Next();
        char text[48];
        api->keyText(key, text, sizeof text);
        ImGui::TextUnformatted(text);
        s_rowValue.Next();
        if (st.rebindCapture && g_rebindTarget == target)
        {
            ImGui::TextColored(Accent(), TR("press a key, with Ctrl, Shift or Alt if you want (Escape cancels)"));
            for (int k = 0x08; k < 0xFF; ++k)
            {
                if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU || (k >= VK_LSHIFT && k <= VK_RMENU) || k == VK_LWIN || k == VK_RWIN) continue;
                if (!KeyDown(k)) continue;
                if (k != VK_ESCAPE)
                {
                    key.vk = static_cast<uint8_t>(k);
                    key.mods = static_cast<uint8_t>((KeyDown(VK_CONTROL) ? PSM_MOD_CTRL : 0) | (KeyDown(VK_SHIFT) ? PSM_MOD_SHIFT : 0) |
                                                    (KeyDown(VK_MENU) ? PSM_MOD_ALT : 0));
                    changed = true;
                }
                st.rebindCapture = false;
                g_rebindTarget = -1;
                break;
            }
        }
        else if (!st.rebindCapture)
        {
            if (ImGui::SmallButton(TR("Rebind"))) { st.rebindCapture = true; g_rebindTarget = target; }
            if (key.vk) { ImGui::SameLine(); if (ImGui::SmallButton(TR("Clear"))) { key = PsmKey{}; changed = true; } }
            if (const char* clash = PsmKeyClash(api, row, key, c)) { ImGui::SameLine(); ImGui::TextColored(kWarn, TR("also %s"), clash); }
        }
        ImGui::PopID();
        return changed;
    }

    // A storage combo is held buttons plus one pressed button, and the order
    // matters: LB+LS is hold LB, click the stick. Capture remembers which
    // button went down last and makes that the pressed one.
    static bool PsmPadRow(const psm::Api* api, const char* label, PsmPad& pad, int target, int row, const Config& c)
    {
        State& st = State::Get();
        bool changed = false;
        ImGui::PushID(target);
        ImGui::TextUnformatted(label);
        s_rowLabel.Next();
        char text[48];
        api->padText(pad, text, sizeof text);
        ImGui::TextDisabled("%s", text);
        s_rowValue.Next();
        if (st.rebindCapture && g_rebindTarget == target)
        {
            const unsigned held = hooks::PadButtons();
            const unsigned fresh = held & ~s_psmPadPrev;
            if (fresh)
            {
                s_psmPadLast = fresh & (~fresh + 1);
                s_psmPadWidest |= held;
            }
            s_psmPadPrev = held;
            const bool done = !held && PsmBits(s_psmPadWidest) >= 2;
            if (KeyDown(VK_ESCAPE)) { st.rebindCapture = false; g_rebindTarget = -1; s_psmPadWidest = s_psmPadLast = s_psmPadPrev = 0; }
            else if (done)
            {
                pad.press = static_cast<uint16_t>(s_psmPadLast);
                pad.hold = static_cast<uint16_t>(s_psmPadWidest & ~s_psmPadLast);
                changed = true;
                st.rebindCapture = false;
                g_rebindTarget = -1;
                s_psmPadWidest = s_psmPadLast = s_psmPadPrev = 0;
            }
            else if (!held && s_psmPadWidest) s_psmPadWidest = s_psmPadLast = 0;   // one button is not a combo, start over
            else if (s_psmPadWidest)
            {
                PsmPad preview{static_cast<uint16_t>(s_psmPadWidest & ~s_psmPadLast), static_cast<uint16_t>(s_psmPadLast)};
                api->padText(preview, text, sizeof text);
                ImGui::TextColored(Accent(), TR("%s, let go to keep it"), text);
            }
            else ImGui::TextColored(Accent(), TR("hold a button, then press the one that opens it (Escape cancels)"));
        }
        else if (!st.rebindCapture)
        {
            if (ImGui::SmallButton(TR("Set"))) { st.rebindCapture = true; g_rebindTarget = target; s_psmPadWidest = s_psmPadLast = 0; s_psmPadPrev = hooks::PadButtons(); }
            if (pad.press) { ImGui::SameLine(); if (ImGui::SmallButton(TR("Clear"))) { pad = PsmPad{}; changed = true; } }
            if (const char* clash = PsmPadClash(api, row, pad, c)) { ImGui::SameLine(); ImGui::TextColored(kWarn, TR("also %s"), clash); }
        }
        ImGui::PopID();
        return changed;
    }

    // The item stack multiplier, from Stack Master or from Private Storage
    // Master, whichever is applying it. Nothing here knows which: the link
    // picks the provider and this edits it. The multiplier is read at startup
    // by that mod, so a change here only shows in the game after a restart,
    // which the tab says in three places because it is the one thing that
    // confuses people about the feature.
    static char s_stackWhy[192] = {};

    static void TabStacks(Config&)
    {
        const stack::Api* api = stack::Get();
        if (!api)
        {
            ImGui::TextColored(kWarn, TR("Item stacks: %s"), stack::Why());
            ImGui::TextWrapped("%s", TR("This tab needs Stack Master, or a build of Private Storage Master that raises stack limits."));
            return;
        }
        StackStatus s{};
        s.size = sizeof s;
        if (!api->getStatus(&s))
        {
            ImGui::TextColored(kWarn, "%s", TR("That mod did not hand over its stack settings."));
            return;
        }

        ImGui::Text(TR("%s %s for game build %s"), s.provider, s.version, s.gameVersion);
        ImGui::TextDisabled("%s", TR("Every stackable item's limit is multiplied when the game starts. The setting is saved in that mod's own ini as soon as you pick it."));
        if (s_stackWhy[0]) ImGui::TextColored(kWarn, "%s", s_stackWhy);
        if (stack::RefusedOther()[0]) ImGui::TextColored(kWarn, "%s", stack::RefusedOther());
        // liveRaise arrived after the first layout, so it counts only when the
        // provider says it wrote that far. A provider built before it fills
        // less and the tab keeps the old wording, which is still true there.
        const bool liveOk = s.size >= static_cast<uint32_t>(STACK_STATUS_V1) + sizeof s.liveRaise;
        const bool live = liveOk && s.liveRaise != 0;
        // A raise still waiting where the provider can apply one means the
        // game was busy with a storage screen, a shop, a cutscene or a load
        // when it was picked. The value is saved either way, so picking it
        // again out in the world costs nothing and saves the restart.
        if (s.restartNeeded && live && s.multiplierSetting > s.multiplier)
            ImGui::TextColored(kGold, "%s", TR("Waiting for a restart, or pick it again out in the world and it takes hold at once."));
        else if (s.restartNeeded)
            ImGui::TextColored(kGold, "%s", TR("Restart the game for the multiplier you picked to take effect."));
        else if (live)
            ImGui::TextColored(kGoldDim, "%s", TR("A bigger multiplier takes hold as soon as you pick it. A smaller one waits for the next launch."));

        Section(TR("Status"));
        OnOff("Raising stack limits", s.applying != 0, "yes", "no");
        ImGui::TextUnformatted(TR("Why")); s_statusLabel.Next();
        ImGui::TextColored(s.applying ? kGood : kMuted, "%s", stack::StandDownText(s.standDownReason));
        ImGui::TextUnformatted(TR("This launch")); s_statusLabel.Next();
        // Named here as well as in the line above, because with both mods
        // installed the one applying the multiplier is not the one a player
        // went looking for.
        if (s.multiplier > 1) ImGui::TextColored(kGood, TR("x%d, by %s"), s.multiplier, s.provider);
        else                  ImGui::TextColored(kMuted, "%s", TR("stacks are the size the game ships"));
        ImGui::TextUnformatted(TR("Saved")); s_statusLabel.Next();
        if (s.multiplierSetting > 1) ImGui::Text(TR("x%d"), s.multiplierSetting);
        else                         ImGui::TextUnformatted(TR("off"));
        if (s.applying && s.itemsRaised)
        {
            ImGui::TextUnformatted(TR("Items raised")); s_statusLabel.Next();
            ImGui::Text(TR("%d of %d, the rest do not stack"), s.itemsRaised, s.itemsRaised + s.itemsUnstackable);
            ImGui::TextUnformatted(TR("Largest limit")); s_statusLabel.Next();
            ImGui::Text("%lld", static_cast<long long>(s.biggest));
            // The provider's own ceiling, not the header's: a build of it
            // newer than this menu may allow more.
            if (s.ceiling > 0 && s.biggest >= s.ceiling)
                ImGui::TextDisabled(TR("Some items reached the largest stack that mod allows, %d, and stopped there."), s.ceiling);
        }

        Section(TR("Stack multiplier"));
        // A provider that has stood aside for another mod keeps standing
        // aside, so a multiplier written here would be saved, would ask for a
        // restart, and would change nothing after it. That happens when the
        // mod it stood aside for is one this build cannot talk to, which the
        // line above names.
        const bool futile = !s.applying && s.standDownReason == STACK_STANDDOWN_OTHER_MOD;
        // Only one of these can promise the player somewhere to go. A mod
        // that stood aside for something this build cannot name leaves the
        // second sentence, which says so rather than pointing at nothing.
        if (futile && stack::RefusedModule()[0])
            ImGui::TextColored(kWarn, TR("%s is not the mod setting stack sizes, so changing it here would do nothing. Set the multiplier in %s instead."), s.provider, stack::RefusedModule());
        else if (futile)
            ImGui::TextColored(kWarn, TR("%s has stood aside for another mod, so changing it here would do nothing. That mod is not one this menu can reach, so set the multiplier in it directly."), s.provider);
        ImGui::BeginDisabled(futile);
        static const int kPicks[] = { 1, 2, 3, 5, 10, 20, 50, 100 };
        for (int i = 0; i < static_cast<int>(sizeof kPicks / sizeof kPicks[0]); ++i)
        {
            // A provider that accepts less than 100 would refuse the pick with
            // a reason; better not to offer it. Its own maximum, since that is
            // what it will measure the value against.
            if (s.maxMultiplier > 0 && kPicks[i] > s.maxMultiplier) break;
            char label[32];
            if (kPicks[i] == 1) snprintf(label, sizeof label, "%s", TR("Off"));
            else                snprintf(label, sizeof label, "x%d", kPicks[i]);
            ImGui::PushID(700 + i);
            if (ImGui::RadioButton(label, s.multiplierSetting == kPicks[i]) && s.multiplierSetting != kPicks[i])
            {
                char why[160] = {};
                // Written down because the provider's own log records what it
                // did and not who asked: a multiplier that arrived from this
                // menu and one typed into the ini read the same there.
                if (api->applyMultiplier(kPicks[i], why, sizeof why))
                {
                    s_stackWhy[0] = 0;
                    LOG("[stacks] asked %s for x%d", s.provider, kPicks[i]);
                }
                else
                {
                    snprintf(s_stackWhy, sizeof s_stackWhy, TR("%s refused the change: %s"), s.provider, why);
                    LOG_ERR("[stacks] %s refused x%d: %s", s.provider, kPicks[i], why);
                }
            }
            ImGui::PopID();
            if (i != 3 && i != 7) ImGui::SameLine();
        }
        ImGui::EndDisabled();
        if (live)
            Help("Most things the game lets you stack 100 of, so at x10 they hold 1000. It applies to everything that stacks and never to what does not, like weapons and armour, "
                 "and the deepest stacks in the game stop at the limit that mod reports above. Picking a bigger multiplier takes hold at once; a smaller one waits for the next launch, "
                 "because a stack already built cannot be shrunk under what is in it.");
        else
            Help("Most things the game lets you stack 100 of, so at x10 they hold 1000. It applies to everything that stacks and never to what does not, like weapons and armour, "
                 "and the deepest stacks in the game stop at the limit that mod reports above. The multiplier is read when the game starts, so the game has to be restarted before it shows.");
        ImGui::TextColored(kWarn, "%s", TR("Empty your big stacks before turning this down or off."));
        Help("Lowering the multiplier does not shrink a stack you already built. A slot holding more than the game now allows keeps what is in it until you take some out, "
             "and anything above the new limit can be lost.");
    }

    static void TabStorage(Config& c)
    {
        const psm::Api* api = psm::Get();
        if (!api)
        {
            ImGui::TextColored(kWarn, TR("Private Storage Master: %s"), psm::Why());
            ImGui::TextWrapped("%s", TR("This tab needs a build of Private Storage Master that matches this version of Master Looter."));
            return;
        }

        PsmStatus status{};
        status.size = sizeof status;
        api->getStatus(&status);
        // Take the plugin's settings fresh unless a slider or field here is mid-edit,
        // so a hand edit of the ini and Reload show up.
        if (!s_psmFetched || !ImGui::IsAnyItemActive()) PsmFetch(api);

        ImGui::Text(TR("Private Storage Master %s for game build %s"), status.version, status.gameVersion);
        ImGui::TextDisabled("%s", TR("Its settings live in PrivateStorageMaster.ini beside the plugin. Changes here are saved there at once."));
        if (s_psmWhy[0]) ImGui::TextColored(kWarn, "%s", s_psmWhy);
        if (status.restartNeeded) ImGui::TextColored(kGold, "%s", TR("Some changes take effect the next time the game starts."));

        Section(TR("Status"));
        OnOff("Storage keys", status.storageReady != 0, "working", status.enabled ? "off, see the error below" : "off, the mod is disabled");
        OnOff("Storage sizes", status.capacityHooked != 0, "set this launch", "left as the game has them");
        OnOff("Game window", status.keyWindowFound != 0, "found", "not found, bound keys also reach the game");
        ImGui::TextUnformatted(TR("Controller")); s_statusLabel.Next();
        if (status.padSlot >= 0) ImGui::TextColored(kGood, TR("on XInput slot %d"), status.padSlot);
        else ImGui::TextColored(kMuted, "%s", TR("none seen"));
        ImGui::TextUnformatted(TR("Open now")); s_statusLabel.Next();
        ImGui::TextUnformatted(status.openStorage >= 0 ? api->storageName(status.openStorage) : TR("nothing"));
        if (status.oldModLoaded)
            ImGui::TextColored(kWarn, "%s", TR("PrivateStorageAnywhere.asi is also installed. The two change the same parts of the game; remove that one."));
        if (status.imported)
            ImGui::TextDisabled("%s", TR("The key bindings were brought over from PrivateStorageAnywhere.ini on the first run."));
        if (status.lastError[0])
        {
            ImGui::TextUnformatted(TR("Last error")); s_statusLabel.Next();
            ImGui::TextColored(kWarn, "%s", status.lastError);
        }

        Section(TR("General"));
        {
            bool enabled = s_psm.enabled != 0;
            if (ImGui::Checkbox(TR("Private Storage Master on"), &enabled)) { s_psm.enabled = enabled; PsmApply(api); }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("takes effect next launch"));
            bool debug = s_psm.debugLog != 0;
            if (ImGui::Checkbox(TR("Detailed log"), &debug)) { s_psm.debugLog = debug; PsmApply(api); }
            // 1.0.1 started rotating its logs, and the key-block exports came
            // with it, so their presence says which text is true.
            if (api->getKeyBlock)
                Help("Writes every key press, open and close to PrivateStorageMaster.log, and keeps the last 24 sessions. Off, the log keeps the startup summary, errors and the size dump, and the last two sessions.");
            else
                Help("Writes every key press, open and close to PrivateStorageMaster.log. Off, the log keeps the startup summary, errors and the size dump.");
        }

        Section(TR("Store loot"));
        if (!api->getAutoStore)
            ImGui::TextDisabled("%s", TR("Storing loot needs a newer Private Storage Master."));
        else if (!s_psmAutoOk)
            ImGui::TextColored(kWarn, "%s", TR("Private Storage Master did not hand over its store loot settings."));
        else
        {
            const bool avail = s_psmAuto.available != 0;
            if (!avail) ImGui::TextColored(kWarn, "%s", TR("Not on this game version: Private Storage Master could not find the move it needs."));
            ImGui::BeginDisabled(!avail);
            bool on = s_psmAuto.enabled != 0;
            if (ImGui::Checkbox(TR("Put what Master Looter picks up into storage"), &on)) { s_psmAuto.enabled = on; PsmApplyAuto(api); }
            Help("A moment after an item lands in the bag, Private Storage Master moves it into the first storage below that takes it, the same move you would make at the storage. "
                 "Only what Master Looter picked up is moved, never what you picked up by hand, and from a body or a carcass only what your item rules allow. Nothing moves while a storage is open or outside free play, "
                 "keys, documents, quest items and anything on the never-move list stay put, and what no storage takes stays in the bag.");
            if (ImGui::Checkbox(TR("Show a notice when loot is stored"), &c.notifyAutoStore)) Settings::MarkDirty();
            Help("At most one line a second, naming each storage and how many went into it. Off, nothing is shown and it goes to the log only.");
            if (ImGui::Checkbox(TR("Store what your pet loots from bodies"), &c.petLootToStorage)) Settings::MarkDirty();
            Help("A pet or a companion that loots a body puts what it found in your bag. On, those items go to storage the same way as Master Looter's own pickups, as long as your item rules allow them. "
                 "Loose items a pet picks up are reported by the game as your own pickups, so those stay in the bag.");
            ImGui::BeginDisabled(!on);
            ImGui::TextDisabled("%s", TR("Each item goes to the first storage in this list that takes it."));
            for (const int i : kPsmAutoOrder)
            {
                ImGui::PushID(400 + i);
                bool st = s_psmAuto.storages[i] != 0;
                if (ImGui::Checkbox(api->storageName(i), &st)) { s_psmAuto.storages[i] = st; PsmApplyAuto(api); }
                ImGui::PopID();
            }
            bool only = s_psmAuto.onlyGained != 0;
            if (ImGui::Checkbox(TR("Only move what was picked up"), &only)) { s_psmAuto.onlyGained = only; PsmApplyAuto(api); }
            Help("On, only the amount that just arrived is moved, so food and potions you already carried stay in the bag. Off, the whole stack goes.");
            ImGui::EndDisabled();
            PsmNeverMoveList(api);
            ImGui::EndDisabled();
        }

        Section(TR("Keys"));
        ImGui::TextDisabled("%s", TR("A key or controller combo opens that storage from anywhere, and again closes it. Another storage's key switches straight to it."));
        bool dirty = false;
        for (int i = 0; i < PSM_STORAGES; ++i)
            dirty |= PsmKeyRow(api, api->storageName(i), kPsmHelp[i], s_psm.keys[i], kPsmKeyTarget + i, i, c);
        dirty |= PsmKeyRow(api, TR("Write the size dump to the log"), nullptr, s_psm.dumpKey, kPsmKeyTarget + PSM_STORAGES, PSM_STORAGES, c);
        if (s_psmBlockOk)
        {
            bool on = s_psmBlock.on != 0;
            if (ImGui::Checkbox(TR("Hold other keys back while Ctrl is down"), &on)) { s_psmBlock.on = on; PsmApplyBlock(api); }
            Help("While Ctrl, or any modifier your storage keys use, is held, other keys are kept from the game, so a slip off a storage key does not fire a skill. "
                 "Movement, Space, Tab, Enter, Escape, Alt+F4 and the keys the game uses with Ctrl (Z, Shift, +) still go through.");
            if (PsmKeyRow(api, TR("Turn that on and off"), "Turns the key block on and off while you play, and saves it.",
                          s_psmBlock.toggleKey, kPsmKeyTarget + kPsmBlockRow, kPsmBlockRow, c))
                PsmApplyBlock(api);
        }

        Section(TR("Controller"));
        ImGui::TextDisabled("%s", TR("Hold one or more buttons, then press the one that opens the storage. The pressed button is hidden from the game while the others are held."));
        for (int i = 0; i < PSM_STORAGES; ++i)
            dirty |= PsmPadRow(api, api->storageName(i), s_psm.pads[i], kPsmPadTarget + i, i, c);
        if (dirty) PsmApply(api);

        Section(TR("Sizes"));
        ImGui::TextColored(kGoldDim, "%s", TR("Size changes take effect the next time the game starts."));
        {
            bool alone = s_psm.leaveCapacityAlone != 0;
            if (ImGui::Checkbox(TR("Leave every size alone"), &alone)) { s_psm.leaveCapacityAlone = alone; PsmApply(api); }
            Help("For JSON capacity mods: the storage sizes below are ignored and the game, or that mod, decides.");
        }
        ImGui::BeginDisabled(s_psm.leaveCapacityAlone != 0);
        for (int i = 0; i < PSM_STORAGES; ++i)
        {
            PsmSize sz{};
            sz.size = sizeof sz;
            api->getSize(i, &sz);
            ImGui::PushID(300 + i);
            ImGui::TextUnformatted(api->storageName(i));
            if (i == 0) Help("The total, purchased expansions and story slots included. The left end keeps the game's size.");
            s_rowLabel.Next();
            if (const int fixed = api->fixedSlots ? api->fixedSlots(i) : 0)
            {
                ImGui::Text("%d", fixed);
                Help("This chest holds one of each collectible, so its slots cannot be changed.");
                ImGui::SameLine();
                if (sz.liveCapacity >= 0) ImGui::TextDisabled(TR("now %d of %d used"), sz.liveUsed, sz.liveCapacity);
                ImGui::PopID();
                continue;
            }
            // The left end of the slider is the game's own size, stored as 0.
            int floor = sz.known ? sz.gameDefault : 1;
            if (i == 0 && sz.known) floor += status.learnedExpansions;
            if (floor < 1) floor = 1;
            if (floor > PSM_MAX_SLOTS) floor = PSM_MAX_SLOTS;
            int value = s_psm.slots[i] > floor ? s_psm.slots[i] : floor;
            char fmt[64];
            if (value <= floor) snprintf(fmt, sizeof fmt, "%s", TR("game size (%d)"));
            else snprintf(fmt, sizeof fmt, "%s", "%d");
            ImGui::SetNextItemWidth(260 * g_scale);
            if (ImGui::SliderInt("##slots", &value, floor, PSM_MAX_SLOTS, fmt, ImGuiSliderFlags_AlwaysClamp))
                s_psm.slots[i] = value <= floor ? 0 : value;
            if (ImGui::IsItemDeactivatedAfterEdit()) PsmApply(api);
            ImGui::SameLine();
            if (sz.liveCapacity >= 0) ImGui::TextDisabled(TR("now %d of %d used"), sz.liveUsed, sz.liveCapacity);
            else ImGui::TextDisabled("%s", TR("load a save to see it"));
            ImGui::PopID();
        }
        {
            ImGui::TextUnformatted(TR("Private Storage expansions")); s_rowLabel.Next();
            int mode = s_psm.privateStorageExpansions < 0 ? 0 : 1;
            if (ImGui::RadioButton(TR("read from the save"), mode == 0) && mode != 0) { s_psm.privateStorageExpansions = -1; PsmApply(api); }
            ImGui::SameLine();
            if (ImGui::RadioButton(TR("set"), mode == 1) && mode != 1) { s_psm.privateStorageExpansions = status.learnedExpansions; PsmApply(api); }
            if (mode == 1)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120 * g_scale);
                int n = s_psm.privateStorageExpansions;
                if (ImGui::InputInt("##exp", &n, 10, 100)) s_psm.privateStorageExpansions = n < 0 ? 0 : (n > PSM_MAX_SLOTS ? PSM_MAX_SLOTS : n);
                if (ImGui::IsItemDeactivatedAfterEdit()) PsmApply(api);
            }
            else { ImGui::SameLine(); ImGui::TextDisabled(TR("%d last time"), status.learnedExpansions); }
            Help("How many Private Storage slots come from expansions you bought and from the story. The slider's total counts them, so the base size is the total less these.");
        }
        ImGui::EndDisabled();
        if (ImGui::Button(TR("Write the size dump to the log"))) api->writeDump();
        Help("Every storage's size and how many slots are in use, written to PrivateStorageMaster.log. Attach it to a size report.");

        Section(TR("Settings file"));
        if (ImGui::Button(TR("Reload from the ini"))) { api->reloadSettings(); PsmFetch(api); s_psmWhy[0] = 0; }
        ImGui::SameLine();
        if (ConfirmButton("psm-defaults", TR("Reset to defaults"), TR("Click again to reset"), true))
        {
            // Two saves, and a refusal from the first stays on screen: the
            // second one succeeding used to clear it.
            char refused[256] = "";
            PsmSettings d{};
            d.size = sizeof d;
            if (!api->getDefaults(&d)) snprintf(refused, sizeof refused, "%s", TR("Private Storage Master did not hand over its defaults"));
            else
            {
                s_psm = d;
                if (!PsmApply(api, "reset to defaults, settings")) snprintf(refused, sizeof refused, "%s", s_psmWhy);
            }
            if (api->getKeyBlock)
            {
                PsmKeyBlock b{};
                b.size = sizeof b;
                if (api->getKeyBlock(&b, 1))
                {
                    s_psmBlock = b;
                    if (!PsmApplyBlock(api, "reset to defaults, key block") && !refused[0]) snprintf(refused, sizeof refused, "%s", s_psmWhy);
                }
            }
            if (api->getAutoStore)
            {
                PsmAutoStore a{};
                a.size = sizeof a;
                if (api->getAutoStore(&a, 1))
                {
                    s_psmAuto = a;
                    if (!PsmApplyAuto(api, "reset to defaults, auto-store") && !refused[0]) snprintf(refused, sizeof refused, "%s", s_psmWhy);
                }
            }
            if (api->getNeverMove)
            {
                uint16_t d[PSM_NEVER_MOVE_MAX];
                const int n = api->getNeverMove(d, PSM_NEVER_MOVE_MAX, 1);
                if (n >= 0 && !PsmApplyNever(api, d, n, "reset to defaults") && !refused[0]) snprintf(refused, sizeof refused, "%s", s_psmWhy);
            }
            if (refused[0]) snprintf(s_psmWhy, sizeof s_psmWhy, "%s", refused);
        }
    }

    // The only thing drawn while the menu is closed: a notice that fades out.
    static void DrawNotice()
    {
        const State& st = State::Get();
        const DWORD now = GetTickCount();
        if (!st.notice[0] || static_cast<LONG>(st.noticeUntil - now) <= 0) return;
        const LONG left = static_cast<LONG>(st.noticeUntil - now);
        const float alpha = left < 600 ? left / 600.0f : 1.0f;
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        // A notice naming what the pet filter deleted, or both copies of the
        // plugin, runs longer than the screen is wide, so it wraps at 60% of
        // the width. Measured in the notice's own font, which is the larger one.
        const float wrap = disp.x * 0.6f;
        ImGui::PushFont(g_fontHead);
        const ImVec2 size = ImGui::CalcTextSize(st.notice, nullptr, false, wrap);
        ImGui::PopFont();
        ImGui::SetNextWindowPos(ImVec2((disp.x - size.x) * 0.5f - 18 * g_scale, disp.y * 0.12f));
        ImGui::SetNextWindowBgAlpha(0.70f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g_scale, 9 * g_scale));
        if (ImGui::Begin("##mlnotice", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::PushFont(g_fontHead);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
            ImGui::TextColored(kGold, "%s", st.notice);
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // The menu's cursor is virtual, and the moment ImGui is handed it decides
    // whether it is used at all. The Win32 backend polls GetCursorPos inside
    // its own NewFrame and queues a position every frame, and ImGui applies
    // queued positions in order, so the last one queued before the frame is
    // built is the one it keeps.
    //
    // This ran from Render until 13 September 2026, which is after
    // ImGui::NewFrame, so the virtual position sat in the queue until the
    // following frame and the backend's poll overwrote it every time. It read
    // as correct anyway, because the GetCursorPos detour makes that poll
    // return the virtual position as well. Where another mod owns
    // GetCursorPos the detour does not land, the poll comes back with the real
    // cursor, and the game clips the real cursor to a single pixel, so the
    // menu's pointer sits on that pixel and will not move. Sov's log of that
    // day: ImGui at 1118,684 with the virtual cursor at 1031,566 and the OS
    // cursor clipped to 1118,684..1118,684.
    void FeedInput()
    {
        State& st = State::Get();
        ImGuiIO& io = ImGui::GetIO();
        const bool capt = st.Captures();
        io.MouseDrawCursor = capt;
        static bool s_wasCapt = false;
        if (capt != s_wasCapt) { s_wasCapt = capt; if (capt) input::MenuOpened(); else input::MenuClosed(); }
        if (capt) input::FeedMouse(io);
        else { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddFocusEvent(capt); } // nothing hovers or reacts while watching
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
            ImGui::TextColored(kTextDim, "  v%s", ML_VERSION_FULL);
            const float closeW = ImGui::CalcTextSize("Close").x + ImGui::CalcTextSize("Watch").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeW);
            if (ImGui::SmallButton(TR("Watch"))) st.menuWatch = true;
            // KeyName returns one shared buffer for most keys, so two calls in
            // one argument list print the same name twice. Copy the first.
            const std::string menuKeyName = Settings::KeyName(c.menuKey);
            if (ImGui::BeginItemTooltip())
            {
                if (c.keyWatch) ImGui::Text(TR("Keep the menu on screen while you play. %s brings it back, %s closes it."), menuKeyName.c_str(), Settings::KeyName(c.keyWatch));
                else            ImGui::Text(TR("Keep the menu on screen while you play. %s brings it back."), menuKeyName.c_str());
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(TR("Close"))) open = false;
            if (st.menuWatch)
            {
                if (c.keyWatch) ImGui::TextColored(kGold, TR("Watch mode: the game has your controls. %s to interact, %s to close."), menuKeyName.c_str(), Settings::KeyName(c.keyWatch));
                else            ImGui::TextColored(kGold, TR("Watch mode: the game has your controls. %s to interact."), menuKeyName.c_str());
            }
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
                // Right of Status, and only when Private Storage Master is installed.
                if (psm::Installed())
                {
                    char label[96];
                    snprintf(label, sizeof label, "%s###Storage", TR("Storage"));
                    ImGui::PushFont(g_fontHead);
                    const bool sel = ImGui::BeginTabItem(label);
                    ImGui::PopFont();
                    if (sel) { ImGui::Dummy(ImVec2(0, 4 * g_scale)); TabStorage(c); ImGui::EndTabItem(); }
                }
                // Last, and only when a mod that raises stack limits is loaded.
                if (stack::Installed())
                {
                    char label[96];
                    snprintf(label, sizeof label, "%s###Stacks", TR("Stacks"));
                    ImGui::PushFont(g_fontHead);
                    const bool sel = ImGui::BeginTabItem(label);
                    ImGui::PopFont();
                    if (sel) { ImGui::Dummy(ImVec2(0, 4 * g_scale)); TabStacks(c); ImGui::EndTabItem(); }
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
