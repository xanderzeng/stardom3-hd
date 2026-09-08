#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cwchar>
#include <cstring>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

int g_key_menu_messages = 0;
int g_window_position_messages = 0;
IDirect3DDevice9* g_nested_reset_device = nullptr;
D3DPRESENT_PARAMETERS g_nested_reset_parameters{};
HRESULT g_nested_reset_result = S_OK;
int g_nested_reset_calls = 0;

HRESULT STDMETHODCALLTYPE SimulateDeviceLost(IDirect3DDevice9*) {
    return D3DERR_DEVICELOST;
}

bool ReplaceDeviceSlot(void** table, size_t index, void* replacement) {
    DWORD protection = 0;
    if (!VirtualProtect(table + index, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    table[index] = replacement;
    DWORD ignored = 0;
    return VirtualProtect(table + index, sizeof(void*), protection, &ignored) != FALSE;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_WINDOWPOSCHANGING) {
        ++g_window_position_messages;
        if (g_nested_reset_device) {
            auto* device = g_nested_reset_device;
            g_nested_reset_device = nullptr;
            ++g_nested_reset_calls;
            g_nested_reset_result = device->Reset(&g_nested_reset_parameters);
        }
    }
    if (message == WM_SYSCOMMAND &&
        (wparam & 0xFFF0u) == SC_KEYMENU) {
        ++g_key_menu_messages;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 4 && argc != 6 && argc != 7 && argc != 8) {
        std::fwprintf(stderr,
            L"usage: proxy_smoke <d3d9.dll> [expected-client-width expected-client-height]\n"
            L"       proxy_smoke <d3d9.dll> <config-width> <config-height> <expected-client-width> <expected-client-height> [borderless]\n"
            L"       use -1 -1 to require an unchanged client area\n");
        return 2;
    }

    const bool staged_config = argc >= 6;
    const int expected_index = staged_config ? 4 : 2;
    const long expected_width = argc >= 4 ?
        std::wcstol(argv[expected_index], nullptr, 10) : 1920;
    const long expected_height = argc >= 4 ?
        std::wcstol(argv[expected_index + 1], nullptr, 10) : 1080;
    const bool borderless = argc >= 7 && std::wcstol(argv[6], nullptr, 10) != 0;
    const bool lifecycle_test = argc == 8;
    const bool native_render = lifecycle_test && std::wcstol(argv[7], nullptr, 10) != 0;

    wchar_t staged_directory[MAX_PATH]{};
    wchar_t staged_dll[MAX_PATH]{};
    wchar_t staged_ini[MAX_PATH]{};
    wchar_t staged_log[MAX_PATH]{};
    const wchar_t* dll_path = argv[1];
    if (staged_config) {
        wchar_t temporary_root[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary_root)) {
            std::fwprintf(stderr, L"GetTempPath failed: %lu\n", GetLastError());
            return 9;
        }
        swprintf_s(staged_directory, L"%lsStardom3ProxySmoke-%lu",
                   temporary_root, GetCurrentProcessId());
        if (!CreateDirectoryW(staged_directory, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            std::fwprintf(stderr, L"CreateDirectory failed: %lu\n", GetLastError());
            return 10;
        }
        swprintf_s(staged_dll, L"%ls\\d3d9.dll", staged_directory);
        swprintf_s(staged_ini, L"%ls\\Stardom3.Widescreen.ini",
                   staged_directory);
        swprintf_s(staged_log, L"%ls\\Stardom3.Widescreen.log",
                   staged_directory);
        wchar_t source_dll[MAX_PATH]{};
        if (!GetFullPathNameW(argv[1], MAX_PATH, source_dll, nullptr) ||
            !CopyFileW(source_dll, staged_dll, FALSE)) {
            std::fwprintf(stderr, L"CopyFile failed: %lu\n", GetLastError());
            return 11;
        }
        WritePrivateProfileStringW(L"Widescreen", L"Enabled", L"1", staged_ini);
        if (std::wcscmp(argv[2], L"-") != 0) {
            WritePrivateProfileStringW(
                L"Widescreen", L"Width", argv[2], staged_ini);
        }
        if (std::wcscmp(argv[3], L"-") != 0) {
            WritePrivateProfileStringW(
                L"Widescreen", L"Height", argv[3], staged_ini);
        }
        WritePrivateProfileStringW(L"Widescreen", L"Borderless",
                                   borderless ? L"1" : L"0", staged_ini);
        WritePrivateProfileStringW(L"Widescreen", L"NativeRender",
                                   native_render ? L"1" : L"0", staged_ini);
        if (lifecycle_test) {
            WritePrivateProfileStringW(L"Widescreen", L"UIScaleMode", L"2", staged_ini);
            // This executable exercises the device hooks, not game EXE patches.
            WritePrivateProfileStringW(L"Widescreen", L"UnifiedUILayout", L"0", staged_ini);
        }
        dll_path = staged_dll;
    }

    HMODULE proxy = LoadLibraryW(dll_path);
    if (!proxy) {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }

    const auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(proxy, "Direct3DCreate9"));
    if (!create9) {
        std::fwprintf(stderr, L"Direct3DCreate9 export is missing\n");
        return 4;
    }

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"Stardom3WidescreenSmoke";
    if (!RegisterClassW(&window_class)) {
        std::fwprintf(stderr, L"RegisterClass failed: %lu\n", GetLastError());
        return 5;
    }

    HWND window = CreateWindowW(window_class.lpszClassName, L"smoke", WS_OVERLAPPEDWINDOW,
                                0, 0, 800, 600, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) {
        std::fwprintf(stderr, L"CreateWindow failed: %lu\n", GetLastError());
        return 6;
    }
    RECT initial_client{};
    GetClientRect(window, &initial_client);
    DEVMODEW desktop_before{};
    desktop_before.dmSize = sizeof(desktop_before);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop_before);

    IDirect3D9* d3d = create9(D3D_SDK_VERSION);
    if (!d3d) {
        std::fwprintf(stderr, L"Direct3DCreate9 returned null\n");
        return 7;
    }

    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth = 800;
    parameters.BackBufferHeight = 600;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    const D3DDEVTYPE device_type = lifecycle_test ? D3DDEVTYPE_HAL : D3DDEVTYPE_NULLREF;
    const HRESULT result = d3d->CreateDevice(D3DADAPTER_DEFAULT, device_type, window,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                             &parameters, &device);
    RECT client{};
    GetClientRect(window, &client);
    const LONG_PTR final_style = GetWindowLongPtrW(window, GWL_STYLE);
    SendMessageW(window, WM_SYSCOMMAND, SC_KEYMENU, 0);
    std::printf("result=0x%08lX backbuffer=%ux%u client=%ldx%ld\n",
                static_cast<unsigned long>(result), parameters.BackBufferWidth,
                parameters.BackBufferHeight, client.right, client.bottom);

    bool lifecycle_ok = true;
    if (lifecycle_test && device) {
        const UINT render_width = native_render ? expected_width : 800;
        const UINT render_height = native_render ? expected_height : 600;
        bool expect_fullscreen = false;
        auto verify = [&]() {
            IDirect3DSwapChain9* chain = nullptr;
            D3DPRESENT_PARAMETERS presentation{};
            if (FAILED(device->GetSwapChain(0, &chain))) return false;
            const HRESULT queried = chain->GetPresentParameters(&presentation);
            chain->Release();
            if (FAILED(queried) || !!presentation.Windowed == expect_fullscreen) return false;
            DEVMODEW desktop{};
            desktop.dmSize = sizeof(desktop);
            if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop)) return false;
            RECT current_client{};
            GetClientRect(window, &current_client);
            if (expect_fullscreen) {
                std::printf("exclusive display=%lux%lu client=%ldx%ld\n",
                    desktop.dmPelsWidth, desktop.dmPelsHeight, current_client.right, current_client.bottom);
                if (desktop.dmPelsWidth != render_width || desktop.dmPelsHeight != render_height ||
                    current_client.right != render_width || current_client.bottom != render_height) return false;
            } else if (desktop.dmPelsWidth != desktop_before.dmPelsWidth ||
                       desktop.dmPelsHeight != desktop_before.dmPelsHeight ||
                       (!borderless && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)) {
                return false;
            }
            IDirect3DSurface9* target = nullptr;
            if (FAILED(device->GetRenderTarget(0, &target))) return false;
            D3DSURFACE_DESC desc{};
            const HRESULT described = target->GetDesc(&desc);
            const HRESULT rebound = device->SetRenderTarget(0, target);
            target->Release();
            D3DVIEWPORT9 viewport{0, 0, 800, 600, 0.0f, 1.0f};
            const HRESULT set = device->SetViewport(&viewport);
            D3DVIEWPORT9 actual{};
            const HRESULT get = device->GetViewport(&actual);
            std::printf("verify: target=%ux%u viewport=%ux%u expected=%ux%u\n",
                desc.Width, desc.Height, actual.Width, actual.Height, render_width, render_height);
            return SUCCEEDED(described) && SUCCEEDED(rebound) && SUCCEEDED(set) &&
                SUCCEEDED(get) && desc.Width == render_width && desc.Height == render_height &&
                actual.Width == render_width && actual.Height == render_height &&
                SUCCEEDED(device->BeginScene()) && SUCCEEDED(device->EndScene());
        };
        lifecycle_ok = verify();
        // Model repeated power-off/lost-device responses without actually
        // turning off the user's monitor. No window changes or parameter
        // mutation are allowed, and the real device must remain usable.
        auto* table = *reinterpret_cast<void***>(device);
        void* original_cooperative = table[3];
        if (ReplaceDeviceSlot(table, 3, reinterpret_cast<void*>(&SimulateDeviceLost))) {
            const int positions_before = g_window_position_messages;
            auto unavailable = parameters;
            unavailable.Windowed = FALSE;
            const auto before = unavailable;
            for (int retry = 0; retry < 100; ++retry) {
                const HRESULT lost = device->Reset(&unavailable);
                lifecycle_ok = lifecycle_ok && lost == D3DERR_DEVICELOST &&
                    std::memcmp(&before, &unavailable, sizeof(before)) == 0;
            }
            const bool restored = ReplaceDeviceSlot(table, 3, original_cooperative);
            lifecycle_ok = restored && lifecycle_ok &&
                positions_before == g_window_position_messages && verify();
        } else {
            lifecycle_ok = false;
        }
        std::printf("lost-device retry isolation: %s\n", lifecycle_ok ? "PASS" : "FAIL");
        for (int cycle = 0; cycle < 3 && lifecycle_ok; ++cycle) {
            D3DPRESENT_PARAMETERS reset{};
            reset.BackBufferWidth = 800;
            reset.BackBufferHeight = 600;
            reset.BackBufferCount = 1;
            reset.BackBufferFormat = D3DFMT_R5G6B5;
            reset.SwapEffect = D3DSWAPEFFECT_FLIP;
            expect_fullscreen = cycle != 1;
            reset.Windowed = !expect_fullscreen;
            reset.FullScreen_RefreshRateInHz = 60;
            reset.hDeviceWindow = window;
            if (cycle == 0) {
                auto invalid = reset;
                IDirect3DSurface9* held_backbuffer = nullptr;
                lifecycle_ok = SUCCEEDED(device->GetBackBuffer(
                    0, 0, D3DBACKBUFFER_TYPE_MONO, &held_backbuffer));
                const HRESULT failed_reset = device->Reset(&invalid);
                std::printf("invalid reset=0x%08lX\n", static_cast<unsigned long>(failed_reset));
                if (held_backbuffer) held_backbuffer->Release();
                lifecycle_ok = lifecycle_ok && FAILED(failed_reset);
            }
            g_nested_reset_parameters = reset;
            g_nested_reset_device = device;
            const int nested_before = g_nested_reset_calls;
            const HRESULT reset_result = device->Reset(&reset);
            g_nested_reset_device = nullptr;
            std::printf("reset %d=0x%08lX\n", cycle, static_cast<unsigned long>(reset_result));
            lifecycle_ok = lifecycle_ok && SUCCEEDED(reset_result) &&
                g_nested_reset_calls == nested_before + 1 &&
                g_nested_reset_result == D3DERR_DEVICELOST && verify();
            if (!lifecycle_ok) break;
            const ULONG remaining_refs = device->Release();
            std::printf("release refs=%lu\n", remaining_refs);
            lifecycle_ok = remaining_refs == 0;
            device = nullptr;
            reset.Windowed = !expect_fullscreen;
            reset.BackBufferFormat = D3DFMT_R5G6B5;
            reset.FullScreen_RefreshRateInHz = 60;
            lifecycle_ok = lifecycle_ok && SUCCEEDED(d3d->CreateDevice(
                D3DADAPTER_DEFAULT, device_type, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &reset, &device)) && verify();
        }
        std::printf("lifecycle reset/recreate: %s\n", lifecycle_ok ? "PASS" : "FAIL");
    }
    if (device) {
        device->Release();
    }
    DEVMODEW desktop_after{};
    desktop_after.dmSize = sizeof(desktop_after);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop_after);
    lifecycle_ok = lifecycle_ok && desktop_after.dmPelsWidth == desktop_before.dmPelsWidth &&
        desktop_after.dmPelsHeight == desktop_before.dmPelsHeight;
    d3d->Release();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    FreeLibrary(proxy);

    if (staged_config) {
        DeleteFileW(staged_log);
        DeleteFileW(staged_ini);
        DeleteFileW(staged_dll);
        RemoveDirectoryW(staged_directory);
    }

    const bool expect_unchanged = expected_width < 0 && expected_height < 0;
    const bool patched = expect_unchanged ?
        (client.right == initial_client.right &&
         client.bottom == initial_client.bottom) :
        (client.right == expected_width && client.bottom == expected_height);
    // Some non-interactive CI desktops cannot create any D3D9 device. In that
    // environment, window resizing still verifies that the proxy loaded and
    // intercepted CreateDevice. A real device is checked during game QA.
    const bool device_ok = FAILED(result) || expect_unchanged ||
        (parameters.BackBufferWidth == (native_render ? expected_width : 800) &&
         parameters.BackBufferHeight == (native_render ? expected_height : 600));
    // When the proxy resizes the window it must also drop the sizing frame and
    // maximize box so Aero Snap cannot maximize the fixed-backbuffer window
    // (dragging the title bar to the top edge otherwise crashes the game).
    const bool style_ok = expect_unchanged ||
        (final_style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == 0;
    // Valid patched modes must suppress the standalone-Alt system menu command.
    // Invalid configurations leave the original window procedure untouched.
    const bool key_menu_ok = expect_unchanged ?
        g_key_menu_messages == 1 : g_key_menu_messages == 0;
    if (lifecycle_test && FAILED(result)) return 77;
    return patched && device_ok && style_ok && key_menu_ok && lifecycle_ok ? 0 : 8;
}
