#include "proxy_config.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        ++failures;
    }
}

void WriteOption(const std::wstring& path, const wchar_t* key,
                 const wchar_t* value) {
    Check(WritePrivateProfileStringW(
              L"Widescreen", key, value, path.c_str()) != FALSE,
          "failed to write temporary config option");
}

}  // namespace

int wmain() {
    wchar_t temporary_directory[MAX_PATH]{};
    wchar_t temporary_path[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temporary_directory) != 0,
          "GetTempPath failed");
    Check(GetTempFileNameW(
              temporary_directory, L"S3C", 0, temporary_path) != 0,
          "GetTempFileName failed");
    const std::wstring path(temporary_path);

    WriteOption(path, L"UIDrawDiagnostics", L"1");
    WriteOption(path, L"UIContainerProbe", L"1");
    WriteOption(path, L"GUIRuntimeProbe", L"1");
    WriteOption(path, L"SuppressTransparentUI", L"1");
    WriteOption(path, L"SuppressProxyContainers", L"1");

    stardom::Config config = stardom::LoadConfig(path);
    Check(!config.debug_mode, "DebugMode must default to disabled");
    Check(!config.ui_draw_diagnostics,
          "draw diagnostics bypassed disabled DebugMode");
    Check(!config.ui_container_probe,
          "container probe bypassed disabled DebugMode");
    Check(!config.gui_runtime_probe,
          "runtime probe bypassed disabled DebugMode");
    Check(config.suppress_transparent_ui,
          "render compatibility option was incorrectly debug-gated");
    Check(config.suppress_proxy_containers,
          "proxy suppression option was incorrectly debug-gated");
    Check(config.font_name == L"SimHei",
          "FontName must default to SimHei");
    Check(config.font_scale == 1.00,
          "FontScale must default to 1.00");
    Check(config.small_font_name == L"SimSun",
          "SmallFontName must default to SimSun");
    Check(config.small_font_max_height == 16,
          "SmallFontMaxHeight must default to 16");
    Check(config.small_font_scale == 1.00,
          "SmallFontScale must default to 1.00");
    Check(config.small_font_weight == 0,
          "SmallFontWeight must default to preserving the original weight");

    WriteOption(path, L"FontName", L"Microsoft JhengHei");
    config = stardom::LoadConfig(path);
    Check(config.font_name == L"Microsoft JhengHei",
          "configured FontName was not loaded");

    WriteOption(path, L"SmallFontName", L"Arial");
    WriteOption(path, L"SmallFontMaxHeight", L"14");
    WriteOption(path, L"SmallFontScale", L"1.10");
    WriteOption(path, L"SmallFontWeight", L"600");
    config = stardom::LoadConfig(path);
    Check(config.small_font_name == L"Arial",
          "configured SmallFontName was not loaded");
    Check(config.small_font_max_height == 14,
          "configured SmallFontMaxHeight was not loaded");
    Check(config.small_font_scale == 1.10,
          "configured SmallFontScale was not loaded");
    Check(config.small_font_weight == 600,
          "configured SmallFontWeight was not loaded");

    WriteOption(path, L"FontScale", L"1.25");
    config = stardom::LoadConfig(path);
    Check(config.font_scale == 1.25,
          "configured FontScale was not loaded");
    WriteOption(path, L"FontScale", L"2.0");
    config = stardom::LoadConfig(path);
    Check(config.font_scale == 1.50,
          "FontScale upper bound was not enforced");
    WriteOption(path, L"FontScale", L"invalid");
    config = stardom::LoadConfig(path);
    Check(config.font_scale == 1.00,
          "malformed FontScale did not use the default");
    WriteOption(path, L"SmallFontMaxHeight", L"200");
    config = stardom::LoadConfig(path);
    Check(config.small_font_max_height == 128,
          "SmallFontMaxHeight upper bound was not enforced");
    WriteOption(path, L"SmallFontScale", L"0.5");
    config = stardom::LoadConfig(path);
    Check(config.small_font_scale == 0.75,
          "SmallFontScale lower bound was not enforced");
    WriteOption(path, L"SmallFontWeight", L"1200");
    config = stardom::LoadConfig(path);
    Check(config.small_font_weight == 900,
          "SmallFontWeight upper bound was not enforced");
    WriteOption(path, L"SmallFontWeight", L"50");
    config = stardom::LoadConfig(path);
    Check(config.small_font_weight == 100,
          "SmallFontWeight lower bound was not enforced");
    WriteOption(path, L"SmallFontWeight", L"0");
    config = stardom::LoadConfig(path);
    Check(config.small_font_weight == 0,
          "SmallFontWeight=0 did not preserve the original weight");

    WriteOption(path, L"DebugMode", L"1");
    config = stardom::LoadConfig(path);
    Check(config.debug_mode, "DebugMode=1 was not loaded");
    Check(config.ui_draw_diagnostics,
          "draw diagnostics did not activate in DebugMode");
    Check(config.ui_container_probe,
          "container probe did not activate in DebugMode");
    Check(config.gui_runtime_probe,
          "runtime probe did not activate in DebugMode");

    WriteOption(path, L"FontName",
                L"This font face name is longer than LOGFONT permits");
    config = stardom::LoadConfig(path);
    Check(config.font_name == L"SimHei",
          "overlong FontName did not use its default");

    WriteOption(path, L"SmallFontName",
                L"This small font face name is longer than LOGFONT permits");
    config = stardom::LoadConfig(path);
    Check(config.small_font_name == L"SimSun",
          "overlong SmallFontName did not use its default");

    DeleteFileW(path.c_str());
    return failures == 0 ? 0 : 1;
}
