#pragma once
// fopen of a filesystem path on every host: _wfopen with the wide path on Windows (any Unicode name), fopen with the native UTF-8 path elsewhere.
#include <cstdio>
#include <filesystem>
#include <string>

namespace uc {
inline std::FILE* uc_fopen(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
}  // namespace uc
