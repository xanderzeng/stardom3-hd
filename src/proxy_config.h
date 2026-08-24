#pragma once

#include "display_config.h"

#include <string>

namespace stardom {

struct Config {
    ResolutionMode resolution = kDefaultResolution;
    int configured_width = kDefaultResolution.width;
    int configured_height = kDefaultResolution.height;
    bool resolution_valid = true;
    bool enabled = true;
    bool borderless = false;
    bool native_render = false;
    int ui_scale_mode = 1;
    bool debug_mode = false;
    bool ui_draw_diagnostics = false;
    bool suppress_transparent_ui = false;
    bool ui_container_probe = false;
    bool suppress_proxy_containers = true;
    bool gui_runtime_probe = false;
    bool unified_ui_layout = true;
    int title_screen_mode = 1;
    std::wstring font_name = L"SimHei";
    double font_scale = 1.00;
};

Config LoadConfig(const std::wstring& ini_path);

}  // namespace stardom
