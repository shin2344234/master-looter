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
    // %LOCALAPPDATA%\MasterLooter\ with a trailing backslash, created on first
    // use, or empty when Windows names no such folder. Backups, presets and a
    // copy of the ini live here, out of reach of a mod manager that clears
    // every file named after the plugin in bin64 when it reinstalls it.
    const std::wstring& DataDir();
    std::string DataDirUtf8();
    HMODULE Module();
    // A data table: the file next to the plugin when one exists (an override,
    // for trying a regenerated table), otherwise the copy compiled into the
    // plugin as an RCDATA resource. `fromFile` reports which one was read.
    bool ReadDataText(const wchar_t* fileName, const wchar_t* resourceName, std::string& out, bool* fromFile);
}
