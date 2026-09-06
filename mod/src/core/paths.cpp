#include "paths.h"

#include <cstdio>

namespace ml::Paths
{
    static std::wstring g_dir;
    static HMODULE      g_module = nullptr;

    void Init(HMODULE module)
    {
        g_module = module;
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(module, buf, MAX_PATH);
        std::wstring p(buf);
        const size_t slash = p.find_last_of(L"\\/");
        g_dir = (slash == std::wstring::npos) ? L".\\" : p.substr(0, slash + 1);
    }

    const std::wstring& Dir() { return g_dir; }

    std::wstring File(const wchar_t* name) { return g_dir + name; }

    std::string FileUtf8(const wchar_t* name)
    {
        const std::wstring w = File(name);
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    HMODULE Module() { return g_module; }

    bool ReadDataText(const wchar_t* fileName, const wchar_t* resourceName, std::string& out, bool* fromFile)
    {
        out.clear();
        if (fromFile) *fromFile = false;
        if (FILE* f = _wfopen(File(fileName).c_str(), L"rb"))
        {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
            fclose(f);
            if (!out.empty()) { if (fromFile) *fromFile = true; return true; }
        }
        if (!g_module) return false;
        const HRSRC res = FindResourceW(g_module, resourceName, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        if (!res) return false;
        const HGLOBAL h = LoadResource(g_module, res);
        const void* p = h ? LockResource(h) : nullptr;
        const DWORD n = SizeofResource(g_module, res);
        if (!p || !n) return false;
        out.assign(static_cast<const char*>(p), n);
        return true;
    }
}
