#include "font_config.h"

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
              L"FontPatch", key, value, path.c_str()) != FALSE,
          "failed to write temporary config option");
}

}  // namespace

int wmain() {
    wchar_t temp_directory[MAX_PATH]{};
    wchar_t temp_path[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp_directory) != 0,
          "GetTempPath failed");
    Check(GetTempFileNameW(temp_directory, L"S3F", 0, temp_path) != 0,
          "GetTempFileName failed");
    const std::wstring path(temp_path);

    auto config = stardom_font::LoadFontConfig(path);
    Check(config.enabled, "Enabled must default to true");
    Check(config.font_name == L"SimHei",
          "FontName must default to SimHei");
    Check(config.font_scale == 1.00, "FontScale must default to 1.00");
    Check(config.small_font_name == L"SimSun",
          "SmallFontName must default to SimSun");
    Check(config.small_font_max_height == 16,
          "SmallFontMaxHeight must default to 16");
    Check(config.small_font_scale == 1.00,
          "SmallFontScale must default to 1.00");

    WriteOption(path, L"Enabled", L"0");
    WriteOption(path, L"FontName", L"Arial");
    WriteOption(path, L"FontScale", L"1.25");
    WriteOption(path, L"SmallFontName", L"Tahoma");
    WriteOption(path, L"SmallFontMaxHeight", L"14");
    WriteOption(path, L"SmallFontScale", L"1.10");
    config = stardom_font::LoadFontConfig(path);
    Check(!config.enabled, "Enabled=0 was not loaded");
    Check(config.font_name == L"Arial", "custom FontName was not loaded");
    Check(config.font_scale == 1.25, "custom FontScale was not loaded");
    Check(config.small_font_name == L"Tahoma",
          "custom SmallFontName was not loaded");
    Check(config.small_font_max_height == 14,
          "custom SmallFontMaxHeight was not loaded");
    Check(config.small_font_scale == 1.10,
          "custom SmallFontScale was not loaded");

    WriteOption(path, L"FontScale", L"2.0");
    config = stardom_font::LoadFontConfig(path);
    Check(config.font_scale == 1.50, "FontScale upper bound failed");
    WriteOption(path, L"FontScale", L"invalid");
    config = stardom_font::LoadFontConfig(path);
    Check(config.font_scale == 1.00, "invalid FontScale fallback failed");
    WriteOption(path, L"SmallFontMaxHeight", L"200");
    WriteOption(path, L"SmallFontScale", L"0.5");
    config = stardom_font::LoadFontConfig(path);
    Check(config.small_font_max_height == 128,
          "SmallFontMaxHeight upper bound failed");
    Check(config.small_font_scale == 0.75,
          "SmallFontScale lower bound failed");
    WriteOption(path, L"FontName",
                L"This font face name is longer than LOGFONT permits");
    config = stardom_font::LoadFontConfig(path);
    Check(config.font_name == L"SimHei",
          "overlong FontName fallback failed");
    WriteOption(path, L"SmallFontName",
                L"This small font face name is longer than LOGFONT permits");
    config = stardom_font::LoadFontConfig(path);
    Check(config.small_font_name == L"SimSun",
          "overlong SmallFontName fallback failed");

    DeleteFileW(path.c_str());
    return failures == 0 ? 0 : 1;
}
