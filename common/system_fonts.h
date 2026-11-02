#pragma once
// The UI fonts the port borrows from the operating system (none ship with it): the launcher look, the in-game menu and the button prompts.
//   sans: the primary text face, then fallbacks merged into it for Chinese, Korean and Japanese (the fallbacks may be missing).
//   sans_bold: the prompt labels.   serif: the headings.
#include <filesystem>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace uc {
enum class UiFont { sans, sans_bold, serif };

// The folder of the system fonts, empty when it cannot be found.
inline std::filesystem::path system_font_folder() {
#ifdef _WIN32
    wchar_t windows[32768];
    const auto length = GetWindowsDirectoryW(windows, 32768);
    if (!length || length >= 32768) return {};
    return std::filesystem::path(windows) / L"Fonts";
#elif defined(__APPLE__)
    return "/System/Library/Fonts";
#else
    return "/system/fonts";   // Android
#endif
}

// The files for a role, most important first; for sans the first one is required and the rest are fallbacks.
inline std::vector<std::filesystem::path> ui_font_files(UiFont role) {
    const auto folder = system_font_folder();
    if (folder.empty()) return {};
#ifdef _WIN32
    switch (role) {
    case UiFont::sans: return {folder / L"segoeui.ttf", folder / L"msyh.ttc", folder / L"malgun.ttf", folder / L"msgothic.ttc"};
    case UiFont::sans_bold: return {folder / L"segoeuib.ttf"};
    case UiFont::serif: return {folder / L"georgia.ttf"};
    }
#elif defined(__APPLE__)
    const auto supplemental = folder / "Supplemental";
    switch (role) {
    case UiFont::sans: return {supplemental / "Arial.ttf", folder / "Hiragino Sans GB.ttc", folder / "AppleSDGothicNeo.ttc"};
    case UiFont::sans_bold: return {supplemental / "Arial Bold.ttf"};
    case UiFont::serif: return {supplemental / "Georgia.ttf"};
    }
#else
    switch (role) {
    case UiFont::sans: return {folder / "Roboto-Regular.ttf", folder / "NotoSansCJK-Regular.ttc"};
    case UiFont::sans_bold: return {folder / "Roboto-Bold.ttf"};
    case UiFont::serif: return {folder / "NotoSerif-Regular.ttf"};
    }
#endif
    return {};
}
}  // namespace uc
