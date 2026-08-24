#pragma once

#include <string>

namespace stardom_font {

struct FontConfig {
    bool enabled = true;
    std::wstring font_name = L"SimHei";
    double font_scale = 1.00;
    std::wstring small_font_name = L"SimSun";
    int small_font_max_height = 16;
    double small_font_scale = 1.00;
};

FontConfig LoadFontConfig(const std::wstring& ini_path);

}  // namespace stardom_font
