#pragma once
#include <Windows.h>
#include <string>

namespace ml::Paths
{
    void Init(HMODULE module);
    // Folder the .asi lives in, with a trailing backslash.
    const std::wstring& Dir();
    std::wstring File(const wchar_t* name);
    std::string  FileUtf8(const wchar_t* name);
}
