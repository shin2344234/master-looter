#include "itemdb.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_map>

#include "log.h"
#include "paths.h"

namespace ml::ItemDb
{
    static std::vector<Item>                            g_items;
    static std::unordered_map<uint32_t, size_t>         g_byKey;
    static std::unordered_map<std::string, size_t>      g_byName;
    static std::vector<int>                             g_byRow; // row -> index, -1 when absent
    static std::vector<std::pair<std::string, int>>     g_classes;
    static std::vector<std::pair<std::string, int>>     g_tags;
    static bool                                         g_loaded = false;
    static const char*                                  g_source = "none";

    static void Split(const std::string& line, std::vector<std::string>& out)
    {
        out.clear();
        size_t p = 0;
        while (true)
        {
            size_t t = line.find('\t', p);
            if (t == std::string::npos) { out.push_back(line.substr(p)); break; }
            out.push_back(line.substr(p, t - p));
            p = t + 1;
        }
    }

    bool Load()
    {
        std::string text;
        bool fromFile = false;
        if (!Paths::ReadDataText(L"MasterLooter.items.tsv", L"ML_ITEMS_TSV", text, &fromFile)) return false;
        g_source = fromFile ? "file next to the plugin" : "built into the plugin";

        std::map<std::string, int> classCount, tagCount;
        std::vector<std::string> cols;
        g_items.clear();
        g_byKey.clear();
        g_byName.clear();
        size_t pos = 0;
        bool header = true, hasRow = false;
        int maxRow = -1;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (header) { header = false; hasRow = line.rfind("row\t", 0) == 0; continue; }
            if (line.empty()) continue;
            Split(line, cols);
            size_t c = 0;
            Item it;
            if (hasRow) { if (cols.size() < 8) continue; it.row = atoi(cols[c++].c_str()); }
            else if (cols.size() < 7) continue;
            it.key       = static_cast<uint32_t>(strtoul(cols[c++].c_str(), nullptr, 10));
            it.stringKey = cols[c++];
            it.name      = cols[c++];
            it.klass     = cols[c++];
            it.tags      = " " + cols[c++] + " ";
            it.tier      = atoi(cols[c++].c_str());
            it.value     = cols[c].empty() ? -1 : atoll(cols[c].c_str());
            if (it.row > maxRow) maxRow = it.row;
            classCount[it.klass]++;
            size_t a = 1;
            while (a < it.tags.size())
            {
                size_t b = it.tags.find(' ', a);
                if (b == std::string::npos) break;
                if (b > a) tagCount[it.tags.substr(a, b - a)]++;
                a = b + 1;
            }
            g_byKey[it.key] = g_items.size();
            if (!it.stringKey.empty()) g_byName.emplace(it.stringKey, g_items.size());
            g_items.push_back(std::move(it));
        }
        g_byRow.assign(maxRow >= 0 ? static_cast<size_t>(maxRow) + 1 : 0, -1);
        for (size_t i = 0; i < g_items.size(); ++i)
            if (g_items[i].row >= 0) g_byRow[g_items[i].row] = static_cast<int>(i);
        g_classes.assign(classCount.begin(), classCount.end());
        std::sort(g_classes.begin(), g_classes.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
        g_tags.assign(tagCount.begin(), tagCount.end());
        g_loaded = !g_items.empty();
        if (g_loaded && !hasRow)
            LOG_ERR("MasterLooter.items.tsv has no row column; regenerate it with scripts/make_itemdb_tsv.py. Items will be matched by name only.");
        return g_loaded;
    }

    bool Loaded() { return g_loaded; }
    const char* Source() { return g_source; }
    int  Count()  { return static_cast<int>(g_items.size()); }

    const Item* Find(uint32_t key)
    {
        auto it = g_byKey.find(key);
        return it == g_byKey.end() ? nullptr : &g_items[it->second];
    }

    const Item* ByRow(int row)
    {
        if (row < 0 || static_cast<size_t>(row) >= g_byRow.size() || g_byRow[row] < 0) return nullptr;
        return &g_items[g_byRow[row]];
    }

    const Item* ByStringKey(const char* stringKey)
    {
        if (!stringKey || !*stringKey) return nullptr;
        auto it = g_byName.find(stringKey);
        return it == g_byName.end() ? nullptr : &g_items[it->second];
    }

    const std::vector<Item>& All() { return g_items; }
    const std::vector<std::pair<std::string, int>>& Classes() { return g_classes; }
    const std::vector<std::pair<std::string, int>>& Tags() { return g_tags; }
}
