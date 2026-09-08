#include "font_outline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

alignas(DWORD)
#include "font_outline_shader.h"

namespace stardom {
// Supplied by each host; the shared renderer has no widescreen dependency.
void Log(const char*, ...);
bool IsDebugModeEnabled();
namespace {

struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
struct Quad {
    float x, y, u, v, u_max, v_max, du, dv;
    DWORD color;
};
struct GlyphDraw {
    const void* native_vertices = nullptr;
    unsigned pass = 0;
    bool disabled = false;
    Quad previous[4]{};
    IDirect3DBaseTexture9* texture = nullptr; // borrowed within the native call
};
thread_local GlyphDraw* g_glyph = nullptr;
thread_local bool g_inventory_contrast = false;
thread_local bool g_light_text_contrast = false;
thread_local bool g_brighten_text = false;
using NativeGlyphDraw = DWORD (__thiscall*)(void*, DWORD, DWORD, DWORD,
                                          DWORD, DWORD, DWORD);
NativeGlyphDraw g_original = nullptr;

DWORD __fastcall HookGlyphDraw(void* self, void*, DWORD glyph, DWORD a2,
                              DWORD a3, DWORD a4, DWORD flags, DWORD a6) {
    auto* colors = static_cast<unsigned char*>(self);
    DWORD cell = 0;
    std::memcpy(&cell, colors + 8, sizeof(cell));
    const bool small_outline = cell >= 8 && cell <= 16 && (flags & 0x200);
    const bool plain_text = (flags & 0x200) == 0;
    bool neutral_text = true;
    bool targeted_contrast = (g_inventory_contrast || small_outline) && colors[0x51] == 0;
    for (unsigned i = 0; i < 4; ++i) {
        neutral_text &= colors[0x58 + i * 4] == colors[0x59 + i * 4] &&
                        colors[0x59 + i * 4] == colors[0x5A + i * 4];
        // Native inventory description #DADAD8, stored as RGBA bytes.
        targeted_contrast &= (g_light_text_contrast || small_outline) ?
                    (colors[0x58 + i * 4] >= 192 &&
                     colors[0x59 + i * 4] >= 192 &&
                     colors[0x5A + i * 4] >= 192) :
                    (colors[0x58 + i * 4] == 0xDA &&
                    colors[0x59 + i * 4] == 0xDA &&
                    colors[0x5A + i * 4] == 0xD8);
    }
    // Generic outline applies only to black/white/gray. Keep the pre-existing
    // page-specific exceptions and authored colored outlines unchanged.
    const bool contrast = (plain_text && neutral_text) || targeted_contrast;
    unsigned char saved_outline[16];
    unsigned char saved_body[16];
    if (contrast) {
        std::memcpy(saved_outline, colors + 0x78, 16);
        std::memcpy(saved_body, colors + 0x58, 16);
        for (unsigned i = 0; i < 4; ++i) {
            // Plain text has no authored border. Pick a contrasting neutral
            // border from its foreground, retaining source color and opacity.
            const unsigned luminance = 77u * colors[0x58 + i * 4] +
                150u * colors[0x59 + i * 4] + 29u * colors[0x5A + i * 4];
            const bool light_border = plain_text && !g_brighten_text && luminance < 128u * 256u;
            colors[0x78 + i * 4] = light_border ? 0xF0 : 0x30;
            colors[0x79 + i * 4] = light_border ? 0xF0 : 0x34;
            colors[0x7A + i * 4] = light_border ? 0xF0 : 0x28;
            colors[0x7B + i * 4] = colors[0x5B + i * 4];
            if (g_brighten_text) {
                colors[0x58 + i * 4] = 255;
                colors[0x59 + i * 4] = 255;
                colors[0x5A + i * 4] = 255;
            }
        }
        flags |= 0x200;
    }
    GlyphDraw draw;
    // 0056B3C7 / 0056B4xx build four 28-byte RwIm2DVertex records here;
    // 0056B712 submits this array to the native indexed renderer.
    draw.native_vertices = static_cast<const unsigned char*>(self) + 0x10C;
    GlyphDraw* saved = g_glyph;
    // +51 enables an extra shadow draw before the outline. Leave that path
    // alone; otherwise the draw sequence would not be four borders + body.
    const bool eligible = (flags & 0x200) != 0 &&
        static_cast<const unsigned char*>(self)[0x51] == 0;
    const auto* palette = static_cast<const unsigned char*>(self);
    // Coverage union is color-independent, including colored small-font
    // borders. The old light-border filter left those on repeated blending.
    g_glyph = eligible ? &draw : nullptr;
    const DWORD result = g_original(self, glyph, a2, a3, a4, flags, a6);
    g_glyph = saved;
    if ((flags & 0x200) && IsDebugModeEnabled()) {
        // Keep the complete diagnostic operation inside DebugMode. The old
        // first-12-glyph limit was consumed by small text before large labels
        // were drawn, hiding which size/condition actually fell back.
        struct ReportKey {
            DWORD width, height, flags, body, outline;
            unsigned passes;
            bool shadow, eligible, fallback;
        };
        static ReportKey reported[48]{};
        static unsigned count = 0;
        if (count < 48) {
            ReportKey key{};
            std::memcpy(&key.width, palette + 8, sizeof(DWORD));
            std::memcpy(&key.height, palette + 12, sizeof(DWORD));
            std::memcpy(&key.body, palette + 0x58, sizeof(DWORD));
            std::memcpy(&key.outline, palette + 0x78, sizeof(DWORD));
            key.flags = flags;
            key.passes = draw.pass;
            key.shadow = palette[0x51] != 0;
            key.eligible = eligible;
            key.fallback = draw.disabled;
            const auto same = [&](const ReportKey& p) {
                return p.width == key.width && p.height == key.height &&
                    p.flags == key.flags && p.body == key.body &&
                    p.outline == key.outline && p.passes == key.passes &&
                    p.shadow == key.shadow && p.eligible == key.eligible &&
                    p.fallback == key.fallback;
            };
            if (std::none_of(reported, reported + count, same)) {
                reported[count++] = key;
                Log("Font outline debug: cell=%lux%lu eligible=%d shadow=%d bodyRGBA=%08lX outlineRGBA=%08lX passes=%u fallback=%d flags=%08lX",
                    key.width, key.height, key.eligible, key.shadow,
                    key.body, key.outline, key.passes, key.fallback, flags);
            }
        }
    }
    if (contrast) {
        std::memcpy(colors + 0x78, saved_outline, 16);
        std::memcpy(colors + 0x58, saved_body, 16);
    }
    return result;
}

bool Near(float a, float b) { return std::abs(a - b) <= 0.00001f; }

bool ReadQuad(const void* data, UINT count, UINT stride, Quad& q) {
    if (!data || count != 4 || stride != sizeof(Vertex)) return false;
    Vertex v[4];
    std::memcpy(v, data, sizeof(v));
    float min_x = v[0].x, max_x = v[0].x;
    float min_y = v[0].y, max_y = v[0].y;
    float min_u = v[0].u, max_u = v[0].u;
    float min_v = v[0].v, max_v = v[0].v;
    for (const auto& p : v) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            !std::isfinite(p.u) || !std::isfinite(p.v) ||
            !std::isfinite(p.rhw) || p.rhw <= 0 ||
            !Near(p.rhw, v[0].rhw) || !Near(p.z, v[0].z) ||
            p.color != v[0].color) return false;
        min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
        min_y = std::min(min_y, p.y); max_y = std::max(max_y, p.y);
        min_u = std::min(min_u, p.u); max_u = std::max(max_u, p.u);
        min_v = std::min(min_v, p.v); max_v = std::max(max_v, p.v);
    }
    if (max_x <= min_x || max_y <= min_y || max_u <= min_u ||
        max_v <= min_v) return false;
    unsigned corners = 0;
    for (const auto& p : v) {
        const bool right = Near(p.x, max_x), bottom = Near(p.y, max_y);
        if ((!right && !Near(p.x, min_x)) ||
            (!bottom && !Near(p.y, min_y)) ||
            !Near(p.u, right ? max_u : min_u) ||
            !Near(p.v, bottom ? max_v : min_v)) return false;
        corners |= 1u << (unsigned(right) + 2 * unsigned(bottom));
    }
    if (corners != 15) return false;
    q = {min_x, min_y, min_u, min_v, max_u, max_v,
         (max_u - min_u) / (max_x - min_x),
         (max_v - min_v) / (max_y - min_y), v[0].color};
    return true;
}

bool RenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE key, DWORD expected) {
    DWORD actual = 0;
    return SUCCEEDED(d->GetRenderState(key, &actual)) && actual == expected;
}
bool StageState(IDirect3DDevice9* d, DWORD stage,
                D3DTEXTURESTAGESTATETYPE key, DWORD expected) {
    DWORD actual = 0;
    return SUCCEEDED(d->GetTextureStageState(stage, key, &actual)) && actual == expected;
}

bool SupportedState(IDirect3DDevice9* d) {
    DWORD fvf = 0, srgb = 0;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    const HRESULT ps_hr = d->GetPixelShader(&ps);
    const HRESULT vs_hr = d->GetVertexShader(&vs);
    const bool fixed = SUCCEEDED(ps_hr) && SUCCEEDED(vs_hr) && !ps && !vs;
    if (ps) ps->Release();
    if (vs) vs->Release();
    return fixed && SUCCEEDED(d->GetFVF(&fvf)) &&
        fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) &&
        SUCCEEDED(d->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb)) && !srgb &&
        RenderState(d, D3DRS_ALPHABLENDENABLE, TRUE) &&
        RenderState(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA) &&
        RenderState(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA) &&
        RenderState(d, D3DRS_BLENDOP, D3DBLENDOP_ADD) &&
        (RenderState(d, D3DRS_ALPHATESTENABLE, FALSE) ||
         ((RenderState(d, D3DRS_ALPHAFUNC, D3DCMP_GREATER) ||
           RenderState(d, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL)) &&
          RenderState(d, D3DRS_ALPHAREF, 0))) &&
        RenderState(d, D3DRS_FOGENABLE, FALSE) &&
        // Native UI quads retain depth testing. At their common depth, a
        // read-only test, ALWAYS, or LESSEQUAL gives identical visibility to
        // all overlapping passes; LESS with depth writes would not.
        (RenderState(d, D3DRS_ZENABLE, D3DZB_FALSE) ||
         RenderState(d, D3DRS_ZWRITEENABLE, FALSE) ||
         RenderState(d, D3DRS_ZFUNC, D3DCMP_ALWAYS) ||
         RenderState(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL)) &&
        RenderState(d, D3DRS_STENCILENABLE, FALSE) &&
        StageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE) &&
        StageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE) &&
        StageState(d, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE) &&
        StageState(d, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE) &&
        StageState(d, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE) &&
        StageState(d, 0, D3DTSS_TEXCOORDINDEX, 0);
}

// The atlas owns its shader reference. No global device reference or separate
// glyph/texture cache, and destruction/reset follows the game's atlas lifetime.
constexpr GUID kOutlineShader = {0x0f9f6a88, 0x7fe2, 0x4ca2,
    {0x9b, 0xe5, 0x19, 0x6e, 0x93, 0x50, 0xa2, 0x61}};
IDirect3DPixelShader9* Shader(IDirect3DDevice9* d, IDirect3DBaseTexture9* t) {
    IDirect3DPixelShader9* shader = nullptr;
    DWORD size = sizeof(shader);
    if (SUCCEEDED(t->GetPrivateData(kOutlineShader, &shader, &size))) return shader;
    if (FAILED(d->CreatePixelShader(
            reinterpret_cast<const DWORD*>(g_font_outline_shader), &shader))) return nullptr;
    if (FAILED(t->SetPrivateData(kOutlineShader, shader, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) {
        shader->Release();
        return nullptr;
    }
    return shader;
}

} // namespace

InventoryTextContrast::InventoryTextContrast(bool enabled, bool light_text, bool brighten)
    : saved_(g_inventory_contrast), saved_light_(g_light_text_contrast),
      saved_brighten_(g_brighten_text) {
    g_inventory_contrast = enabled;
    g_light_text_contrast = light_text;
    g_brighten_text = brighten;
}
InventoryTextContrast::~InventoryTextContrast() {
    g_inventory_contrast = saved_;
    g_light_text_contrast = saved_light_;
    g_brighten_text = saved_brighten_;
}

FontOutlineDraw::FontOutlineDraw(IDirect3DDevice9* d, UINT count)
    : FontOutlineDraw(d, g_glyph ? g_glyph->native_vertices : nullptr,
                      count, sizeof(Vertex)) {}

FontOutlineDraw::FontOutlineDraw(IDirect3DDevice9* d, const void* vertices,
                               UINT count, UINT stride) {
    if (!g_glyph || g_glyph->disabled || g_glyph->pass >= 5) return;
    auto& context = *g_glyph;
    Quad q{};
    if (!ReadQuad(vertices, count, stride, q) || !SupportedState(d)) {
        if (IsDebugModeEnabled()) {
            static unsigned reports = 0;
            if (reports < 4) {
                ++reports;
                DWORD fvf = 0, alpha_test = 0, alpha_op = 0, z = 0;
                d->GetFVF(&fvf);
                d->GetRenderState(D3DRS_ALPHATESTENABLE, &alpha_test);
                d->GetRenderState(D3DRS_ZENABLE, &z);
                d->GetTextureStageState(0, D3DTSS_ALPHAOP, &alpha_op);
                Log("Font outline debug: unsupported quad/state count=%u stride=%u fvf=%08lX alphaTest=%lu alphaOp=%lu z=%lu",
                    count, stride, fvf, alpha_test, alpha_op, z);
                const D3DRENDERSTATETYPE states[] = {D3DRS_SRCBLEND, D3DRS_DESTBLEND,
                    D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
                    D3DRS_FOGENABLE, D3DRS_STENCILENABLE};
                DWORD values[8]{};
                for (unsigned i = 0; i < 8; ++i) d->GetRenderState(states[i], &values[i]);
                DWORD arg1 = 0, arg2 = 0;
                d->GetTextureStageState(0, D3DTSS_ALPHAARG1, &arg1);
                d->GetTextureStageState(0, D3DTSS_ALPHAARG2, &arg2);
                Log("Font outline debug: quad=%d src=%lu dst=%lu alphaRef=%lu alphaFunc=%lu zWrite=%lu zFunc=%lu fog=%lu stencil=%lu alphaArgs=%lu,%lu",
                    ReadQuad(vertices, count, stride, q), values[0], values[1], values[2],
                    values[3], values[4], values[5], values[6], values[7], arg1, arg2);
            }
        }
        context.disabled = true;
        return;
    }
    IDirect3DBaseTexture9* texture = nullptr;
    if (FAILED(d->GetTexture(0, &texture)) || !texture) {
        context.disabled = true;
        return;
    }
    const unsigned pass = context.pass;
    bool valid = texture->GetType() == D3DRTYPE_TEXTURE &&
                 (pass == 0 || context.texture == texture);
    float params[9][4]{};
    for (unsigned i = 0; i < pass && i < 4; ++i) {
        const Quad& p = context.previous[i];
        valid &= Near(p.du, q.du) && Near(p.dv, q.dv) &&
                 (pass == 4 || p.color == q.color);
        params[i][0] = (q.x - p.x) * q.du + p.u - q.u;
        params[i][1] = (q.y - p.y) * q.dv + p.v - q.v;
        params[i][2] = pass == 4 ? 0.f : 1.f;
        params[i][3] = float(p.color >> 24) / 255.f;
        params[4 + i][0] = p.u; params[4 + i][1] = p.v;
        params[4 + i][2] = p.u_max; params[4 + i][3] = p.v_max;
    }
    params[8][0] = pass == 4 ? 1.f : 0.f;
    IDirect3DPixelShader9* shader = valid ? Shader(d, texture) : nullptr;
    if (!shader || FAILED(d->GetPixelShaderConstantF(0, constants_, 9))) {
        if (shader) shader->Release();
        texture->Release();
        context.disabled = true;
        return;
    }
    const HRESULT constants = d->SetPixelShaderConstantF(0, &params[0][0], 9);
    const HRESULT set = SUCCEEDED(constants) ? d->SetPixelShader(shader) : constants;
    shader->Release();
    if (FAILED(set)) {
        d->SetPixelShaderConstantF(0, constants_, 9);
        context.disabled = true;
    } else {
        device_ = d;
        context.texture = texture;
        if (pass < 4) context.previous[pass] = q;
        ++context.pass;
    }
    texture->Release();
}

FontOutlineDraw::~FontOutlineDraw() {
    if (device_) {
        device_->SetPixelShader(nullptr);
        device_->SetPixelShaderConstantF(0, constants_, 9);
    }
}

void UninstallFontOutlineHook(HMODULE executable) {
    if (!g_original || !executable) return;
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x16ABC0;
    DWORD protection;
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(target, reinterpret_cast<void*>(g_original), 6);
    DWORD ignored;
    VirtualProtect(target, 6, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    VirtualFree(reinterpret_cast<void*>(g_original), 0, MEM_RELEASE);
    g_original = nullptr;
}

bool InstallFontOutlineHook(HMODULE executable) {
    if (g_original) return true;
    auto* base = reinterpret_cast<unsigned char*>(executable);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage < 0x16B834) return false;
    auto* target = base + 0x16ABC0;
    constexpr unsigned char entry[] = {0x81, 0xEC, 0xB8, 0, 0, 0};
    constexpr unsigned char ending[] = {0x5F, 0x5E, 0x5B, 0x81, 0xC4,
        0xB8, 0, 0, 0, 0xC2, 0x18, 0};
    constexpr unsigned char outline[] = {0xF7, 0xC7, 0, 2, 0, 0, 0x74,
        8, 0xC7, 0x44, 0x24, 0x40, 5, 0, 0, 0};
    if (std::memcmp(target, entry, sizeof(entry)) ||
        std::memcmp(base + 0x16B828, ending, sizeof(ending)) ||
        std::memcmp(base + 0x16ABF9, outline, sizeof(outline))) {
        Log("Font outline composition: native signature mismatch; unchanged");
        return false;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 11,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(trampoline, entry, 6);
    trampoline[6] = 0xE9;
    const auto back = static_cast<int32_t>((target + 6) - (trampoline + 11));
    std::memcpy(trampoline + 7, &back, 4);
    DWORD protection = 0;
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_original = reinterpret_cast<NativeGlyphDraw>(trampoline);
    const auto offset = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookGlyphDraw) - target - 5);
    target[0] = 0xE9;
    std::memcpy(target + 1, &offset, 4);
    target[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(target, 6, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 11);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    Log("Installed coverage-union font outline hook");
    return true;
}

} // namespace stardom
