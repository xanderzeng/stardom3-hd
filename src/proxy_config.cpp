#include "proxy_config.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string>

namespace stardom {
namespace {

int ReadResolutionValue(const std::wstring& ini_path, const wchar_t* key,
                        int default_value, bool& present, bool& valid) {
    wchar_t text[64]{};
    const DWORD length = GetPrivateProfileStringW(
        L"Widescreen", key, L"", text, static_cast<DWORD>(std::size(text)),
        ini_path.c_str());
    present = length != 0;
    if (!present) {
        return default_value;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = std::wcstol(text, &end, 10);
    valid = errno == 0 && end != text && *end == L'\0' &&
        parsed >= INT_MIN && parsed <= INT_MAX;
    return valid ? static_cast<int>(parsed) : 0;
}

std::wstring ReadFontName(const std::wstring& ini_path, const wchar_t* key,
                          const wchar_t* default_name) {
    wchar_t value[256]{};
    const DWORD length = GetPrivateProfileStringW(
        L"Widescreen", key, default_name, value,
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
        L"Widescreen", key, L"1.00", text,
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

Config LoadConfig(const std::wstring& ini_path) {
    Config config;
    config.enabled = GetPrivateProfileIntW(
        L"Widescreen", L"Enabled", 1, ini_path.c_str()) != 0;

    bool width_present = false;
    bool height_present = false;
    bool width_valid = true;
    bool height_valid = true;
    config.configured_width = ReadResolutionValue(
        ini_path, L"Width", kDefaultResolution.width,
        width_present, width_valid);
    config.configured_height = ReadResolutionValue(
        ini_path, L"Height", kDefaultResolution.height,
        height_present, height_valid);
    const auto validation = ValidateResolution(
        config.configured_width, config.configured_height);
    config.resolution_valid = width_valid && height_valid &&
        width_present == height_present && validation.supported;
    config.resolution = validation.mode;

    config.borderless = GetPrivateProfileIntW(
        L"Widescreen", L"Borderless", 0, ini_path.c_str()) != 0;
    config.native_render = GetPrivateProfileIntW(
        L"Widescreen", L"NativeRender", 0, ini_path.c_str()) != 0;
    config.ui_scale_mode = std::clamp(static_cast<int>(GetPrivateProfileIntW(
        L"Widescreen", L"UIScaleMode", 1, ini_path.c_str())), 0, 2);
    config.debug_mode = GetPrivateProfileIntW(
        L"Widescreen", L"DebugMode", 0, ini_path.c_str()) != 0;
    config.ui_draw_diagnostics = config.debug_mode &&
        GetPrivateProfileIntW(
            L"Widescreen", L"UIDrawDiagnostics", 0, ini_path.c_str()) != 0;
    config.suppress_transparent_ui = GetPrivateProfileIntW(
        L"Widescreen", L"SuppressTransparentUI", 0, ini_path.c_str()) != 0;
    config.ui_container_probe = config.debug_mode &&
        GetPrivateProfileIntW(
            L"Widescreen", L"UIContainerProbe", 0, ini_path.c_str()) != 0;
    config.suppress_proxy_containers = GetPrivateProfileIntW(
        L"Widescreen", L"SuppressProxyContainers", 0, ini_path.c_str()) != 0;
    config.gui_runtime_probe = config.debug_mode &&
        GetPrivateProfileIntW(
            L"Widescreen", L"GUIRuntimeProbe", 0, ini_path.c_str()) != 0;
    config.unified_ui_layout = GetPrivateProfileIntW(
        L"Widescreen", L"UnifiedUILayout", 1, ini_path.c_str()) != 0;
    config.title_screen_mode = std::clamp(static_cast<int>(
        GetPrivateProfileIntW(L"Widescreen", L"TitleScreenMode", 1,
                              ini_path.c_str())), 0, 1);
    config.font_name = ReadFontName(
        ini_path, L"FontName", L"SimHei");
    config.font_scale = ReadFontScale(ini_path, L"FontScale");
    config.small_font_name = ReadFontName(
        ini_path, L"SmallFontName", L"SimSun");
    config.small_font_max_height = std::clamp(static_cast<int>(
        GetPrivateProfileIntW(L"Widescreen", L"SmallFontMaxHeight", 16,
                              ini_path.c_str())), 0, 128);
    config.small_font_scale = ReadFontScale(
        ini_path, L"SmallFontScale");
    return config;
}

}  // namespace stardom
