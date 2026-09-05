#include "menu.h"

#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/itemdb.h"
#include "../core/log.h"
#include "../core/rules.h"
#include "../core/settings.h"
#include "../core/state.h"
#include "../version.h"

namespace ml::gui
{
    static float g_scale = 1.0f;

    static ImVec4 Accent(float a = 1.0f) { return ImVec4(0.66f, 0.14f, 0.17f, a); }

    void InitStyle(float scale)
    {
        g_scale = std::clamp(scale, 0.8f, 3.0f);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        // A real Windows font at the right size instead of a scaled bitmap font.
        ImFontConfig cfg;
        cfg.SizePixels = 16.0f * g_scale;
        char path[MAX_PATH] = {};
        GetWindowsDirectoryA(path, MAX_PATH);
        std::string segoe = std::string(path) + "\\Fonts\\segoeui.ttf";
        if (!io.Fonts->AddFontFromFileTTF(segoe.c_str(), cfg.SizePixels))
            io.Fonts->AddFontDefault(&cfg);

        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 4.0f;
        s.FrameRounding  = 3.0f;
        s.GrabRounding   = 3.0f;
        s.TabRounding    = 3.0f;
        s.WindowPadding  = ImVec2(12, 10);
        s.FramePadding   = ImVec2(8, 4);
        s.ItemSpacing    = ImVec2(8, 6);
        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.09f, 0.10f, 0.96f);
        c[ImGuiCol_TitleBg]           = ImVec4(0.11f, 0.12f, 0.13f, 1.0f);
        c[ImGuiCol_TitleBgActive]     = ImVec4(0.16f, 0.17f, 0.19f, 1.0f);
        c[ImGuiCol_FrameBg]           = ImVec4(0.15f, 0.16f, 0.18f, 1.0f);
        c[ImGuiCol_FrameBgHovered]    = ImVec4(0.21f, 0.22f, 0.25f, 1.0f);
        c[ImGuiCol_FrameBgActive]     = ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
        c[ImGuiCol_CheckMark]         = ImVec4(0.90f, 0.35f, 0.38f, 1.0f);
        c[ImGuiCol_SliderGrab]        = Accent();
        c[ImGuiCol_SliderGrabActive]  = ImVec4(0.85f, 0.27f, 0.31f, 1.0f);
        c[ImGuiCol_Button]            = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
        c[ImGuiCol_ButtonHovered]     = Accent(0.8f);
        c[ImGuiCol_ButtonActive]      = Accent();
        c[ImGuiCol_Header]            = Accent(0.35f);
        c[ImGuiCol_HeaderHovered]     = Accent(0.6f);
        c[ImGuiCol_HeaderActive]      = Accent(0.8f);
        c[ImGuiCol_Tab]               = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
        c[ImGuiCol_TabHovered]        = Accent(0.7f);
        c[ImGuiCol_TabSelected]       = Accent();
        c[ImGuiCol_TabDimmedSelected] = Accent(0.6f);
        c[ImGuiCol_TableHeaderBg]     = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
        c[ImGuiCol_NavCursor]         = ImVec4(0.90f, 0.35f, 0.38f, 0.8f);
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

        const bool key = KeyDown(c.menuKey);
        if (key && !s_keyWas && !st.rebindCapture)
        {
            st.menuOpen = !st.menuOpen;
            if (!st.menuOpen) st.textCapture = false;
        }
        s_keyWas = key;

        const bool esc = KeyDown(VK_ESCAPE);
        if (esc && !s_escWas && st.menuOpen && !st.textCapture && !st.rebindCapture)
            st.menuOpen = false;
        s_escWas = esc;
    }

    bool WantsDraw()
    {
        return State::Get().menuOpen || Settings::Get().showHud;
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

    static void Help(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    static bool TriState(const char* id, int& value)
    {
        // value: 0 default, 1 always, -1 never
        bool changed = false;
        ImGui::PushID(id);
        if (ImGui::RadioButton("default", value == 0)) { value = 0; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("always", value == 1)) { value = 1; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("never", value == -1)) { value = -1; changed = true; }
        ImGui::PopID();
        return changed;
    }

    // --- tabs ---------------------------------------------------------------
    static void TabGeneral(Config& c)
    {
        State& st = State::Get();
        bool dirty = false;

        dirty |= ImGui::Checkbox("Master Looter enabled", &c.enabled);
        Help("Master switch for automatic looting. The menu and HUD keep working when this is off.");
        dirty |= ImGui::Checkbox("Show HUD line when the menu is closed", &c.showHud);

        ImGui::SeparatorText("Menu key");
        ImGui::Text("Open and close: %s", Settings::KeyName(c.menuKey));
        ImGui::SameLine();
        if (!st.rebindCapture)
        {
            if (ImGui::Button("Rebind")) st.rebindCapture = true;
        }
        else
        {
            ImGui::TextColored(Accent(), "press the new key (Escape cancels)");
            for (int vk = 0x08; vk < 0xFF; ++vk)
            {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
                if (!KeyDown(vk)) continue;
                if (vk != VK_ESCAPE) { c.menuKey = vk; dirty = true; }
                st.rebindCapture = false;
                break;
            }
        }

        ImGui::SeparatorText("Looting");
        dirty |= ImGui::SliderFloat("Range (m)", &c.lootRange, 1.0f, 50.0f, "%.0f");
        Help("How far from the player the mod looks for loot.");
        dirty |= ImGui::SliderInt("Max loots per second", &c.maxLootsPerSec, 1, 30);
        dirty |= ImGui::Checkbox("Loot corpses", &c.lootCorpses);
        dirty |= ImGui::Checkbox("Pick up items on the ground", &c.pickUpItems);
        dirty |= ImGui::Checkbox("Gather plants", &c.gatherPlants);
        dirty |= ImGui::Checkbox("Catch insects and small animals", &c.catchInsects);
        dirty |= ImGui::Checkbox("Open containers", &c.lootContainers);

        ImGui::SeparatorText("Filters");
        dirty |= ImGui::Checkbox("Skip quest items", &c.skipQuestItems);
        Help("Items tagged quest are left alone so puzzles and story pickups are never auto-taken.");
        dirty |= ImGui::Checkbox("Skip items shops refuse to buy", &c.skipNoSell);
        dirty |= ImGui::SliderInt("Minimum value (copper)", &c.minValueCopper, 0, 500);
        Help("0 turns the floor off. Items with an unknown value are never filtered by it.");

        if (dirty) Settings::MarkDirty();
    }

    static void TabClasses(Config& c)
    {
        static char filter[64] = "";
        ImGui::SetNextItemWidth(260 * g_scale);
        ImGui::InputTextWithHint("##classfilter", "filter classes", filter, sizeof filter);
        ImGui::SameLine();
        if (ImGui::Button("Loot all")) { for (const auto& kv : ItemDb::Classes()) c.classRule[kv.first] = 1; Settings::MarkDirty(); }
        ImGui::SameLine();
        if (ImGui::Button("Skip all")) { for (const auto& kv : ItemDb::Classes()) c.classRule[kv.first] = 0; Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled("%d classes", static_cast<int>(ItemDb::Classes().size()));

        const std::string f = Lower(filter);
        if (ImGui::BeginTable("classes", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH, ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Loot", ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
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
                if (kv.first == "dev") ImGui::TextDisabled("%s (never looted)", kv.first.c_str());
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
        ImGui::InputTextWithHint("##tagfilter", "filter tags", filter, sizeof filter);
        ImGui::SameLine();
        if (ImGui::Button("Reset all tags")) { c.tagRule.clear(); Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled("always beats the class rule; never beats always");

        const std::string f = Lower(filter);
        if (ImGui::BeginTable("tags", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
            ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthFixed, 260 * g_scale);
            ImGui::TableHeadersRow();
            for (const auto& kv : ItemDb::Tags())
            {
                if (!Contains(kv.first, f)) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                auto it = c.tagRule.find(kv.first);
                const int cur = it == c.tagRule.end() ? 0 : it->second;
                if (cur) ImGui::TextColored(Accent(), "%s", kv.first.c_str());
                else ImGui::TextUnformatted(kv.first.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", kv.second);
                ImGui::TableSetColumnIndex(2);
                int v = cur;
                if (TriState(kv.first.c_str(), v))
                {
                    if (v == 0) c.tagRule.erase(kv.first); else c.tagRule[kv.first] = v;
                    Settings::MarkDirty();
                }
            }
            ImGui::EndTable();
        }
    }

    static void TabItems(Config& c)
    {
        static char query[96] = "";
        ImGui::SetNextItemWidth(360 * g_scale);
        ImGui::InputTextWithHint("##itemsearch", "search item name or key (3+ letters)", query, sizeof query);
        ImGui::SameLine();
        if (ImGui::Button("Clear overrides")) { c.itemRule.clear(); Settings::MarkDirty(); }
        ImGui::SameLine();
        ImGui::TextDisabled("%d overrides", static_cast<int>(c.itemRule.size()));

        const std::string q = Lower(query);
        std::vector<const Item*> rows;
        if (q.size() >= 3)
        {
            for (const Item& it : ItemDb::All())
            {
                if (Contains(it.name, q) || Contains(it.stringKey, q) || it.klass == q)
                {
                    rows.push_back(&it);
                    if (rows.size() >= 250) break;
                }
            }
        }
        else
        {
            for (const auto& kv : c.itemRule)
                if (const Item* it = ItemDb::Find(kv.first)) rows.push_back(it);
        }

        if (q.size() < 3) ImGui::TextDisabled("Showing current overrides. Type to search all %d items.", ItemDb::Count());
        else ImGui::TextDisabled("%d match%s%s", static_cast<int>(rows.size()), rows.size() == 1 ? "" : "es", rows.size() >= 250 ? " (first 250)" : "");

        if (ImGui::BeginTable("items", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 120 * g_scale);
            ImGui::TableSetupColumn("Copper", ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
            ImGui::TableSetupColumn("Verdict", ImGuiTableColumnFlags_WidthFixed, 170 * g_scale);
            ImGui::TableSetupColumn("Override", ImGuiTableColumnFlags_WidthFixed, 260 * g_scale);
            ImGui::TableHeadersRow();
            for (const Item* it : rows)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(it->key));
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(it->name.empty() ? it->stringKey.c_str() : it->name.c_str());
                if (!it->name.empty() && ImGui::BeginItemTooltip()) { ImGui::Text("%s (key %u, tier %d)", it->stringKey.c_str(), it->key, it->tier); ImGui::TextWrapped("%s", it->tags.c_str()); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(it->klass.c_str());
                ImGui::TableSetColumnIndex(2);
                if (it->value >= 0) ImGui::Text("%lld", it->value); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3);
                const Rules::Verdict v = Rules::Decide(*it, c);
                if (v.loot) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "loot: %s", v.rule);
                else ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.45f, 1), "skip: %s", v.rule);
                if (!v.detail.empty() && ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(v.detail.c_str()); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(4);
                auto ov = c.itemRule.find(it->key);
                int cur = ov == c.itemRule.end() ? 0 : ov->second;
                if (TriState("ov", cur))
                {
                    if (cur == 0) c.itemRule.erase(it->key); else c.itemRule[it->key] = cur;
                    Settings::MarkDirty();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    static void TabStatus()
    {
        const State& st = State::Get();
        ImGui::Text("Master Looter v%s for game build %s", ML_VERSION, ML_GAME_BUILD);
        ImGui::Text("DX12 hooks: %s", st.hooksOk ? "installed" : "failed");
        ImGui::Text("Overlay: %s", st.overlayReady ? "ready" : "waiting for first frame");
        ImGui::Text("Item database: %s", ItemDb::Loaded() ? "loaded" : "missing (MasterLooter.items.tsv)");
        if (ItemDb::Loaded()) ImGui::Text("  %d items, %d classes, %d tags", ItemDb::Count(), static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size()));
        ImGui::Text("Settings: %ls", Settings::Path().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload now")) Settings::Load();
        ImGui::SameLine();
        if (ImGui::SmallButton("Save now")) Settings::Save();
        ImGui::TextDisabled("Loot engine: not wired in this build. Rules are evaluated live in the Items tab.");

        ImGui::SeparatorText("Log");
        static std::vector<std::string> lines;
        Log::Snapshot(lines, 60);
        if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders))
        {
            for (const auto& l : lines) ImGui::TextUnformatted(l.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    static void DrawHud(const Config& c)
    {
        ImGui::SetNextWindowPos(ImVec2(12 * g_scale, 12 * g_scale));
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##mlhud", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
        {
            ImGui::TextColored(Accent(), "Master Looter");
            ImGui::SameLine();
            ImGui::Text("%s  |  %s opens the menu", c.enabled ? "on" : "off", Settings::KeyName(c.menuKey));
        }
        ImGui::End();
    }

    void Render()
    {
        State& st = State::Get();
        Config& c = Settings::Get();

        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = st.menuOpen;

        if (c.showHud && !st.menuOpen) DrawHud(c);
        if (!st.menuOpen) { st.textCapture = false; return; }

        ImGui::SetNextWindowSize(ImVec2(820 * g_scale, 600 * g_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80 * g_scale, 80 * g_scale), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Master Looter", &open, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("tabs"))
            {
                if (ImGui::BeginTabItem("General")) { TabGeneral(c); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Classes")) { TabClasses(c); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Tags"))    { TabTags(c);    ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Items"))   { TabItems(c);   ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Status"))  { TabStatus();   ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (!open) st.menuOpen = false;
        st.textCapture = io.WantTextInput;
    }
}
