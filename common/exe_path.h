#pragma once
// The running executable's path on every host (GetModuleFileNameW, _NSGetExecutablePath, /proc/self/exe); empty when it cannot be read.
#include <filesystem>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace uc {
inline std::filesystem::path executable_path() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return path;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size + 1, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(path.data()), error);
    return error ? std::filesystem::path(path.data()) : resolved;
#else
    std::error_code error;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : resolved;
#endif
}
// The folder of the running executable (uc.exe / uc and their data next to it).
inline std::filesystem::path executable_dir() { return executable_path().parent_path(); }
}  // namespace uc
