#include "font_hook.h"

#include "font_config.h"
#include "import_hook.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace stardom_font {
namespace {

using CreateFontIndirectAFn = HFONT (WINAPI*)(const LOGFONTA*);

ImportHook g_create_font_hook;
bool g_hook_attempted = false;

std::wstring IniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path,
                       MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    const std::wstring directory =
        slash == std::wstring::npos ? L"." : value.substr(0, slash);
    return directory + L"\\Stardom3.FontPatch.ini";
}

const FontConfig& Config() {
    static const FontConfig config = LoadFontConfig(IniPath());
    return config;
}

LONG ScaleHeight(LONG height, double scale) {
    if (height == 0) {
        return 0;
    }
    const double scaled = std::clamp(
        static_cast<double>(height) * scale,
        static_cast<double>(LONG_MIN), static_cast<double>(LONG_MAX));
    const LONG rounded = static_cast<LONG>(std::lround(scaled));
    return rounded != 0 ? rounded : (height < 0 ? -1 : 1);
}

LONG FontPixelHeight(LONG height) {
    if (height == LONG_MIN) {
        return LONG_MAX;
    }
    return height < 0 ? -height : height;
}

HFONT WINAPI HookCreateFontIndirectA(const LOGFONTA* font) {
    const auto original = reinterpret_cast<CreateFontIndirectAFn>(
        g_create_font_hook.original);
    if (!original || !font || !Config().enabled ||
        _stricmp(font->lfFaceName, "MingLiU") != 0) {
        return original ? original(font) : nullptr;
    }

    const FontConfig& config = Config();
    const bool use_small_font = config.small_font_max_height > 0 &&
        FontPixelHeight(font->lfHeight) <= config.small_font_max_height;
    const std::wstring& replacement_name = use_small_font
        ? config.small_font_name : config.font_name;
    const double replacement_scale = use_small_font
        ? config.small_font_scale : config.font_scale;

    LOGFONTW replacement{};
    replacement.lfHeight = ScaleHeight(font->lfHeight, replacement_scale);
    replacement.lfWidth = font->lfWidth;
    replacement.lfEscapement = font->lfEscapement;
    replacement.lfOrientation = font->lfOrientation;
    replacement.lfWeight = font->lfWeight;
    replacement.lfItalic = font->lfItalic;
    replacement.lfUnderline = font->lfUnderline;
    replacement.lfStrikeOut = font->lfStrikeOut;
    replacement.lfCharSet = font->lfCharSet;
    replacement.lfOutPrecision = font->lfOutPrecision;
    replacement.lfClipPrecision = font->lfClipPrecision;
    replacement.lfQuality = font->lfQuality;
    replacement.lfPitchAndFamily = font->lfPitchAndFamily;
    wcsncpy_s(replacement.lfFaceName, replacement_name.c_str(), _TRUNCATE);

    HFONT created = CreateFontIndirectW(&replacement);
    return created ? created : original(font);
}

}  // namespace

bool OutlineEnabled() { return Config().enabled && Config().font_outline_union; }

bool InstallFontHook() {
    if (g_hook_attempted) {
        return g_create_font_hook.slot != nullptr;
    }
    g_hook_attempted = true;
    return PatchImport(
        GetModuleHandleW(nullptr), "gdi32.dll", "CreateFontIndirectA",
        reinterpret_cast<void*>(&HookCreateFontIndirectA),
        g_create_font_hook);
}

bool UninstallFontHook() {
    return RestoreImport(g_create_font_hook);
}

}  // namespace stardom_font
