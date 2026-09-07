#include "font_scale_patch.h"
#include <cstring>
#include <iterator>
#include <array>
#include <cmath>
#include <cstdint>

namespace stardom {
void Log(const char*, ...);
namespace {
thread_local int fitted_label_pixels = 0;
using GlyphDrawFn = DWORD(__thiscall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
GlyphDrawFn glyph_draw = nullptr;
void* ascii_resume = nullptr;
int __fastcall LabelAsciiAdvance(int native_width, int half_width) {
    return fitted_label_pixels > 0 ? half_width : native_width;
}
__declspec(naked) void LabelAsciiAdvanceHook() {
    __asm {
        push eax
        push edx
        mov edx, [esp+3Ch] // native half-cell metric, originally ESP+34h
        call LabelAsciiAdvance
        mov ecx, eax
        pop edx
        pop eax
        mov al, [ebp]
        mov [esp+48h], ecx
        jmp dword ptr [ascii_resume]
    }
}
float ReadFloat(const void* object, size_t offset) {
    float result;
    std::memcpy(&result, static_cast<const unsigned char*>(object) + offset, 4);
    return result;
}
DWORD __fastcall LabelGlyphDraw(void* self, void*, DWORD native_x, DWORD glyph,
        DWORD x_bits, DWORD y_bits, DWORD flags, DWORD advance) {
    if (fitted_label_pixels > 0 && (flags & 0x1800) == 0x1800) {
        float x, y;
        std::memcpy(&x, &x_bits, 4);
        std::memcpy(&y, &y_bits, 4);
        const float sx = ReadFloat(self, 0x24), sy = ReadFloat(self, 0x28);
        // The native glyph renderer clips its atlas-sized quad BEFORE scaling.
        // Skip that software crop only when the complete fitted character is
        // inside the real clip rectangle. Partially visible characters retain
        // the native clipping path. No atlas/cache dimensions are modified.
        if (sx > 0 && sx < 1 && sy > 0 && sy < 1 &&
            advance > 0 && advance <= static_cast<DWORD>(fitted_label_pixels) &&
            x >= ReadFloat(self, 0x1164) && y >= ReadFloat(self, 0x1168) &&
            x + advance <= ReadFloat(self, 0x116C) + 0.001f &&
            y + fitted_label_pixels <= ReadFloat(self, 0x1170) + 0.001f) {
            flags &= ~0x1800u;
        }
    }
    return glyph_draw(self, native_x, glyph, x_bits, y_bits, flags, advance);
}
constexpr size_t kAsciiSite = 0x16A031;
constexpr unsigned char kAsciiOriginal[] = {0x8A,0x45,0,0x89,0x4C,0x24,0x48};
constexpr size_t kGlyphSites[] = {
    0x1694D4,0x169605,0x1696B1,0x169778,0x169824,0x169E76,0x169FA0,0x16A0D0};
template<size_t N>
std::array<unsigned char,N> BranchBytes(const void* from, const void* to, bool call) {
    std::array<unsigned char,N> bytes;
    bytes.fill(0x90);
    bytes[0] = call ? 0xE8 : 0xE9;
    const int32_t relative = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) -
        (reinterpret_cast<uintptr_t>(from) + 5));
    std::memcpy(bytes.data()+1, &relative, 4);
    return bytes;
}
// IDIV turns fitted 14/19px text using the fallback 20px atlas into scale
// zero. Keep font slots, caches, draw arguments and clipping intact; change
// only the ratio calculation to floating point in both text callbacks.
constexpr unsigned char kUnclippedOriginal[] = {
    0x8B,0x46,0x0C,0x99,0xF7,0xF9,0x8B,0x56,0x24,0x52,0x6A,0,0x6A,0,
    0x89,0x44,0x24,0x1C,0xDB,0x44,0x24,0x1C,0xD9,0x5C,0x24,0x1C};
constexpr unsigned char kUnclippedReplacement[] = {
    0x51,                         // push ecx (atlas height)
    0xDB,0x46,0x0C,               // fild dword ptr [esi+0Ch] (requested height)
    0xDA,0x34,0x24,               // fidiv dword ptr [esp]
    0x59,                         // pop ecx
    0x8B,0x56,0x24,0x52,0x6A,0,0x6A,0, // original draw arguments
    0xD9,0x5C,0x24,0x1C,          // fstp dword ptr [esp+1Ch]
    0x90,0x90,0x90,0x90,0x90,0x90};
constexpr unsigned char kClippedOriginal[] = {
    0x8B,0x46,0x0C,0x99,0xF7,0xF9,0x8B,0x56,0x24,0x8B,0x0E,0x52,
    0x89,0x44,0x24,0x24,0xDB,0x44,0x24,0x24,0xD9,0x5C,0x24,0x24};
constexpr unsigned char kClippedReplacement[] = {
    0x51,0xDB,0x46,0x0C,0xDA,0x34,0x24,0x59,
    0x8B,0x56,0x24,0x8B,0x0E,0x52,0xD9,0x5C,0x24,0x24,
    0x90,0x90,0x90,0x90,0x90,0x90};
static_assert(sizeof(kUnclippedOriginal) == sizeof(kUnclippedReplacement));
static_assert(sizeof(kClippedOriginal) == sizeof(kClippedReplacement));
constexpr size_t kUnclippedSites[] = {
    0x10A449,0x10A4BC,0x10A505,0x10A54E,0x10A5A8,0x10A5F1,
    0x10A63A,0x10A68C,0x10A6F8,0x10A764,0x10A7D0,0x10A832};
constexpr size_t kClippedSites[] = {
    0x10A94B,0x10A9EA,0x10AA70,0x10AAFA,0x10AB99,0x10AC1F,
    0x10ACA9,0x10AD43,0x10ADDB,0x10AE73,0x10AF0B,0x10AF91};
constexpr size_t kAdvanceSite = 0x169392;
constexpr unsigned char kAdvanceOriginal[] = {
    0xD9,0x84,0x24,0xF0,0,0,0, // fld scale
    0xE8,0x56,0xDA,0x06,0,    // truncate scale before multiplying: loses fractions
    0x8B,0xF8,0x8B,0x4E,0x30,0x0F,0xAF,0xFD};

int __cdecl RoundAdvance(double pixels) {
    // The ratio has already been stored as a float. Round the final pixel
    // width so 20 * float(14/20) does not truncate to 13. The game uses this
    // width for full/half-width advances, wrapping, tabs and line bounds.
    return static_cast<int>(std::lround(pixels));
}
__declspec(naked) void RoundAdvanceFromX87() {
    __asm {
        sub esp, 8
        fstp qword ptr [esp]
        call RoundAdvance
        add esp, 8
        ret
    }
}
std::array<unsigned char, sizeof(kAdvanceOriginal)> AdvanceReplacement(
        const unsigned char* destination) {
    std::array<unsigned char, sizeof(kAdvanceOriginal)> result = {
        0xD9,0x84,0x24,0xF0,0,0,0, // fld scale
        0xDA,0x4E,0x08,             // fimul atlas width BEFORE conversion
        0xE8,0,0,0,0,              // round final pixel advance
        0x8B,0xF8,0x8B,0x4E,0x30}; // mov edi,eax; mov ecx,[esi+30h]
    const auto relative = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&RoundAdvanceFromX87) -
        (reinterpret_cast<uintptr_t>(destination) + 15));
    std::memcpy(result.data() + 11, &relative, sizeof(relative));
    return result;
}
template<size_t N, size_t S>
bool Matches(const unsigned char* base, const size_t (&sites)[N],
             const unsigned char (&bytes)[S]) {
    for (size_t site : sites)
        if (std::memcmp(base + site, bytes, S) != 0) return false;
    return true;
}
}
FittedLabelFontScope::FittedLabelFontScope(int pixels) : saved_(fitted_label_pixels) {
    fitted_label_pixels = pixels;
}
FittedLabelFontScope::~FittedLabelFontScope() { fitted_label_pixels = saved_; }
bool InstallFractionalFontScalePatch(HMODULE executable) {
    if (!executable) return false;
    auto* base = reinterpret_cast<unsigned char*>(executable);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.SizeOfImage < 0x16ABC6) return false;
    const auto advance = AdvanceReplacement(base + kAdvanceSite);
    const auto ascii = BranchBytes<7>(base + kAsciiSite, &LabelAsciiAdvanceHook, false);
    bool glyph_original = true, glyph_patched = true;
    for (size_t site : kGlyphSites) {
        const auto original = BranchBytes<5>(base + site, base + 0x16ABC0, true);
        const auto patched = BranchBytes<5>(base + site, &LabelGlyphDraw, true);
        glyph_original &= std::memcmp(base+site, original.data(), 5) == 0;
        glyph_patched &= std::memcmp(base+site, patched.data(), 5) == 0;
    }
    if (Matches(base, kUnclippedSites, kUnclippedReplacement) &&
        Matches(base, kClippedSites, kClippedReplacement) &&
        std::memcmp(base + kAdvanceSite, advance.data(), advance.size()) == 0 &&
        std::memcmp(base + kAsciiSite, ascii.data(), ascii.size()) == 0 && glyph_patched) return true;
    // Validate every branch before modifying any of them.
    if (!Matches(base, kUnclippedSites, kUnclippedOriginal) ||
        !Matches(base, kClippedSites, kClippedOriginal) ||
        std::memcmp(base + kAdvanceSite, kAdvanceOriginal, sizeof(kAdvanceOriginal)) != 0 ||
        std::memcmp(base+kAsciiSite, kAsciiOriginal, sizeof(kAsciiOriginal)) != 0 || !glyph_original) {
        Log("Fractional font scale signature mismatch");
        return false;
    }
    constexpr size_t start = kUnclippedSites[0];
    constexpr size_t length = kGlyphSites[std::size(kGlyphSites)-1] + 5 - start;
    DWORD protection = 0;
    if (!VirtualProtect(base + start, length, PAGE_EXECUTE_READWRITE, &protection))
        return false;
    for (size_t site : kUnclippedSites)
        std::memcpy(base + site, kUnclippedReplacement, sizeof(kUnclippedReplacement));
    for (size_t site : kClippedSites)
        std::memcpy(base + site, kClippedReplacement, sizeof(kClippedReplacement));
    std::memcpy(base + kAdvanceSite, advance.data(), advance.size());
    ascii_resume = base + kAsciiSite + sizeof(kAsciiOriginal);
    glyph_draw = reinterpret_cast<GlyphDrawFn>(base + 0x16ABC0);
    std::memcpy(base+kAsciiSite, ascii.data(), ascii.size());
    for (size_t site : kGlyphSites) {
        const auto patched = BranchBytes<5>(base + site, &LabelGlyphDraw, true);
        std::memcpy(base+site, patched.data(), patched.size());
    }
    DWORD ignored = 0;
    VirtualProtect(base + start, length, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), base + start, length);
    Log("Installed fractional text scaling, label character advances and fitted glyph clipping");
    return true;
}
}
