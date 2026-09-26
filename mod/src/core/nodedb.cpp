#include "nodedb.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "paths.h"

namespace ml::NodeDb
{
    static std::vector<NodeType>                    g_rows;
    static int                                      g_statePick = 0;
    static std::unordered_map<std::string, size_t>  g_byPrefab;
    static bool                                     g_loaded = false;
    static const char*                              g_source = "";

    // Basename, lower case, without the extension and without the wrapper
    // suffix a placed object's prefab carries. Kept identical to prefab_key()
    // in scripts/make_nodes_tsv.py: both sides of the lookup must agree.
    static std::string Key(const char* path)
    {
        if (!path || !*path) return std::string();
        const char* slash = strrchr(path, '/');
        const char* back  = strrchr(path, '\\');
        if (back > slash) slash = back;
        std::string s = slash ? slash + 1 : path;
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        static const char kExt[] = ".prefab";
        if (s.size() > sizeof kExt - 1 && s.compare(s.size() - (sizeof kExt - 1), sizeof kExt - 1, kExt) == 0)
            s.resize(s.size() - (sizeof kExt - 1));
        static const char kWrap[] = "_scenecollector";
        if (s.size() > sizeof kWrap - 1 && s.compare(s.size() - (sizeof kWrap - 1), sizeof kWrap - 1, kWrap) == 0)
            s.resize(s.size() - (sizeof kWrap - 1));
        return s;
    }

    bool Load()
    {
        std::string text;
        bool fromFile = false;
        if (!Paths::ReadDataText(L"MasterLooter.nodes.tsv", L"ML_NODES_TSV", text, &fromFile)) return false;
        g_source = fromFile ? "file next to the plugin" : "built into the plugin";
        bool header = true;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            const std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (header) { header = false; continue; }
            // Seven columns now. A six-column table still loads and no node in
            // it says what it can hand over, a five-column one has every node
            // breakable, which is what the mod did before that column existed,
            // and a four-column one counts every row as a guess.
            // Ten columns, statepick the last. The array and the loop stopped
            // at nine from 17 September to 26 September 2026, so the loop never
            // read that column and cols[9] read one past the end of the array:
            // no plant was ever picked through its state, from 1.6.26 on.
            // LuxDragon's Palmar Leaves armed 457 times in one 1.6.42 session
            // and never filled. kCols is the one number to change for a new
            // column.
            constexpr int kCols = 10;
            std::string cols[kCols]; int c = 0;
            for (const char* p = line.c_str(); *p && c < kCols; ++p)
            {
                if (*p == '\t') { ++c; continue; }
                if (*p == '\r' || *p == '\n') break;
                cols[c] += *p;
            }
            if (c < 3 || cols[0].empty() || cols[1].empty()) continue;
            NodeType n;
            n.prefab = cols[0]; n.kind = cols[1]; n.itemKey = cols[2]; n.name = cols[3];
            // "seen" is a prefab somebody watched the game offer a pick-up on,
            // for the handful the game tags as nothing at all. It counts the
            // same as the game's own tag: both are evidence, and a name guess
            // is the only thing that is not.
            // "tag" is the game's own word, "folder" is the loose-item folder
            // it filed the prefab in. A name guess is the only one of the three
            // that is not evidence, and it is the only one left out.
            n.tagged = cols[4] == "tag" || cols[4] == "seen" || cols[4] == "folder";
            // The kind says what a thing is; this says how to reach it. They
            // are not the same question, and firewood is where they part: the
            // game files it as wood you collect and as a small box you pick up,
            // both at once, so its kind is wood and its verb is Take. A table
            // without the column leaves the two welded together, which is how
            // every build before this one behaved. Issue #63.
            n.pickup = n.kind == "pickup" || (c >= 8 && cols[8] == "1");
            n.breaks = c < 5 || cols[5] != "0";
            n.driven = c >= 7 && cols[7] == "1";
            n.statePick = c >= 9 && cols[9] == "1";
            if (n.statePick) ++g_statePick;
            if (c >= 6 && !cols[6].empty())
            {
                std::string one;
                for (char ch : cols[6])
                {
                    if (ch == ' ') { if (!one.empty()) { n.yields.push_back(one); one.clear(); } continue; }
                    one += ch;
                }
                if (!one.empty()) n.yields.push_back(one);
            }
            g_byPrefab[n.prefab] = g_rows.size();
            g_rows.push_back(std::move(n));
        }
        g_loaded = !g_rows.empty();
        return g_loaded;
    }

    bool Loaded() { return g_loaded; }
    const char* Source() { return g_source; }
    int Count() { return static_cast<int>(g_rows.size()); }
    int StatePickCount() { return g_statePick; }

    const NodeType* ByPrefab(const char* path)
    {
        if (!g_loaded) return nullptr;
        const std::string k = Key(path);
        if (k.empty()) return nullptr;
        auto it = g_byPrefab.find(k);
        return it == g_byPrefab.end() ? nullptr : &g_rows[it->second];
    }
}
