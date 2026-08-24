#include "d3d9_proxy_internal.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>

namespace stardom {
namespace {

using CreateFontIndirectAFn = HFONT (WINAPI*)(const LOGFONTA*);

CreateFontIndirectAFn g_original_create_font_indirect_a = nullptr;
bool g_font_hook_attempted = false;
bool g_font_hook_installed = false;

const Config& FontConfig() {
    static const Config config = LoadConfig(IniPath());
    return config;
}

LONG ScaleFontHeight(LONG height, double scale) {
    if (height == 0) {
        return 0;
    }
    const double scaled = std::clamp(
        static_cast<double>(height) * scale,
        static_cast<double>(LONG_MIN), static_cast<double>(LONG_MAX));
    const LONG rounded = static_cast<LONG>(std::lround(scaled));
    if (rounded == 0) {
        return height < 0 ? -1 : 1;
    }
    return rounded;
}

HFONT WINAPI HookCreateFontIndirectA(const LOGFONTA* font) {
    if (!font || _stricmp(font->lfFaceName, "MingLiU") != 0) {
        return g_original_create_font_indirect_a(font);
    }

    LOGFONTW replacement{};
    replacement.lfHeight = ScaleFontHeight(
        font->lfHeight, FontConfig().font_scale);
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
    wcsncpy_s(replacement.lfFaceName, FontConfig().font_name.c_str(),
              _TRUNCATE);

    HFONT created = CreateFontIndirectW(&replacement);
    return created ? created : g_original_create_font_indirect_a(font);
}

}  // namespace

bool InstallFontReplacementHook() {
    if (g_font_hook_attempted) {
        return g_font_hook_installed;
    }
    g_font_hook_attempted = true;
    g_font_hook_installed = PatchImport(
        GetModuleHandleW(nullptr), "gdi32.dll", "CreateFontIndirectA",
        reinterpret_cast<void*>(&HookCreateFontIndirectA),
        reinterpret_cast<void**>(&g_original_create_font_indirect_a));
    return g_font_hook_installed;
}

}  // namespace stardom
