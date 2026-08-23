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

    WriteOption(path, L"DebugMode", L"1");
    config = stardom::LoadConfig(path);
    Check(config.debug_mode, "DebugMode=1 was not loaded");
    Check(config.ui_draw_diagnostics,
          "draw diagnostics did not activate in DebugMode");
    Check(config.ui_container_probe,
          "container probe did not activate in DebugMode");
    Check(config.gui_runtime_probe,
          "runtime probe did not activate in DebugMode");

    DeleteFileW(path.c_str());
    return failures == 0 ? 0 : 1;
}
