#include "font_hook.h"

#include "font_config.h"
#include "import_hook.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace stardom_font {
namespace {

using CreateFontIndirectAFn = HFONT (WINAPI*)(const LOGFONTA*);
using LabelSetTextFn = void(__thiscall*)(void*, const char*);

ImportHook g_create_font_hook;
bool g_hook_attempted = false;

struct InlineHook {
    unsigned char* target = nullptr;
    unsigned char* trampoline = nullptr;
    unsigned char original[6]{};
    bool installed = false;
};

InlineHook g_inventory_count_hook;
LabelSetTextFn g_original_label_set_text = nullptr;

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
    replacement.lfWeight = use_small_font && config.small_font_weight != 0
        ? config.small_font_weight : font->lfWeight;
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

bool IsInventoryCountText(const char* text) {
    if (!text || text[0] != 'x') {
        return false;
    }
    size_t index = 1;
    if (text[index] < '0' || text[index] > '9') {
        return false;
    }
    while (index <= 3 && text[index] >= '0' && text[index] <= '9') {
        ++index;
    }
    return index <= 3 && text[index] == '\0';
}

bool IsInventoryCountLabel(void* object) {
    if (!object) {
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }
    const uintptr_t image_base = reinterpret_cast<uintptr_t>(executable);
    auto* bytes = static_cast<unsigned char*>(object);
    if (*reinterpret_cast<uintptr_t*>(bytes) != image_base + 0x2F0348 ||
        *reinterpret_cast<int*>(bytes + 0x80) != 330 ||
        *reinterpret_cast<int*>(bytes + 0x84) != 5 ||
        *reinterpret_cast<int*>(bytes + 0x88) != 30 ||
        *reinterpret_cast<int*>(bytes + 0x8C) != 20 ||
        *reinterpret_cast<uint32_t*>(bytes + 0x100) != 0xFFE1007C ||
        *reinterpret_cast<int*>(bytes + 0x130) != 16) {
        return false;
    }

    void* parent = *reinterpret_cast<void**>(bytes + 0xF0);
    if (!parent) {
        return false;
    }
    auto* parent_bytes = static_cast<unsigned char*>(parent);
    const int parent_width = *reinterpret_cast<int*>(parent_bytes + 0x88);
    return *reinterpret_cast<uintptr_t*>(parent_bytes) ==
            image_base + 0x2F1560 &&
        (parent_width == 370 || parent_width == 380) &&
        *reinterpret_cast<int*>(parent_bytes + 0x8C) == 90;
}

void __fastcall HookLabelSetText(void* self, void*, const char* text) {
    const bool inventory_count_text = IsInventoryCountText(text);
    g_original_label_set_text(self, text);
    if (!inventory_count_text || !IsInventoryCountLabel(self)) {
        return;
    }

    // The game budgets exactly fontSize / 2 pixels for each single-byte
    // glyph. Bold SimSun can paint through the final cell boundary. Extend
    // only the inventory count's measured raster; its authored UI rectangle,
    // font, and all other small labels remain unchanged.
    auto* bytes = static_cast<unsigned char*>(self);
    *reinterpret_cast<int*>(bytes + 0x154) += 2;
}

bool IsStardom3Executable(HMODULE executable) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(executable, path, MAX_PATH)) {
        return false;
    }
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"Stardom3.exe") == 0;
}

bool InstallInventoryCountHook() {
    if (g_inventory_count_hook.installed) {
        return true;
    }
    const FontConfig& config = Config();
    if (!config.enabled || config.small_font_weight < 600) {
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable || !IsStardom3Executable(executable)) {
        return false;
    }

    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x130100;
    const unsigned char expected[] = {0x81, 0xEC, 0x00, 0x04, 0x00, 0x00};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        return false;
    }

    constexpr size_t stolen_size = sizeof(expected);
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, stolen_size + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, target, stolen_size);
    trampoline[stolen_size] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + stolen_size + 1) =
        static_cast<int32_t>((target + stolen_size) -
            (trampoline + stolen_size + 5));

    DWORD old_protection = 0;
    if (!VirtualProtect(target, stolen_size, PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(g_inventory_count_hook.original, target, stolen_size);
    g_original_label_set_text = reinterpret_cast<LabelSetTextFn>(trampoline);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookLabelSetText) - (target + 5));
    target[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(target, stolen_size, old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, stolen_size);

    g_inventory_count_hook.target = target;
    g_inventory_count_hook.trampoline = trampoline;
    g_inventory_count_hook.installed = true;
    return true;
}

bool UninstallInventoryCountHook() {
    if (!g_inventory_count_hook.installed) {
        return true;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(g_inventory_count_hook.target,
                        sizeof(g_inventory_count_hook.original),
                        PAGE_EXECUTE_READWRITE, &old_protection)) {
        return false;
    }
    std::memcpy(g_inventory_count_hook.target,
                g_inventory_count_hook.original,
                sizeof(g_inventory_count_hook.original));
    DWORD ignored = 0;
    VirtualProtect(g_inventory_count_hook.target,
                   sizeof(g_inventory_count_hook.original),
                   old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(),
                          g_inventory_count_hook.target,
                          sizeof(g_inventory_count_hook.original));
    VirtualFree(g_inventory_count_hook.trampoline, 0, MEM_RELEASE);
    g_inventory_count_hook = {};
    g_original_label_set_text = nullptr;
    return true;
}

}  // namespace

bool InstallFontHook() {
    if (g_hook_attempted) {
        return g_create_font_hook.slot != nullptr;
    }
    g_hook_attempted = true;
    const bool font_hook_installed = PatchImport(
        GetModuleHandleW(nullptr), "gdi32.dll", "CreateFontIndirectA",
        reinterpret_cast<void*>(&HookCreateFontIndirectA),
        g_create_font_hook);
    InstallInventoryCountHook();
    return font_hook_installed;
}

bool EnsureInventoryCountHook() {
    return InstallInventoryCountHook();
}

bool UninstallFontHook() {
    const bool inventory_restored = UninstallInventoryCountHook();
    return RestoreImport(g_create_font_hook) && inventory_restored;
}

}  // namespace stardom_font
