#include "menu.h"

#include <Windows.h>
#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue for the group checkboxes
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

    void InitStyle(float scale)
    {
        g_scale = std::clamp(scale, 0.8f, 3.0f);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        char win[MAX_PATH] = {};
        GetWindowsDirectoryA(win, MAX_PATH);
        const std::string fonts = std::string(win) + "\\Fonts\\";
        ImFontConfig body; body.SizePixels = 16.0f * g_scale;
        g_fontBody = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), body.SizePixels);
        if (!g_fontBody) g_fontBody = io.Fonts->AddFontDefault(&body);
        ImFontConfig head; head.SizePixels = 20.0f * g_scale;
        const char* serifs[] = { "georgia.ttf", "constan.ttf", "cambria.ttc", "times.ttf" };
        for (const char* f : serifs)
        {
            g_fontHead = io.Fonts->AddFontFromFileTTF((fonts + f).c_str(), head.SizePixels);
            if (g_fontHead) break;
        }
        if (!g_fontHead) g_fontHead = g_fontBody;

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
        const bool key = KeyDown(c.menuKey);
        if (key && !s_keyWas && !st.rebindCapture)
        {
            // Insert: closed -> interactive; watching -> interactive; interactive -> closed.
            if (!st.menuOpen) { st.menuOpen = true; st.menuWatch = false; }
            else if (st.menuWatch) st.menuWatch = false;
            else { st.menuOpen = false; st.textCapture = false; }
        }
        s_keyWas = key;

        const bool watch = KeyDown(c.keyWatch);
        if (watch && !s_watchWas && !st.rebindCapture && !st.textCapture)
        {
            // Home: closed -> watching; interactive -> watching; watching -> closed.
            if (!st.menuOpen) { st.menuOpen = true; st.menuWatch = true; }
            else if (!st.menuWatch) st.menuWatch = true;
            else { st.menuOpen = false; st.menuWatch = false; }
        }
        s_watchWas = watch;

        const bool esc = KeyDown(VK_ESCAPE);
        if (esc && !s_escWas && st.Captures() && !st.textCapture && !st.rebindCapture)
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

        Section("Keys");
        dirty |= KeyRow("Open and close this menu", c.menuKey, 0);
        dirty |= KeyRow("Auto-loot on / off", c.keyToggle, 1);
        dirty |= KeyRow("Loot everything in range once", c.keyBurst, 2);
        dirty |= KeyRow("Watch mode (menu stays up, you keep playing)", c.keyWatch, 3);

        Section("Pace");
        dirty |= ImGui::SliderInt("Scans per second", &c.scansPerSec, 1, 30);
        Help("How often the scene is read. The scan runs on its own thread; only the final take costs the game a fraction of a millisecond.");
        dirty |= ImGui::SliderInt("Objects per scan", &c.perScan, 0, 64, c.perScan ? "%d" : "no limit");
        dirty |= ImGui::SliderInt("Objects per burst press", &c.burstPerKey, 0, 64, c.burstPerKey ? "%d" : "everything in range");
        dirty |= ImGui::SliderInt("Retry the same object after (ms)", &c.retryAfterMs, 500, 30000);
        Help("The game removes taken objects with a delay. This stops the same object being sent twice while it fades.");

        Section("Filters");
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
        Section("What to collect");
        struct Toggle { const char* label; bool* value; const char* help; };
        const Toggle toggles[] = {
            { "Ground items",   &c.pickUpItems,    "Items lying in the world, including drops from enemies." },
            { "Carcasses",      &c.lootCorpses,    "The skinning interaction, once per carcass. Human corpses drop ordinary loot instead." },
            { "Plants",         &c.gatherPlants,   "Herbs, mushrooms, crops and other materials a node yields." },
            { "Ore",            &c.gatherOre,      "Nodes that yield ore." },
            { "Stone",          &c.gatherStone,    "Nodes that yield stone." },
            { "Wood",           &c.gatherWood,     "Nodes that yield branches and stalks." },
            { "Unidentified nodes", &c.gatherUnknown, "Nodes whose yield is not known yet. Off (the default) leaves them alone; gather one by hand and its kind is learned from what lands in your bag. On makes the mod gather them to find out, which means a plant can be taken while Plants is off." },
            { "Insects",        &c.catchInsects,   "Butterflies, beetles and the rest. Species come from the creature table; a creature the table cannot name is only caught when every category it could belong to is on." },
            { "Fish",           &c.catchFish,      "Fish in reach of the catch interaction." },
            { "Small animals",  &c.catchAnimals,   "Chickens, coots, rats, frogs: anything else the game puts in the bag whole." },
            { "Containers",     &c.lootContainers, "Chests, crates and drop-set nodes. They rarely respond to the loot event. Off by default." },
            { "Furniture nodes", &c.lootFurniture, "Furniture with an interaction node. Mostly clutter. Off by default." },
        };
        if (ImGui::BeginTable("collect", 3, ImGuiTableFlags_SizingStretchSame))
        {
            for (const Toggle& t : toggles)
            {
                ImGui::TableNextColumn();
                dirty |= ImGui::Checkbox(t.label, t.value);
                if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f); ImGui::TextUnformatted(t.help); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            }
            ImGui::EndTable();
        }
        {
            const loot::Status s = loot::GetStatus();
            ImGui::TextDisabled("%d node kinds learned so far. A node is identified the first time it yields something; the list is kept in MasterLooter.learned.tsv.", s.learned);
            ImGui::SameLine();
            if (ImGui::SmallButton("Forget")) loot::ForgetLearned();
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted("Clears the learned list so every node type is identified again from scratch."); ImGui::EndTooltip(); }
        }

        Section("Ranges (metres)");
        dirty |= ImGui::SliderFloat("Scan radius", &c.scanRange, 5.0f, 200.0f, "%.0f");
        Help("What enters the working list at all. The ranges below are clamped to it.");
        dirty |= ImGui::SliderFloat("Items on the ground", &c.lootRange, 0.0f, 100.0f, c.lootRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Gathering", &c.gatherRange, 0.0f, 100.0f, c.gatherRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Catching", &c.catchRange, 0.0f, 100.0f, c.catchRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Carcasses", &c.corpseRange, 0.0f, 100.0f, c.corpseRange > 0 ? "%.0f" : "no limit");
        dirty |= ImGui::SliderFloat("Dead zone around you", &c.minRange, 0.0f, 2.0f, "%.2f");
        Help("Objects closer than this are treated as your own equipment. A safety net; your gear is also recognised by other means.");

        Section("Reaching nodes");
        dirty |= ImGui::Checkbox("Arm nodes ourselves", &c.autoArm);
        Help("The game fills a node's data only when it thinks you can reach it; for an ore vein that means standing on it. Arming asks the game to do it from a distance.");
        dirty |= ImGui::SliderFloat("Arming range", &c.armRange, 0.0f, 60.0f, c.armRange > 0 ? "%.0f" : "same as gathering");
        Help("Arming ignores walls. Keep it short or you will gather through the wall of the next room.");
        dirty |= ImGui::Checkbox("Arm mechanism containers too", &c.armContainers);
        Help("A well bucket and the like are never looted, but arming one makes the game offer its interaction so you can use it by hand.");

        Section("Ownership");
        dirty |= ImGui::Checkbox("Take goods that belong to someone", &c.lootOwned);
        if (c.lootOwned) ImGui::TextColored(kWarn, "The game treats this as stealing and will put a bounty on you.");
        else ImGui::TextDisabled("Uses the game's own Take or Steal check; owned goods are skipped until it has been observed once.");
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
        { "Materials",           "catalyst crafting-material alchemy-material herb wood ingredient seed trade-good goods junk bait", "Ore, herbs, wood, trade goods and other crafting input." },
        { "Creatures",           "insect fish animal amphibian", "Caught creatures, live or lying around; a creature the table can name follows its class rule." },
        { "Ammunition",          "arrow ammo ammo-bundle bullet magic-bullet cannonball explosive", "" },
        { "Books and papers",    "book document note poster skill-poster bounty-notice treasure-map legendary-animal-report recipe recipe-book", "" },
        { "Furniture and decor", "furniture household lamp light ornament painting flower-pot decoration cooking-facility storage chest container", "Household clutter, most of it worthless." },
        { "Mounts and vehicles", "mount-gear mount-feed mount-utility pet-gear vehicle-part", "" },
        { "Treasure and keepsakes", "treasure sealed-artifact artifact keepsake currency", "" },
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
        Section("Groups");
        ImGui::TextDisabled("One click per family. The table below still sets single classes; a group shows a dash when only some of it is on.");
        if (ImGui::BeginTable("groups", 3, ImGuiTableFlags_SizingStretchSame))
        {
            for (const ClassGroup& g : kGroups)
            {
                ImGui::TableNextColumn();
                const int st = GroupState(c, g);
                bool v = st == 1;
                if (st == 2) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                if (ImGui::Checkbox(g.name, &v)) { SetGroup(c, g, st != 1); Settings::MarkDirty(); }
                if (st == 2) ImGui::PopItemFlag();
                if (ImGui::BeginItemTooltip())
                {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
                    if (g.help[0]) ImGui::TextUnformatted(g.help);
                    ImGui::TextDisabled("%s", g.classes);
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            // Cross-cutting switches that live on tags rather than classes.
            ImGui::TableNextColumn();
            bool quest = !c.skipQuestItems;
            if (ImGui::Checkbox("Quest items", &quest)) { c.skipQuestItems = !quest; Settings::MarkDirty(); }
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted("Anything tagged quest, across every class. Off keeps story pickups and puzzle pieces for your own hands."); ImGui::EndTooltip(); }
            ImGui::TableNextColumn();
            bool nosell = !c.skipNoSell;
            if (ImGui::Checkbox("Unsellable items", &nosell)) { c.skipNoSell = !nosell; Settings::MarkDirty(); }
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted("Items no shop will buy, across every class."); ImGui::EndTooltip(); }
            ImGui::TableNextColumn();
            auto mf = c.tagRule.find("memory-fragment"); auto gm = c.tagRule.find("gimmick");
            const bool mfOn = mf != c.tagRule.end() && mf->second > 0, gmOn = gm != c.tagRule.end() && gm->second > 0;
            bool prot = mfOn && gmOn;
            if (mfOn != gmOn) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("Memory chips and puzzle parts", &prot))
            {
                if (prot) { c.tagRule["memory-fragment"] = 1; c.tagRule["gimmick"] = 1; }
                else { c.tagRule.erase("memory-fragment"); c.tagRule.erase("gimmick"); }
                Settings::MarkDirty();
            }
            if (mfOn != gmOn) ImGui::PopItemFlag();
            if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f); ImGui::TextUnformatted("Protected by default: a memory chip starts a memory scene when taken, and a puzzle part breaks its puzzle. Turn on only if you want them auto-taken."); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            ImGui::EndTable();
        }

        Section("Classes");
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

        Section("Overlay");
        OnOff("DirectX 12 hooks", st.hooksOk, "installed", "failed");
        OnOff("Item database", ItemDb::Loaded(), "loaded", "missing MasterLooter.items.tsv");
        if (ItemDb::Loaded()) { ImGui::SameLine(); ImGui::TextDisabled("%d items, %d classes, %d tags", ItemDb::Count(), static_cast<int>(ItemDb::Classes().size()), static_cast<int>(ItemDb::Tags().size())); }

        Section("Loot engine");
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
        ImGui::Text("Scans %ld, events sent %ld, pump ticks %ld, guarded faults %ld, node kinds learned %d", s.scans, s.sent, s.pumpTicks, s.faults, s.learned);
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

        Section("Recent loot");
        static loot::Recent recent[12];
        const int rn = loot::CopyRecent(recent, 12);
        if (!rn) ImGui::TextDisabled("nothing taken yet this session");
        for (int i = 0; i < rn; ++i) ImGui::TextUnformatted(recent[i].text);

        Section("Log");
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
        Config& c = Settings::Get();
        ImGuiIO& io = ImGui::GetIO();
        const bool capt = st.Captures();
        io.MouseDrawCursor = capt;
        st.renderTid = GetCurrentThreadId();
        static bool s_wasCapt = false;
        if (capt != s_wasCapt) { s_wasCapt = capt; if (capt) input::MenuOpened(); else input::MenuClosed(); }
        if (capt) input::FeedMouse(io);
        else { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); io.AddFocusEvent(capt); } // nothing hovers or reacts while watching

        if (c.showHud) DrawNotice();
        if (!st.menuOpen) { st.textCapture = false; if (st.rebindCapture) { st.rebindCapture = false; g_rebindTarget = -1; } return; }

        ImGui::SetNextWindowSize(ImVec2(900 * g_scale, 640 * g_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(80 * g_scale, 80 * g_scale), ImGuiCond_FirstUseEver);
        bool open = true;
        const ImGuiWindowFlags watchFlags = st.menuWatch ? (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus) : 0;
        if (st.menuWatch) ImGui::SetNextWindowBgAlpha(0.72f);
        if (ImGui::Begin("Master Looter", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | watchFlags))
        {
            // Title strip: serif name in gold, version and close on the right, bronze rule.
            ImGui::PushFont(g_fontHead);
            ImGui::TextColored(kGold, "MASTER LOOTER");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "  v%s", ML_VERSION);
            const float closeW = ImGui::CalcTextSize("Close").x + ImGui::CalcTextSize("Watch").x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeW);
            if (ImGui::SmallButton("Watch")) st.menuWatch = true;
            if (ImGui::BeginItemTooltip()) { ImGui::Text("Keep the menu on screen while you play. %s brings it back, %s closes it.", Settings::KeyName(c.menuKey), Settings::KeyName(c.keyWatch)); ImGui::EndTooltip(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Close")) open = false;
            if (st.menuWatch)
                ImGui::TextColored(kGoldDim, "Watch mode: the game has your controls. %s to interact, %s to close.", Settings::KeyName(c.menuKey), Settings::KeyName(c.keyWatch));
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
                    ImGui::PushFont(g_fontHead);
                    const bool sel = ImGui::BeginTabItem(t.name);
                    ImGui::PopFont();
                    if (sel) { ImGui::Dummy(ImVec2(0, 4 * g_scale)); t.fn(c); ImGui::EndTabItem(); }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (!open) { st.menuOpen = false; st.menuWatch = false; }
        st.textCapture = capt && io.WantTextInput;
    }
}
