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
            std::string cols[4]; int c = 0;
            for (const char* p = line.c_str(); *p && c < 4; ++p)
            {
                if (*p == '\t') { ++c; continue; }
                if (*p == '\r' || *p == '\n') break;
                cols[c] += *p;
            }
            if (c < 3 || cols[0].empty() || cols[1].empty()) continue;
            NodeType n;
            n.prefab = cols[0]; n.kind = cols[1]; n.itemKey = cols[2]; n.name = cols[3];
            g_byPrefab[n.prefab] = g_rows.size();
            g_rows.push_back(std::move(n));
        }
        g_loaded = !g_rows.empty();
        return g_loaded;
    }

    bool Loaded() { return g_loaded; }
    const char* Source() { return g_source; }
    int Count() { return static_cast<int>(g_rows.size()); }

    const NodeType* ByPrefab(const char* path)
    {
        if (!g_loaded) return nullptr;
        const std::string k = Key(path);
        if (k.empty()) return nullptr;
        auto it = g_byPrefab.find(k);
        return it == g_byPrefab.end() ? nullptr : &g_rows[it->second];
    }
}
