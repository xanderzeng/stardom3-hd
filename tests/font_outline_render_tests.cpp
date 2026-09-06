// Include the implementation to exercise the private native scope and the
// production shader without adding test-only exports to the shipped DLL.
#include "../src/font_outline.cpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace stardom {
void Log(const char*, ...) {}
bool IsDebugModeEnabled() { return false; }
}

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}
void Require(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s: 0x%08lX\n", operation, hr);
        std::exit(2);
    }
}
constexpr int extent = 24;
constexpr int tex_size = 8;
unsigned char pixels[tex_size * tex_size];

DWORD __cdecl ObserveNativeScope() { return stardom::g_glyph ? 0x12345678 : 0; }

DWORD __fastcall ObserveContrast(void* self, void*, DWORD, DWORD, DWORD,
                                 DWORD, DWORD flags, DWORD) {
    const auto* p = static_cast<unsigned char*>(self);
    Check((flags & 0x200) && stardom::g_glyph, "inventory enables five-pass union");
    Check(p[0x78] == 0x30 && p[0x79] == 0x34 && p[0x7A] == 0x28 &&
          p[0x7B] == 128, "dark outline preserves source opacity");
    return 17;
}

void TestNativeBridge() {
    using namespace stardom;
    constexpr size_t size = 0x16B900;
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, size,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!image) std::exit(2);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->OptionalHeader.SizeOfImage = size;
    const unsigned char entry[] = {0x81,0xEC,0xB8,0,0,0, 0xE8,0,0,0,0,
        0x81,0xC4,0xB8,0,0,0,0xC2,0x18,0};
    const unsigned char end[] = {0x5F,0x5E,0x5B,0x81,0xC4,0xB8,0,0,0,0xC2,0x18,0};
    const unsigned char loop[] = {0xF7,0xC7,0,2,0,0,0x74,8,0xC7,0x44,0x24,0x40,5,0,0,0};
    auto* target = image + 0x16ABC0;
    std::memcpy(target, entry, sizeof(entry));
    const int32_t call = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&ObserveNativeScope) - (target + 11));
    std::memcpy(target + 7, &call, 4);
    std::memcpy(image + 0x16B828, end, sizeof(end));
    std::memcpy(image + 0x16ABF9, loop, sizeof(loop));
    Check(InstallFontOutlineHook(reinterpret_cast<HMODULE>(image)), "verified native signature installs");
    unsigned char font[0x200]{};
    std::memset(font + 0x78, 255, 16);
    const auto native = reinterpret_cast<NativeGlyphDraw>(target);
    GlyphDraw outer; g_glyph = &outer;
    Check(native(font, 1, 2, 3, 4, 0x200, 6) == 0x12345678,
        "x86 bridge selects fifth argument as flags and returns native value");
    Check(g_glyph == &outer, "nested native scope restored");
    font[0x58] = 0x08; font[0x59] = 0x6D; font[0x5A] = 0xB3;
    Check(native(font, 1, 2, 3, 4, 0x200, 6) == 0x12345678,
        "ranking blue foreground must not exclude light outline");
    Check(native(font, 1, 2, 3, 4, 0, 6) == 0, "plain glyph excluded");
    font[0x51] = 1;
    Check(native(font, 1, 2, 3, 4, 0x200, 6) == 0, "extra shadow excluded");
    font[0x51] = 0; font[0x78] = 0;
    Check(native(font, 1, 2, 3, 4, 0x200, 6) == 0, "dark outline excluded");
    g_glyph = nullptr;
    VirtualFree(reinterpret_cast<void*>(g_original), 0, MEM_RELEASE);
    g_original = nullptr;
    VirtualFree(image, 0, MEM_RELEASE);
    unsigned char before[sizeof(font)];
    for (unsigned i = 0; i < 4; ++i) {
        const DWORD body = 0x80D8DADA;
        std::memcpy(font + 0x58 + i * 4, &body, 4);
    }
    std::memcpy(before, font, sizeof(font));
    g_original = reinterpret_cast<NativeGlyphDraw>(&ObserveContrast);
    {
        InventoryTextContrast scope(true);
        Check(HookGlyphDraw(font, nullptr, 1, 2, 3, 4, 0, 6) == 17,
              "inventory preserves native return");
        Check(std::memcmp(before, font, sizeof(font)) == 0,
              "inventory restores entire shared font palette");
        { InventoryTextContrast nested(false);
          Check(!g_inventory_contrast, "unrelated nested text excluded"); }
        Check(g_inventory_contrast, "nested contrast scope restored");
    }
    Check(!g_inventory_contrast, "inventory contrast does not escape memo draw");
    for (unsigned i = 0; i < 4; ++i) {
        const DWORD body = 0x80FFFFFF;
        std::memcpy(font + 0x58 + i * 4, &body, 4);
    }
    std::memcpy(before, font, sizeof(font));
    {
        InventoryTextContrast save_label(true, true);
        Check(HookGlyphDraw(font, nullptr, 1, 2, 3, 4, 0x1A00, 6) == 17,
              "save label white text receives dark outline");
        Check(std::memcmp(before, font, sizeof(font)) == 0,
              "save label palette restored after drawing");
    }
    Check(!g_inventory_contrast && !g_light_text_contrast,
          "save contrast cannot leak to other pages");
    g_original = nullptr;
}

float Sample(float u, float v, bool linear) {
    const auto at = [](int x, int y) {
        x = std::clamp(x, 0, tex_size - 1);
        y = std::clamp(y, 0, tex_size - 1);
        return pixels[y * tex_size + x] / 255.f;
    };
    if (!linear) return at(int(std::floor(u * tex_size)), int(std::floor(v * tex_size)));
    const float x = u * tex_size - .5f, y = v * tex_size - .5f;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
           (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
}

void DrawCase(IDirect3DDevice9* d, IDirect3DSurface9* readback,
              bool linear, unsigned opacity, bool clipped, DWORD foreground,
              unsigned path, DWORD outline = 0xFFFFFF) {
    using namespace stardom;
    Require(d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF203040, 1, 0), "clear");
    Require(d->SetSamplerState(0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT), "min filter");
    Require(d->SetSamplerState(0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT), "mag filter");
    GlyphDraw context;
    g_glyph = &context;
    IDirect3DVertexBuffer9* buffer = nullptr;
    IDirect3DIndexBuffer9* indices = nullptr;
    if (path) {
        Require(d->CreateVertexBuffer(4 * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, D3DPOOL_DEFAULT, &buffer, nullptr), "dynamic vertex buffer");
        if (path == 2) {
            Require(d->CreateIndexBuffer(6 * sizeof(WORD), D3DUSAGE_WRITEONLY,
                D3DFMT_INDEX16, D3DPOOL_MANAGED, &indices, nullptr), "index buffer");
            void* data = nullptr;
            Require(indices->Lock(0, 0, &data, 0), "index lock");
            const WORD triangles[] = {0,1,2,0,2,3};
            std::memcpy(data, triangles, sizeof(triangles)); indices->Unlock();
            Require(d->SetIndices(indices), "set indices");
        }
    }
    const int offsets[5][2] = {{-1,0}, {0,-1}, {1,0}, {0,1}, {0,0}};
    float union_a[extent * extent]{};
    float expected[extent * extent][3];
    for (auto& p : expected) { p[0] = 32 / 255.f; p[1] = 48 / 255.f; p[2] = 64 / 255.f; }
    for (unsigned pass = 0; pass < 5; ++pass) {
        float left = 7.5f + offsets[pass][0], top = 7.5f + offsets[pass][1];
        const float right = left + 6, bottom = top + 6;
        float u = .125f, v = .125f;
        if (clipped) {
            const float new_left = std::max(left, 8.5f);
            u += (new_left - left) / 8.f; left = new_left;
        }
        const DWORD color = (opacity << 24) | (pass < 4 ? outline : foreground);
        Vertex vertices[4] = {{left,top,0,1,color,u,v}, {right,top,0,1,color,.875f,v},
            {right,bottom,0,1,color,.875f,.875f}, {left,bottom,0,1,color,u,.875f}};
        float saved[36];
        for (unsigned i = 0; i < 36; ++i) saved[i] = .03125f * (i + 1);
        Require(d->SetPixelShaderConstantF(0, saved, 9), "seed shader constants");
        context.native_vertices = vertices;
        if (path) {
            void* data = nullptr;
            Require(buffer->Lock(0, sizeof(vertices), &data, D3DLOCK_DISCARD), "vertex upload");
            std::memcpy(data, vertices, sizeof(vertices)); buffer->Unlock();
            Require(d->SetStreamSource(0, buffer, 0, sizeof(Vertex)), "stream source");
        }
        Require(d->BeginScene(), "begin");
        if (!path) {
            FontOutlineDraw scope(d, vertices, 4, sizeof(Vertex));
            Check(context.pass == pass + 1 && !context.disabled, "production shader was engaged");
            Require(d->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, vertices, sizeof(Vertex)), "draw");
        } else {
            FontOutlineDraw scope(d, 4);
            Check(context.pass == pass + 1 && !context.disabled, "streamed native glyph shader was engaged");
            Require(path == 1 ? d->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, 2) :
                d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2), "streamed draw");
        }
        Require(d->EndScene(), "end");
        IDirect3DPixelShader9* current = nullptr;
        Require(d->GetPixelShader(&current), "get restored shader");
        Check(!current, "pixel shader restored");
        if (current) current->Release();
        float restored[36]{};
        Require(d->GetPixelShaderConstantF(0, restored, 9), "get restored constants");
        Check(std::memcmp(saved, restored, sizeof(saved)) == 0, "shader constants restored");
        IDirect3DSurface9* rt = nullptr;
        Require(d->GetRenderTarget(0, &rt), "render target");
        Require(d->GetRenderTargetData(rt, readback), "read pixels"); rt->Release();
        D3DLOCKED_RECT lock{};
        Require(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY), "lock pixels");
        int max_error = 0;
        for (int y = 0; y < extent; ++y) for (int x = 0; x < extent; ++x) {
            const int index = y * extent + x;
            float a = 0;
            if (x >= left && x < right && y >= top && y < bottom)
                a = Sample(u + (x - left) / 8.f, v + (y - top) / 8.f, linear) * opacity / 255.f;
            if (pass < 4) {
                union_a[index] = std::max(union_a[index], a);
                for (int c = 0; c < 3; ++c)
                    expected[index][c] = float((outline >> ((2 - c) * 8)) & 255) / 255.f * union_a[index] + (32 + 16 * c) / 255.f * (1 - union_a[index]);
            } else {
                for (int c = 0; c < 3; ++c) {
                    const float body = float((foreground >> ((2 - c) * 8)) & 255) / 255.f;
                    expected[index][c] = body * a + expected[index][c] * (1 - a);
                }
            }
            const DWORD actual = reinterpret_cast<const DWORD*>(
                static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch)[x];
            for (int c = 0; c < 3; ++c) {
                const int channel = (actual >> ((2 - c) * 8)) & 255;
                max_error = std::max(max_error, std::abs(channel - int(std::lround(expected[index][c] * 255))));
            }
        }
        readback->UnlockRect();
        if (max_error > 4) std::fprintf(stderr, "linear=%d opacity=%u clipped=%d pass=%u error=%d\n", linear, opacity, clipped, pass, max_error);
        Check(max_error <= 4, "GPU output matches coverage-union reference, including foreground");
    }
    g_glyph = nullptr;
    if (buffer) { d->SetStreamSource(0, nullptr, 0, 0); buffer->Release(); }
    if (indices) { d->SetIndices(nullptr); indices->Release(); }
}
} // namespace

int main() {
    using namespace stardom;
    TestNativeBridge();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = instance; wc.lpszClassName = L"OutlineTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Outline test", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
    HMODULE runtime = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = runtime ? reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(
        GetProcAddress(runtime, "Direct3DCreate9")) : nullptr;
    IDirect3D9* api = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!api || !window) return 2;
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = extent; pp.BackBufferHeight = extent; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = window;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D16;
    IDirect3DDevice9* device = nullptr;
    Require(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device), "create D3D9 device");
    Require(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1), "FVF");
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DTexture9* texture = nullptr;
    Require(device->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr), "texture");
    D3DLOCKED_RECT lock{};
    Require(texture->LockRect(0, &lock, nullptr, 0), "texture lock");
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        const unsigned char values[] = {0, 32, 64, 128, 192, 255};
        const unsigned char a = pixels[y * 8 + x] = values[(x + y * 3) % 6];
        reinterpret_cast<DWORD*>(static_cast<unsigned char*>(lock.pBits) + y * lock.Pitch)[x] = DWORD(a) * 0x01010101u;
    }
    texture->UnlockRect(0);
    device->SetTexture(0, texture);
    IDirect3DSurface9* readback = nullptr;
    IDirect3DSurface9* initial = nullptr;
    Require(device->GetRenderTarget(0, &initial), "initial target");
    D3DSURFACE_DESC target_desc{};
    Require(initial->GetDesc(&target_desc), "target format"); initial->Release();
    Require(device->CreateOffscreenPlainSurface(extent, extent, target_desc.Format,
        D3DPOOL_SYSTEMMEM, &readback, nullptr), "readback surface");
    for (bool linear : {false, true}) for (unsigned opacity : {255u, 128u})
        for (bool clipped : {false, true}) for (DWORD body : {0u, 0x086DB3u})
            for (unsigned path : {0u, 1u, 2u})
                DrawCase(device, readback, linear, opacity, clipped, body, path);
    // RenderWare keeps depth testing on for 2D glyphs. With equal depth and
    // LESSEQUAL, coverage union must still work when depth writes are on.
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_ALPHAREF, 0);
    DrawCase(device, readback, true, 255, true, 0x086DB3, 2);
    for (unsigned opacity : {255u, 128u})
        DrawCase(device, readback, true, opacity, true, 0xDADAD8, 2, 0x303428);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    DrawCase(device, readback, true, 128, true, 0x086DB3, 2);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Unsupported geometry/state must leave the pipeline untouched.
    GlyphDraw rejected;
    g_glyph = &rejected;
    { FontOutlineDraw scope(device, nullptr, 4, 28); }
    Check(rejected.disabled && rejected.pass == 0, "invalid geometry falls back");
    Vertex rectangle[4] = {{0,0,0,1,0xFFFFFFFF,0,0}, {4,0,0,1,0xFFFFFFFF,1,0},
        {4,4,0,1,0xFFFFFFFF,1,1}, {0,4,0,1,0xFFFFFFFF,0,1}};
    rejected = {};
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
    { FontOutlineDraw scope(device, rectangle, 4, sizeof(Vertex)); }
    Check(rejected.disabled && rejected.pass == 0, "alpha threshold falls back");
    device->SetRenderState(D3DRS_ALPHAREF, 0);
    rejected = {};
    { FontOutlineDraw scope(device, rectangle, 4, sizeof(Vertex)); }
    Check(!rejected.disabled && rejected.pass == 1, "zero-only alpha test supports coverage union");
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    rejected = {}; rectangle[1].y = 1;
    { FontOutlineDraw scope(device, rectangle, 4, sizeof(Vertex)); }
    Check(rejected.disabled && rejected.pass == 0, "rotated geometry falls back");
    g_glyph = nullptr;
    Check(!InstallFontOutlineHook(GetModuleHandleW(nullptr)), "unknown executable is not patched");
    readback->Release();
    device->SetTexture(0, nullptr);
    texture->Release();
    Check(device->Release() == 0, "atlas-owned shader does not retain device after cleanup");
    api->Release(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName, instance);
    FreeLibrary(runtime);
    return failures ? 1 : 0;
}
