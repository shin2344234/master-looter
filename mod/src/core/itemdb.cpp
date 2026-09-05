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
    static std::vector<std::pair<std::string, int>>     g_classes;
    static std::vector<std::pair<std::string, int>>     g_tags;
    static bool                                         g_loaded = false;

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
        FILE* f = _wfopen(Paths::File(L"MasterLooter.items.tsv").c_str(), L"rb");
        if (!f) return false;
        std::string text;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);

        std::map<std::string, int> classCount, tagCount;
        std::vector<std::string> cols;
        g_items.clear();
        g_byKey.clear();
        size_t pos = 0;
        bool header = true;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (header) { header = false; continue; } // key string_key name class tags tier value
            if (line.empty()) continue;
            Split(line, cols);
            if (cols.size() < 7) continue;
            Item it;
            it.key       = static_cast<uint32_t>(strtoul(cols[0].c_str(), nullptr, 10));
            it.stringKey = cols[1];
            it.name      = cols[2];
            it.klass     = cols[3];
            it.tags      = " " + cols[4] + " ";
            it.tier      = atoi(cols[5].c_str());
            it.value     = cols[6].empty() ? -1 : atoll(cols[6].c_str());
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
            g_items.push_back(std::move(it));
        }
        g_classes.assign(classCount.begin(), classCount.end());
        std::sort(g_classes.begin(), g_classes.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
        g_tags.assign(tagCount.begin(), tagCount.end());
        g_loaded = !g_items.empty();
        return g_loaded;
    }

    bool Loaded() { return g_loaded; }
    int  Count()  { return static_cast<int>(g_items.size()); }

    const Item* Find(uint32_t key)
    {
        auto it = g_byKey.find(key);
        return it == g_byKey.end() ? nullptr : &g_items[it->second];
    }

    const std::vector<Item>& All() { return g_items; }
    const std::vector<std::pair<std::string, int>>& Classes() { return g_classes; }
    const std::vector<std::pair<std::string, int>>& Tags() { return g_tags; }
}
