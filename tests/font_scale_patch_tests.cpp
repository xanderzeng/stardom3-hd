// Execute the actual x86 replacement instructions, including their original
// argument pushes and stack-local result, without requiring a game process.
#include "../src/font_scale_patch.cpp"
#include <cmath>
#include <cstdio>
#include <vector>
#include <fstream>
#include <float.h>
namespace stardom { void Log(const char*, ...) {} }
namespace {
int failures = 0;
void Check(bool ok, const char* message);
DWORD observed_flags;
DWORD __fastcall ObserveGlyph(void*, void*, DWORD native_x, DWORD glyph,
        DWORD x, DWORD y, DWORD flags, DWORD advance) {
    observed_flags = flags;
    return 123;
}
DWORD FloatBits(float value) { DWORD bits; std::memcpy(&bits, &value, 4); return bits; }
void TestLabelLayout() {
    using namespace stardom;
    Check(LabelAsciiAdvance(10, 7) == 10, "native ASCII width preserved outside fitted labels");
    {
        FittedLabelFontScope scope(14);
        Check(LabelAsciiAdvance(10, 7) == 7, "720p digits use measured half-cell width");
        // 10 yi 7295 wan 6393 yuan: 10 digits plus three full-width units.
        const int width = 10 * LabelAsciiAdvance(10, 7) + 3 * 14;
        Check(width == 112 && width <= 139,
            "complete mixed money string fits scaled 116px field");
        {
            FittedLabelFontScope nested(0);
            Check(LabelAsciiAdvance(10, 7) == 10, "nested native draw keeps original width");
        }
        Check(LabelAsciiAdvance(10, 7) == 7, "nested scope restores label metrics");
    }
    unsigned char font[0x1180]{};
    auto set = [&](size_t offset, float value) { std::memcpy(font+offset, &value, 4); };
    set(0x24, 0.7f); set(0x28, 0.7f);
    set(0x1164, 0.f); set(0x1168, 0.f);
    set(0x116C, 52.f); set(0x1170, 14.f);
    glyph_draw = reinterpret_cast<GlyphDrawFn>(&ObserveGlyph);
    auto draw = [&](float x, float y, DWORD advance) {
        return LabelGlyphDraw(font, nullptr, FloatBits(x), 0x1234,
            FloatBits(x), FloatBits(y), 0x1A00, advance);
    };
    Check(draw(24, 0, 14) == 123 && observed_flags == 0x1A00,
        "native glyph clipping and return value preserved");
    {
        FittedLabelFontScope scope(14);
        Check(draw(24, 0, 14) == 123 && observed_flags == 0x200,
            "14px tick label avoids atlas-size crop and retains outline flag");
        draw(45, 0, 7);
        Check(observed_flags == 0x200, "right-aligned last digit fits with half-cell advance");
        draw(46, 0, 7);
        Check(observed_flags == 0x1A00, "partially visible right-edge character stays clipped");
        draw(24, -1, 14);
        Check(observed_flags == 0x1A00, "partially visible top-edge character stays clipped");
        set(0x24, 1.f); set(0x28, 1.f);
        draw(24, 0, 14);
        Check(observed_flags == 0x1A00, "unit scale keeps native clipping");
    }
    glyph_draw = nullptr;
}
int __cdecl TruncateAdvance(double value) { return static_cast<int>(value); }
__declspec(naked) void TruncateFromX87() {
    __asm {
        sub esp, 8
        fstp qword ptr [esp]
        call TruncateAdvance
        add esp, 8
        ret
    }
}
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}
float Execute(const unsigned char* block, size_t length, bool clipped,
              int requested, int atlas) {
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 128,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!code) std::abort();
    std::memcpy(code, block, length);
    const unsigned char tail[] = {0x8B,0x44,0x24,
        static_cast<unsigned char>(clipped ? 0x24 : 0x1C),
        0x83,0xC4,static_cast<unsigned char>(clipped ? 4 : 12),0xC3};
    std::memcpy(code + length, tail, sizeof(tail));
    FlushInstructionCache(GetCurrentProcess(), code, length + sizeof(tail));
    int request[10]{};
    request[3] = requested;
    float result = 0;
    __asm {
        lea esi, request
        mov ecx, atlas
        sub esp, 64
        call code
        add esp, 64
        mov result, eax
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return result;
}
int ExecuteAdvance(float scale, int atlas, bool original = false) {
    using namespace stardom;
    auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 128,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!code) std::abort();
    auto block = AdvanceReplacement(code);
    if (original) {
        std::memcpy(block.data(), kAdvanceOriginal, sizeof(kAdvanceOriginal));
        const auto relative = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&TruncateFromX87) -
            (reinterpret_cast<uintptr_t>(code) + 12));
        std::memcpy(block.data() + 8, &relative, sizeof(relative));
    }
    std::memcpy(code, block.data(), block.size());
    const unsigned char tail[] = {0x8B,0xC7,0xC3}; // return actual EDI advance
    std::memcpy(code + block.size(), tail, sizeof(tail));
    FlushInstructionCache(GetCurrentProcess(), code, block.size() + sizeof(tail));
    int font[16]{};
    font[2] = atlas;
    int result;
    __asm {
        lea esi, font
        mov eax, scale
        mov edx, code
        sub esp, 256
        push ebp
        // Keep the scale at entry ESP+F0 after saving the native EBP.
        mov [esp+0ECh], eax
        mov ebp, atlas
        call edx
        pop ebp
        add esp, 256
        mov result, eax
    }
    VirtualFree(code, 0, MEM_RELEASE);
    return result;
}
}
int main(int argc, char** argv) {
    using namespace stardom;
    TestLabelLayout();
    unsigned int saved_control = 0;
    _controlfp_s(&saved_control, 0, 0);
    for (unsigned int rounding : {_RC_NEAR, _RC_CHOP}) {
      unsigned int ignored;
      _controlfp_s(&ignored, rounding, _MCW_RC);
    for (int requested : {12,14,15,16,19,20,22,24,29,30,38,40,58,72}) {
        for (int atlas : {12,16,18,20,30,40}) {
            const float expected = float(requested) / atlas;
            Check(std::abs(Execute(kUnclippedReplacement,
                sizeof(kUnclippedReplacement), false, requested, atlas) - expected) < 0.00001f,
                "unclipped fractional scale and stack balance");
            Check(std::abs(Execute(kClippedReplacement,
                sizeof(kClippedReplacement), true, requested, atlas) - expected) < 0.00001f,
                "clipped fractional scale and stack balance");
            for (bool clipped : {false, true}) {
                const float scale = clipped
                    ? Execute(kClippedReplacement, sizeof(kClippedReplacement), true, requested, atlas)
                    : Execute(kUnclippedReplacement, sizeof(kUnclippedReplacement), false, requested, atlas);
                Check(ExecuteAdvance(scale, atlas) == requested,
                    "full-width character advance matches measured font at all scales");
            }
        }
    }
    }
    unsigned int ignored;
    _controlfp_s(&ignored, saved_control, _MCW_RC);
    Check(ExecuteAdvance(0.7f, 20, true) == 0 &&
        ExecuteAdvance(0.95f, 20, true) == 0,
        "reproduce previous patch's zero character advance and overlapping Chinese text");
    Check(ExecuteAdvance(1.1f, 20, true) == 20 &&
        ExecuteAdvance(1.1f, 20) == 22, "1080p advance agrees with measured 22px font");
    Check(Execute(kClippedOriginal, sizeof(kClippedOriginal), true, 14, 20) == 0,
        "reproduce invisible 720p small text in original renderer");
    Check(Execute(kUnclippedOriginal, sizeof(kUnclippedOriginal), false, 19, 20) == 0,
        "reproduce invisible 720p normal text in original renderer");
    auto* base = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x170000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) return 2;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->OptionalHeader.SizeOfImage = 0x170000;
    std::memcpy(base + kAdvanceSite, kAdvanceOriginal, sizeof(kAdvanceOriginal));
    std::memcpy(base+kAsciiSite, kAsciiOriginal, sizeof(kAsciiOriginal));
    for (size_t site : kGlyphSites) {
        const auto call = BranchBytes<5>(base+site, base+0x16ABC0, true);
        std::memcpy(base+site, call.data(), call.size());
    }
    for (size_t site : kUnclippedSites)
        std::memcpy(base + site, kUnclippedOriginal, sizeof(kUnclippedOriginal));
    for (size_t site : kClippedSites)
        std::memcpy(base + site, kClippedOriginal, sizeof(kClippedOriginal));
    // Optional offline compatibility verification against an unpacked PE dump.
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.empty()) return 2;
        auto* header = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
        auto* pe = reinterpret_cast<IMAGE_NT_HEADERS*>(bytes.data() + header->e_lfanew);
        auto* section = IMAGE_FIRST_SECTION(pe);
        for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
            if (section[i].VirtualAddress <= kUnclippedSites[0] &&
                section[i].VirtualAddress + section[i].SizeOfRawData > 0x16A0D5) {
                const size_t offset = section[i].PointerToRawData +
                    kUnclippedSites[0] - section[i].VirtualAddress;
                constexpr size_t size = 0x16A0D5 - kUnclippedSites[0];
                if (offset + size > bytes.size()) return 2;
                std::memcpy(base + kUnclippedSites[0], bytes.data() + offset, size);
            }
        }
        Check(Matches(base, kUnclippedSites, kUnclippedOriginal) &&
            Matches(base, kClippedSites, kClippedOriginal) &&
            std::memcmp(base + kAdvanceSite, kAdvanceOriginal, sizeof(kAdvanceOriginal)) == 0,
            "all real engine ratio and character advance signatures");
    }
    auto module = reinterpret_cast<HMODULE>(base);
    base[kClippedSites[11]] ^= 1;
    Check(!InstallFractionalFontScalePatch(module), "reject mismatched last branch");
    Check(Matches(base, kUnclippedSites, kUnclippedOriginal), "no partial patch on mismatch");
    base[kClippedSites[11]] ^= 1;
    base[kAdvanceSite] ^= 1;
    Check(!InstallFractionalFontScalePatch(module), "reject mismatched downstream advance");
    Check(Matches(base, kUnclippedSites, kUnclippedOriginal), "no partial ratio patch on advance mismatch");
    base[kAdvanceSite] ^= 1;
    Check(InstallFractionalFontScalePatch(module), "install all branches");
    Check(InstallFractionalFontScalePatch(module), "idempotent installation");
    Check(Matches(base, kUnclippedSites, kUnclippedReplacement) &&
        Matches(base, kClippedSites, kClippedReplacement), "verify all installed instructions");
    VirtualFree(base, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}
