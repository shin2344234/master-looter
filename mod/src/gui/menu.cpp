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
#include "../hooks/input.h"
#include "../loot/engine.h"
#include "../loot/events.h"
#include "../loot/game.h"
#include "../loot/mem.h"
#include "../version.h"

namespace ml::gui
{
    static float g_scale = 1.0f;
    static int   g_rebindTarget = -1; // 0 menu key, 1 toggle key, 2 burst key

    static ImVec4 Accent(float a = 1.0f) { return ImVec4(0.66f, 0.14f, 0.17f, a); }
    static const ImVec4 kGood(0.55f, 0.85f, 0.55f, 1);
    static const ImVec4 kWarn(0.9f, 0.6f, 0.45f, 1);
    static const ImVec4 kMuted(0.6f, 0.6f, 0.62f, 1);

    void InitStyle(float scale)
    {
        g_scale = std::clamp(scale, 0.8f, 3.0f);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        ImFontConfig cfg;
        cfg.SizePixels = 16.0f * g_scale;
        char path[MAX_PATH] = {};
        GetWindowsDirectoryA(path, MAX_PATH);
        std::string segoe = std::string(path) + "\\Fonts\\segoeui.ttf";
        if (!io.Fonts->AddFontFromFileTTF(segoe.c_str(), cfg.SizePixels))
            io.Fonts->AddFontDefault(&cfg);

        ImGui::StyleColorsDark();
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 4.0f; s.FrameRounding = 3.0f; s.GrabRounding = 3.0f; s.TabRounding = 3.0f;
        s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(8, 4); s.ItemSpacing = ImVec2(8, 6);
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
        const State& st = State::Get();
        if (st.menuOpen) return true;
        return Settings::Get().showHud && st.notice[0] && static_cast<LONG>(st.noticeUntil - GetTickCount()) > 0;
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
    static void OnOff(const char* label, bool on, const char* onText = "yes", const char* offText = "no")
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(220 * g_scale);
        ImGui::TextColored(on ? kGood : kWarn, "%s", on ? onText : offText);
    }

    // One row of the key table: name, current key, rebind button, capture state.
    static bool KeyRow(const char* label, int& vk, int target)
    {
        State& st = State::Get();
        bool dirty = false;
        ImGui::PushID(target);
        ImGui::TextUnformatted(label);
        ImGui::SameLine(260 * g_scale);
        ImGui::Text("%s", Settings::KeyName(vk));
        ImGui::SameLine(400 * g_scale);
        if (st.rebindCapture && g_rebindTarget == target)
        {
            ImGui::TextColored(Accent(), "press a key (Escape cancels)");
            for (int k = 0x08; k < 0xFF; ++k)
            {
                if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue;
                if (!KeyDown(k)) continue;
                if (k != VK_ESCAPE) { vk = k; dirty = true; }
                st.rebindCapture = false; g_rebindTarget = -1;
                break;
            }
        }
        else if (!st.rebindCapture && ImGui::SmallButton("Rebind")) { st.rebindCapture = true; g_rebindTarget = target; }
        ImGui::PopID();
        return dirty;
    }

    // --- tabs ---------------------------------------------------------------
    static void TabGeneral(Config& c)
    {
        bool dirty = false;
        if (ImGui::Checkbox("Auto-loot enabled", &c.enabled)) { dirty = true; State::Get().Notify(c.enabled ? "Master Looter: auto-loot on" : "Master Looter: auto-loot off"); }
        Help("The engine scans around you and takes what the rules allow. Off means nothing is taken automatically; the burst key still works.");
        ImGui::SameLine(300 * g_scale);
        if (ImGui::Button("Loot everything in range now")) loot::RequestBurst();
        dirty |= ImGui::Checkbox("Show a brief notice when auto-loot is toggled", &c.showHud);
        Help("Nothing else is drawn while the menu is closed.");

        ImGui::SeparatorText("Keys");
        dirty |= KeyRow("Open and close this menu", c.menuKey, 0);
        dirty |= KeyRow("Auto-loot on / off", c.keyToggle, 1);
        dirty |= KeyRow("Loot everything in range once", c.keyBurst, 2);

        ImGui::SeparatorText("Pace");
        dirty |= ImGui::SliderInt("Scans per second", &c.scansPerSec, 1, 30);
        Help("How often the scene is read. The scan runs on its own thread; only the final take costs the game a fraction of a millisecond.");
        dirty |= ImGui::SliderInt("Objects per scan", &c.perScan, 0, 64, c.perScan ? "%d" : "no limit");
        dirty |= ImGui::SliderInt("Objects per burst press", &c.burstPerKey, 0, 64, c.burstPerKey ? "%d" : "everything in range");
        dirty |= ImGui::SliderInt("Retry the same object after (ms)", &c.retryAfterMs, 500, 30000);
        Help("The game removes taken objects with a delay. This stops the same object being sent twice while it fades.");

        ImGui::SeparatorText("Filters");
        dirty |= ImGui::Checkbox("Skip quest items", &c.skipQuestItems);
        Help("Items tagged quest are left alone so puzzles and story pickups are never auto-taken.");
        dirty |= ImGui::Checkbox("Skip items shops refuse to buy", &c.skipNoSell);
        dirty |= ImGui::SliderInt("Minimum value (copper)", &c.minValueCopper, 0, 500, c.minValueCopper ? "%d" : "off");
        Help("Items with an unknown value are never filtered by it.");
        dirty |= ImGui::Checkbox("Take items the database cannot name", &c.takeUnknownItems);
        Help("Some world objects carry no readable item name. On: take them anyway. Off: leave anything unidentified.");
        dirty |= ImGui::Checkbox("Verbose log", &c.debugLog);
        if (dirty) Settings::MarkDirty();
    }

    static void TabLooting(Config& c)
    {
        bool dirty = false;
        ImGui::SeparatorText("What to collect");
        dirty |= ImGui::Checkbox("Search animal carcasses", &c.lootCorpses);
        Help("The skinning interaction, once per carcass. Human corpses drop ordinary loot on the ground instead; that is handled by pick up.");
        dirty |= ImGui::Checkbox("Pick up items on the ground", &c.pickUpItems);
        dirty |= ImGui::Checkbox("Gather plants, ore and stone", &c.gatherPlants);
        dirty |= ImGui::Checkbox("Catch insects, fish and small animals", &c.catchCreatures);
        dirty |= ImGui::Checkbox("Try containers and furniture nodes", &c.lootContainers);
        Help("Chests and crates rarely respond to the loot event, and furniture is mostly clutter. Off by default.");

        ImGui::SeparatorText("Ranges (metres)");
        dirty |= ImGui::SliderFloat("Scan radius", &c.scanRange, 5.0f, 200.0f, "%.0f");
        Help("What enters the working list at all. The ranges below are clamped to it.");
        dirty |= ImGui::SliderFloat("Items on the ground", &c.lootRange, 0.0f, 100.0f, c.lootRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Gathering", &c.gatherRange, 0.0f, 100.0f, c.gatherRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Catching", &c.catchRange, 0.0f, 100.0f, c.catchRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Carcasses", &c.corpseRange, 0.0f, 100.0f, c.corpseRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Dead zone around you", &c.minRange, 0.0f, 2.0f, "%.2f");
        Help("Objects closer than this are treated as your own equipment. A safety net; your gear is also recognised by other means.");

        ImGui::SeparatorText("Reaching nodes");
        dirty |= ImGui::Checkbox("Arm nodes ourselves", &c.autoArm);
        Help("The game fills a node's data only when it thinks you can reach it; for an ore vein that means standing on it. Arming asks the game to do it from a distance.");
        dirty |= ImGui::SliderFloat("Arming range", &c.armRange, 0.0f, 60.0f, c.armRange > 0 ? "%.0f" : "same as gathering");
        Help("Arming ignores walls. Keep it short or you will gather through the wall of the next room.");
        dirty |= ImGui::Checkbox("Arm mechanism containers too", &c.armContainers);
        Help("A well bucket and the like are never looted, but arming one makes the game offer its interaction so you can use it by hand.");

        ImGui::SeparatorText("Ownership");
        dirty |= ImGui::Checkbox("Take goods that belong to someone", &c.lootOwned);
        if (c.lootOwned) ImGui::TextColored(kWarn, "The game treats this as stealing and will put a bounty on you.");
        else ImGui::TextDisabled("Uses the game's own Take or Steal check; owned goods are skipped until it has been observed once.");
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
        if (ImGui::BeginTable("classes", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
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
                if (Contains(it.name, q) || Contains(it.stringKey, q) || it.klass == q) { rows.push_back(&it); if (rows.size() >= 250) break; }
        }
        else
        {
            for (const auto& kv : c.itemRule) if (const Item* it = ItemDb::Find(kv.first)) rows.push_back(it);
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
                ImGui::TextUnformatted(it->Label());
                if (ImGui::BeginItemTooltip()) { ImGui::Text("%s (key %u, row %d, tier %d)", it->stringKey.c_str(), it->key, it->row, it->tier); ImGui::TextWrapped("%s", it->tags.c_str()); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(it->klass.c_str());
                ImGui::TableSetColumnIndex(2);
                if (it->value >= 0) ImGui::Text("%lld", it->value); else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3);
                const Rules::Verdict v = Rules::Decide(*it, c);
                if (v.loot) ImGui::TextColored(kGood, "loot: %s", v.rule); else ImGui::TextColored(kWarn, "skip: %s", v.rule);
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
        if (!s.started) { ImGui::TextColored(kWarn, "Loot engine not started."); return; }
        if (!s.resolved) { ImGui::TextColored(kWarn, "Game functions did not resolve; see Status."); return; }
        if (!s.actorManager) { ImGui::TextDisabled("Waiting for the world to load."); return; }
        if (!s.playerFound) { ImGui::TextDisabled("Player not found in the scene yet."); return; }
        ImGui::Text("%d objects within scan range, %d lootable now.", s.candidates, s.lootable);
        ImGui::SameLine();
        if (s.settling) ImGui::TextColored(kWarn, "paused: %s", s.hold);
        else ImGui::TextDisabled("scan %.1f ms", s.lastScanMs);
        ImGui::TextDisabled("Objects are listed nearest first, with the rule that decided each one. Empty nodes read as not ready until the game or arming fills them.");

        static loot::Nearby rows[48];
        const int n = loot::CopyNearby(rows, 48);
        if (ImGui::BeginTable("nearby", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, 50 * g_scale);
            ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
            ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthFixed, 300 * g_scale);
            ImGui::TableHeadersRow();
            for (int i = 0; i < n; ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%.1f", rows[i].dist);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(rows[i].name);
                if (ImGui::BeginItemTooltip()) { ImGui::Text("entity %08X", rows[i].eid); ImGui::EndTooltip(); }
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", rows[i].klass);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(rows[i].loot ? kGood : kMuted, "%s", rows[i].verdict);
            }
            ImGui::EndTable();
        }
    }

    static void TabStatus()
    {
        const State& st = State::Get();
        const loot::Status s = loot::GetStatus();
        ImGui::Text("Master Looter v%s for game build %s", ML_VERSION, ML_GAME_BUILD);
        ImGui::Text("Settings: %ls", Settings::Path().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload now")) Settings::Load();
        ImGui::SameLine();
        if (ImGui::SmallButton("Save now")) Settings::Save();

        ImGui::SeparatorText("Overlay");
        OnOff("DirectX 12 hooks", st.hooksOk, "installed", "failed");
        OnOff("Item database", ItemDb::Loaded(), "loaded", "missing MasterLooter.items.tsv");
        if (ItemDb::Loaded()) { ImGui::SameLine(); ImGui::TextDisabled("%d items, %d classes, %d tags", ItemDb::Count(), static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size())); }

        ImGui::SeparatorText("Loot engine");
        OnOff("Engine", s.started, s.note, "not started");
        OnOff("Game functions", s.resolved, "resolved", "missing");
        OnOff("Game-thread pump", s.hooked, s.pump, "none");
        OnOff("World (actor manager)", s.actorManager, "found", "waiting");
        if (s.playerFound) { ImGui::TextUnformatted("Player"); ImGui::SameLine(220 * g_scale); ImGui::TextColored(kGood, "entity %08X, %d items in bag", s.playerEid, s.inventoryItems); }
        else OnOff("Player", false, "", "not found yet");
        OnOff("Event descriptors", s.descriptors == 3, "3 of 3", s.descriptors ? "incomplete" : "not resolved yet");
        OnOff("Sending events", s.sendAllowed, "allowed", "not yet");
        OnOff("Route id", s.routeKnown, "learned from the game", "using the player's own field");
        OnOff("Ownership oracle", s.ownerOracle, "captured", "waiting for the game to check an item");
        const char* tbl = s.itemTable == 1 ? "rows verified against our database" : s.itemTable == 2 ? "names readable, rows differ" : s.itemTable == -1 ? "unavailable" : "not probed yet";
        OnOff("Item table", s.itemTable > 0, tbl, tbl);
        ImGui::Text("Scans %ld, events sent %ld, pump ticks %ld, guarded faults %ld", s.scans, s.sent, s.pumpTicks, s.faults);
        ImGui::Text("This session: %ld picked up, %ld gathered, %ld caught, %ld carcasses",
                    loot::SessionCount(static_cast<int>(events::Action::Take)), loot::SessionCount(static_cast<int>(events::Action::Gather)),
                    loot::SessionCount(static_cast<int>(events::Action::Catch)), loot::SessionCount(static_cast<int>(events::Action::Search)));

        if (ImGui::TreeNode("Signatures"))
        {
            for (int i = 0; i < game::SigCount(); ++i)
            {
                const game::SigResult& r = game::Sig(i);
                if (r.addr) ImGui::TextColored(kGood, "%-18s +0x%llX", r.name, static_cast<unsigned long long>(mem::Rva(r.addr)));
                else ImGui::TextColored(r.required ? kWarn : kMuted, "%-18s %s (%zu hits)%s", r.name, r.hits ? "ambiguous" : "missing", r.hits, r.required ? "  required" : "  optional");
            }
            ImGui::TreePop();
        }

        ImGui::SeparatorText("Recent loot");
        static loot::Recent recent[12];
        const int rn = loot::CopyRecent(recent, 12);
        if (!rn) ImGui::TextDisabled("nothing taken yet this session");
        for (int i = 0; i < rn; ++i) ImGui::TextUnformatted(recent[i].text);

        ImGui::SeparatorText("Log");
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
        ImGui::SetNextWindowBgAlpha(0.55f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        if (ImGui::Begin("##mlnotice", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
            ImGui::TextUnformatted(st.notice);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void Render()
    {
        State& st = State::Get();
        Config& c = Settings::Get();
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = st.menuOpen;
        st.renderTid = GetCurrentThreadId();
        static bool s_wasOpen = false;
        if (st.menuOpen != s_wasOpen) { s_wasOpen = st.menuOpen; if (st.menuOpen) input::MenuOpened(); else input::MenuClosed(); }
        if (st.menuOpen) input::FeedMouse(io);

        if (c.showHud) DrawNotice();
        if (!st.menuOpen) { st.textCapture = false; if (st.rebindCapture) { st.rebindCapture = false; g_rebindTarget = -1; } return; }

        ImGui::SetNextWindowSize(ImVec2(860 * g_scale, 620 * g_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80 * g_scale, 80 * g_scale), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Master Looter", &open, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("tabs"))
            {
                if (ImGui::BeginTabItem("General")) { TabGeneral(c); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Looting")) { TabLooting(c); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Classes")) { TabClasses(c); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Tags"))    { TabTags(c);    ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Items"))   { TabItems(c);   ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Nearby"))  { TabNearby();   ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Status"))  { TabStatus();   ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (!open) st.menuOpen = false;
        st.textCapture = io.WantTextInput;
    }
}
