#include "paths.h"

namespace ml::Paths
{
    static std::wstring g_dir;

    void Init(HMODULE module)
    {
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
}
