#include "font_config.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace stardom_font {
namespace {

std::wstring ReadFontName(const std::wstring& ini_path, const wchar_t* key,
                          const wchar_t* default_name) {
    wchar_t value[256]{};
    const DWORD length = GetPrivateProfileStringW(
        L"FontPatch", key, default_name, value,
        static_cast<DWORD>(std::size(value)), ini_path.c_str());
    if (length == 0 || length >= LF_FACESIZE) {
        return default_name;
    }
    return value;
}

double ReadFontScale(const std::wstring& ini_path, const wchar_t* key) {
    constexpr double kDefaultFontScale = 1.00;
    wchar_t text[64]{};
    const DWORD length = GetPrivateProfileStringW(
        L"FontPatch", key, L"1.00", text,
        static_cast<DWORD>(std::size(text)), ini_path.c_str());
    if (length == 0) {
        return kDefaultFontScale;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const double parsed = std::wcstod(text, &end);
    if (errno != 0 || end == text || *end != L'\0' ||
        !std::isfinite(parsed)) {
        return kDefaultFontScale;
    }
    return std::clamp(parsed, 0.75, 1.50);
}

}  // namespace

FontConfig LoadFontConfig(const std::wstring& ini_path) {
    FontConfig config;
    config.enabled = GetPrivateProfileIntW(
        L"FontPatch", L"Enabled", 1, ini_path.c_str()) != 0;
    config.font_name = ReadFontName(
        ini_path, L"FontName", L"SimHei");
    config.font_scale = ReadFontScale(ini_path, L"FontScale");
    config.small_font_name = ReadFontName(
        ini_path, L"SmallFontName", L"SimSun");
    config.small_font_max_height = std::clamp(static_cast<int>(
        GetPrivateProfileIntW(L"FontPatch", L"SmallFontMaxHeight", 16,
                              ini_path.c_str())), 0, 128);
    config.small_font_scale = ReadFontScale(
        ini_path, L"SmallFontScale");
    const int small_font_weight = static_cast<int>(GetPrivateProfileIntW(
        L"FontPatch", L"SmallFontWeight", 0, ini_path.c_str()));
    config.small_font_weight = small_font_weight <= 0
        ? 0 : std::clamp(small_font_weight, 100, 900);
    return config;
}

}  // namespace stardom_font
