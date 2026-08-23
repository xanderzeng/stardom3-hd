#include <windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9_proxy_internal.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace stardom {

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

HMODULE g_system_d3d9 = nullptr;
Direct3DCreate9Fn g_create9 = nullptr;
Direct3DCreate9ExFn g_create9_ex = nullptr;
std::wstring g_module_dir;
ULONGLONG g_attach_tick = 0;
bool g_debug_mode = false;


std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring IniPath() {
    return g_module_dir + L"\\Stardom3.Widescreen.ini";
}

std::wstring LogPath() {
    return g_module_dir + L"\\Stardom3.Widescreen.log";
}

void Log(const char* format, ...) {
    FILE* file = nullptr;
    _wfopen_s(&file, LogPath().c_str(), L"a, ccs=UTF-8");
    if (!file) {
        return;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fwprintf(file, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);

    char message[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    wchar_t wide[1024]{};
    MultiByteToWideChar(CP_UTF8, 0, message, -1, wide, static_cast<int>(std::size(wide)));
    std::fwprintf(file, L"%ls\n", wide);
    std::fclose(file);
}

bool IsDebugModeEnabled() {
    return g_debug_mode;
}


void ResizeClientArea(HWND window, UINT width, UINT height, bool borderless) {
    if (!window || !IsWindow(window)) {
        return;
    }

    if (borderless) {
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(window, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return;
    }

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, style, GetMenu(window) != nullptr, ex_style);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool LoadSystemD3D9() {
    if (g_system_d3d9) {
        return true;
    }

    wchar_t system_dir[MAX_PATH]{};
    if (!GetSystemDirectoryW(system_dir, MAX_PATH)) {
        return false;
    }
    const std::wstring path = std::wstring(system_dir) + L"\\d3d9.dll";
    g_system_d3d9 = LoadLibraryW(path.c_str());
    if (!g_system_d3d9) {
        return false;
    }

    g_create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(g_system_d3d9, "Direct3DCreate9"));
    g_create9_ex = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(g_system_d3d9, "Direct3DCreate9Ex"));
    return g_create9 != nullptr;
}

class Direct3D9Proxy final : public IDirect3D9 {
public:
    explicit Direct3D9Proxy(IDirect3D9* real) : real_(real) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
            *object = static_cast<IDirect3D9*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, object);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = real_->Release();
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return real_->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* i) override { return real_->GetAdapterIdentifier(a, f, i); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return real_->GetAdapterModeCount(a, f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* o) override { return real_->EnumAdapterModes(a, f, m, o); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* m) override { return real_->GetAdapterDisplayMode(a, m); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL w) override { return real_->CheckDeviceType(a, t, af, bf, w); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf) override { return real_->CheckDeviceFormat(a, t, af, u, r, cf); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT sf, BOOL w, D3DMULTISAMPLE_TYPE mt, DWORD* q) override { return real_->CheckDeviceMultiSampleType(a, t, sf, w, mt, q); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override { return real_->CheckDepthStencilMatch(a, t, af, rf, df); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override { return real_->CheckDeviceFormatConversion(a, t, s, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override { return real_->GetDeviceCaps(a, t, c); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return real_->GetAdapterMonitor(a); }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus_window,
                                           DWORD flags, D3DPRESENT_PARAMETERS* parameters,
                                           IDirect3DDevice9** device) override {
        if (!parameters) {
            return D3DERR_INVALIDCALL;
        }

        const Config config = LoadConfig(IniPath());
        g_debug_mode = config.debug_mode;
        if (!config.enabled) {
            Log("Patch disabled; forwarding CreateDevice unchanged");
            return real_->CreateDevice(adapter, type, focus_window, flags, parameters, device);
        }
        if (!config.resolution_valid) {
            const std::string supported = SupportedResolutionList();
            Log("Unsupported resolution configuration: Width=%d Height=%d; patch disabled for this CreateDevice. Supported modes: %s",
                config.configured_width, config.configured_height,
                supported.c_str());
            return real_->CreateDevice(adapter, type, focus_window, flags,
                                       parameters, device);
        }

        const UINT output_width = static_cast<UINT>(config.resolution.width);
        const UINT output_height = static_cast<UINT>(config.resolution.height);

        D3DPRESENT_PARAMETERS patched = *parameters;
        const UINT original_width = patched.BackBufferWidth;
        const UINT original_height = patched.BackBufferHeight;
        patched.Windowed = TRUE;
        patched.FullScreen_RefreshRateInHz = 0;
        if (config.native_render) {
            patched.BackBufferWidth = output_width;
            patched.BackBufferHeight = output_height;
        } else {
            // Stardom3 passes 0x0 and lets D3D9 infer 800x600 from the original
            // client area. Once the client is enlarged that would accidentally
            // create a native-size backbuffer, so compatibility mode must pin
            // the game's logical render surface explicitly.
            patched.BackBufferWidth = LegacyCanvas::width;
            patched.BackBufferHeight = LegacyCanvas::height;
        }

        HWND target_window = patched.hDeviceWindow ? patched.hDeviceWindow : focus_window;
        ResizeClientArea(target_window, output_width, output_height, config.borderless);

        Log("CreateDevice: requested=%ux%u windowed=%d, patched backbuffer=%ux%u client=%ux%u native=%d borderless=%d debug=%d",
            original_width, original_height, parameters->Windowed,
            patched.BackBufferWidth, patched.BackBufferHeight,
            output_width, output_height, config.native_render,
            config.borderless, config.debug_mode);

        const HRESULT result = real_->CreateDevice(adapter, type, focus_window, flags, &patched, device);
        if (SUCCEEDED(result)) {
            *parameters = patched;
            ResizeClientArea(target_window, output_width, output_height, config.borderless);
            if (config.native_render && config.unified_ui_layout) {
                InstallUnifiedUILayoutHook(output_width, output_height,
                                           config.title_screen_mode);
                PatchMapLocationProjectionBounds(output_width, output_height);
                PatchAirportLocationLabelFilter();
                PatchMapLocationVisibilityGuards();
            }
            if (config.native_render && config.ui_scale_mode != 0 && device && *device) {
                InstallUIViewportHook(*device, output_width, output_height, config.ui_scale_mode);
            }
            if (config.native_render &&
                (config.ui_draw_diagnostics || config.suppress_transparent_ui ||
                 config.ui_container_probe || config.suppress_proxy_containers ||
                 config.gui_runtime_probe || config.unified_ui_layout) && device && *device) {
                InstallUIDrawHooks(*device, config.ui_draw_diagnostics,
                                   config.suppress_transparent_ui, config.ui_container_probe,
                                   config.suppress_proxy_containers, config.gui_runtime_probe);
            }
        }
        Log("CreateDevice result=0x%08lX", static_cast<unsigned long>(result));
        return result;
    }

private:
    IDirect3D9* real_;
};

}  // namespace stardom

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk_version) {
    if (!stardom::LoadSystemD3D9()) {
        return nullptr;
    }
    stardom::InstallOpeningVideoHooks();
    IDirect3D9* real = stardom::g_create9(sdk_version);
    if (!real) {
        return nullptr;
    }
    stardom::Log("Direct3DCreate9 intercepted (SDK %u)", sdk_version);
    return new stardom::Direct3D9Proxy(real);
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version, IDirect3D9Ex** d3d) {
    if (!d3d || !stardom::LoadSystemD3D9() || !stardom::g_create9_ex) {
        return D3DERR_NOTAVAILABLE;
    }
    stardom::InstallOpeningVideoHooks();
    // Stardom3 imports Direct3DCreate9, not the Ex interface. This export is
    // forwarded for compatibility with launchers and overlays.
    return stardom::g_create9_ex(sdk_version, d3d);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        stardom::g_module_dir = stardom::ModuleDirectory();
        stardom::g_attach_tick = GetTickCount64();
        stardom::InstallOpeningVideoHooks();
    }
    return TRUE;
}
