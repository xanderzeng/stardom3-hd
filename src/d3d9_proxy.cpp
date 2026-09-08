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
#include "font_outline.h"
#include "engine_display_mode.h"
#include "crash_diagnostics.h"
#include "native_resource_recovery.h"

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

constexpr wchar_t kOriginalWindowProcProperty[] =
    L"Stardom3.Widescreen.OriginalWindowProc";

LRESULT CALLBACK CompatibilityWindowProc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    const auto original = reinterpret_cast<WNDPROC>(
        GetPropW(window, kOriginalWindowProcProperty));

    // The original fullscreen game does not have a Windows menu interaction.
    // Once forced into windowed mode, DefWindowProc turns a standalone Alt key
    // press into SC_KEYMENU and enters a modal menu loop, which stops the game
    // from updating until another input dismisses it. Suppress only that system
    // command so Alt combinations and every other window command still work.
    if (message == WM_SYSCOMMAND &&
        (wparam & 0xFFF0u) == SC_KEYMENU) {
        return 0;
    }

    if (!original) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    const LRESULT result = CallWindowProcW(original, window, message,
                                           wparam, lparam);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, kOriginalWindowProcProperty);
    }
    return result;
}

bool InstallWindowCompatibilityHook(HWND window) {
    if (!window || !IsWindow(window)) {
        return false;
    }
    if (GetPropW(window, kOriginalWindowProcProperty)) {
        return true;
    }

    const auto original = reinterpret_cast<WNDPROC>(
        GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!original ||
        !SetPropW(window, kOriginalWindowProcProperty,
                  reinterpret_cast<HANDLE>(original))) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&CompatibilityWindowProc));
    if (!previous && GetLastError() != ERROR_SUCCESS) {
        RemovePropW(window, kOriginalWindowProcProperty);
        return false;
    }
    return true;
}


void ResizeClientArea(HWND window, UINT width, UINT height, bool borderless,
                      bool fullscreen) {
    if (!window || !IsWindow(window)) {
        return;
    }

    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    // Retain frame bits across fullscreen -> windowed resets. Other dynamic
    // bits (visibility, minimization, clipping) remain owned by Windows/game.
    constexpr wchar_t saved_style[] = L"Stardom3.Widescreen.FrameStyle";
    constexpr wchar_t saved_ex_style[] = L"Stardom3.Widescreen.FrameExStyle";
    constexpr DWORD frame_mask = WS_CAPTION | WS_BORDER | WS_DLGFRAME |
        WS_MINIMIZEBOX | WS_SYSMENU;
    constexpr DWORD ex_frame_mask = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
        WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
    if (!GetPropW(window, saved_style)) {
        SetPropW(window, saved_style,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>((style & frame_mask) + 1)));
        SetPropW(window, saved_ex_style,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>((ex_style & ex_frame_mask) + 1)));
    }
    if (!borderless && !fullscreen) {
        const auto saved = reinterpret_cast<ULONG_PTR>(GetPropW(window, saved_style));
        const auto saved_ex = reinterpret_cast<ULONG_PTR>(GetPropW(window, saved_ex_style));
        if (saved) style = (style & ~frame_mask) | static_cast<DWORD>(saved - 1);
        if (saved_ex) ex_style = (ex_style & ~ex_frame_mask) | static_cast<DWORD>(saved_ex - 1);
    }

    // The proxy always presents a fixed-size backbuffer, so the OS window has to
    // stay a fixed size. Windows Aero Snap otherwise maximizes a
    // resizable/maximizable window when its title bar is dragged to the top
    // screen edge; that resize desynchronizes the window from the fixed
    // backbuffer and crashes the game (for example a 1920x1080 window on a
    // 1920x1080 monitor). Drop the sizing frame and maximize box in every mode,
    // which also disables the snap-to-top gesture and border-drag resizing.
    style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    if (borderless || fullscreen) {
        // Additionally strip the caption/border decorations while keeping every
        // other style bit the game and D3D9 already rely on (WS_VISIBLE,
        // WS_CLIPSIBLINGS, WS_CLIPCHILDREN, the game's own flags, ...).
        // Replacing the whole style with WS_POPUP|WS_VISIBLE discarded those
        // bits on the live device window and made borderless startup fail.
        constexpr DWORD kFrameStyle = WS_CAPTION | WS_BORDER | WS_DLGFRAME |
                                      WS_MINIMIZEBOX | WS_SYSMENU;
        constexpr DWORD kFrameExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                        WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
        style &= ~kFrameStyle;
        ex_style &= ~kFrameExStyle;
    }

    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, ex_style);

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, style, GetMenu(window) != nullptr, ex_style);
    const int window_width = rect.right - rect.left;
    const int window_height = rect.bottom - rect.top;

    // Center the resized window inside the monitor's work area and clamp the top
    // edge so the title bar stays reachable. The old SWP_NOMOVE kept the game's
    // original small-window origin, which pushed the enlarged window off-screen
    // and forced the user to drag it toward the top edge (the crash trigger).
    int x = 0;
    int y = 0;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor && GetMonitorInfoW(monitor, &monitor_info)) {
        const RECT& work = fullscreen ? monitor_info.rcMonitor : monitor_info.rcWork;
        const int work_width = work.right - work.left;
        const int work_height = work.bottom - work.top;
        x = work.left + (fullscreen ? 0 : (work_width - window_width) / 2);
        y = work.top + (fullscreen ? 0 : (work_height - window_height) / 2);
        if (x < work.left) {
            x = work.left;
        }
        if (y < work.top) {
            y = work.top;
        }
    }
    SetWindowPos(window, nullptr, x, y, window_width, window_height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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

void NormalizePresentation(D3DPRESENT_PARAMETERS& parameters, const Config& config) {
    parameters.FullScreen_RefreshRateInHz = 0;
    // Preserve the game's mode choice. Exclusive fullscreen changes the display
    // mode to the backbuffer size; UNKNOWN is only legal for windowed output.
    parameters.BackBufferFormat = parameters.Windowed ? D3DFMT_UNKNOWN : D3DFMT_X8R8G8B8;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferWidth = config.native_render ? config.resolution.width : LegacyCanvas::width;
    parameters.BackBufferHeight = config.native_render ? config.resolution.height : LegacyCanvas::height;
}

bool SyncEngineDisplayMode(const Config& config) {
    if (!config.native_render) return false;
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    // These addresses belong to the supported Stardom3 executable, not D3D9.
    // The fullscreen camera obtains raster/picking dimensions from this mode
    // table, so it must agree with the configured render dimensions.
    if (reinterpret_cast<uintptr_t>(base) != 0x400000) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage <= 0x6194FC) return false;
    const unsigned char mode_query[] = {0x8B, 0x0D, 0xF8, 0x94, 0xA1, 0x00};
    const unsigned char width_query[] = {0xA1, 0xA4, 0x08, 0xA2, 0x00, 0xC3};
    if (std::memcmp(base + 0x28F3E0, mode_query, sizeof(mode_query)) != 0 ||
        std::memcmp(base + 0x10E120, width_query, sizeof(width_query)) != 0) return false;
    const unsigned selected = *reinterpret_cast<unsigned*>(base + 0x6194C8);
    const unsigned count = *reinterpret_cast<unsigned*>(base + 0x6194F0);
    auto* modes = *reinterpret_cast<EngineDisplayMode**>(base + 0x6194F8);
    if (!modes || count == 0 || count > 4096 || selected >= count) return false;
    MEMORY_BASIC_INFORMATION region{};
    auto* entry = modes + selected;
    if (!VirtualQuery(entry, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        reinterpret_cast<uintptr_t>(entry) + sizeof(*entry) >
            reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) return false;
    const bool result = UpdateSelectedEngineDisplayMode(modes, count, selected,
        config.resolution.width, config.resolution.height);
    if (result) Log("Engine display mode synchronized: index=%u output=%dx%d",
        selected, config.resolution.width, config.resolution.height);
    return result;
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
        ConfigureCrashDiagnostics(config.debug_mode, g_module_dir.c_str());
        ConfigureRuntimeErrorDiagnostics(config.debug_mode, g_system_d3d9);
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
        NormalizePresentation(patched, config);

        HWND target_window = patched.hDeviceWindow ? patched.hDeviceWindow : focus_window;
        ResizeClientArea(target_window,
            patched.Windowed ? output_width : patched.BackBufferWidth,
            patched.Windowed ? output_height : patched.BackBufferHeight,
            config.borderless, !patched.Windowed);
        if (!InstallWindowCompatibilityHook(target_window)) {
            Log("Failed to install window compatibility hook (error=%lu)",
                GetLastError());
        }

        Log("CreateDevice: requested=%ux%u windowed=%d, patched backbuffer=%ux%u client=%ux%u native=%d borderless=%d debug=%d",
            original_width, original_height, parameters->Windowed,
            patched.BackBufferWidth, patched.BackBufferHeight,
            output_width, output_height, config.native_render,
            config.borderless, config.debug_mode);

        const HRESULT result = real_->CreateDevice(adapter, type, focus_window, flags, &patched, device);
        if (SUCCEEDED(result)) {
            InstallNativeResourceRecovery(GetModuleHandleW(nullptr));
            SyncEngineDisplayMode(config);
            if (device && *device &&
                !InstallDeviceLifecycleHooks(*device, config, target_window)) {
                Log("Failed to install device lifecycle hooks");
            }
            if (config.font_outline_union) {
                InstallFontOutlineHook(GetModuleHandleW(nullptr));
            }
            *parameters = patched;
            ResizeClientArea(target_window,
                patched.Windowed ? output_width : patched.BackBufferWidth,
                patched.Windowed ? output_height : patched.BackBufferHeight,
                config.borderless, !patched.Windowed);
            if (config.native_render && config.unified_ui_layout) {
                InstallUnifiedUILayoutHook(output_width, output_height,
                                           config.title_screen_mode);
                PatchActiveTagBounds(output_width, output_height);
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
                 config.gui_runtime_probe || config.unified_ui_layout ||
                 config.font_outline_union) && device && *device) {
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
    if (reason == DLL_PROCESS_DETACH) stardom::RemoveCrashDiagnostics();
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        stardom::g_module_dir = stardom::ModuleDirectory();
        stardom::g_attach_tick = GetTickCount64();
        stardom::InstallFontReplacementHook();
        stardom::InstallOpeningVideoHooks();
    }
    return TRUE;
}
