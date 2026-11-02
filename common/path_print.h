#pragma once
// Printing a std::filesystem::path without a conversion that can throw.
// On Windows the native string is UTF-16 and path::string() converts to the ANSI code page, throwing for characters it cannot hold
// (a Cyrillic user profile under a Western code page); %ls with c_str() prints the native string as Windows always did. Elsewhere the
// native string is already the UTF-8 bytes.
//   std::fprintf(stderr, "report: " UC_PATH_FMT "\n", UC_PATH_ARG(path));
#include <filesystem>
#include <string>

#ifdef _WIN32
#define UC_PATH_FMT "%ls"
#else
#define UC_PATH_FMT "%s"
#endif
#define UC_PATH_ARG(p) (p).c_str()

namespace uc {
// The path as UTF-8 for messages (exception text): never depends on the code page.
inline std::string path_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
}  // namespace uc
