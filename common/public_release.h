#pragma once
#include "exe_path.h"
#include <cstdlib>
#include <filesystem>

namespace uc {
// True for an installed release client: release.json (the update manifest) sits next to the executable. Such a client writes no logs, crash reports or
// debug files. A development build (build\, dist\, the install folder of build.bat) has no release.json, so it keeps all of them.
// The environment variable UC_LOGS (any value) turns the logs back on in a release client, for developers and for tests of the update flow.
inline bool public_release() {
    static const bool value=[] {
#ifdef _WIN32
        if(GetEnvironmentVariableW(L"UC_LOGS",nullptr,0))return false;
#else
        if(std::getenv("UC_LOGS"))return false;
#endif
        const auto path=executable_path();
        if(path.empty())return false;
        std::error_code ignored;
        return std::filesystem::is_regular_file(path.parent_path()/"release.json",ignored);
    }();
    return value;
}
}
