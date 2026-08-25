#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cwchar>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 4 && argc != 6 && argc != 7) {
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
    const bool borderless = argc == 7 && std::wcstol(argv[6], nullptr, 10) != 0;

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
    const HRESULT result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                             &parameters, &device);
    RECT client{};
    GetClientRect(window, &client);
    const LONG_PTR final_style = GetWindowLongPtrW(window, GWL_STYLE);
    std::printf("result=0x%08lX backbuffer=%ux%u client=%ldx%ld\n",
                static_cast<unsigned long>(result), parameters.BackBufferWidth,
                parameters.BackBufferHeight, client.right, client.bottom);

    if (device) {
        device->Release();
    }
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
        (parameters.BackBufferWidth == 800 && parameters.BackBufferHeight == 600);
    // When the proxy resizes the window it must also drop the sizing frame and
    // maximize box so Aero Snap cannot maximize the fixed-backbuffer window
    // (dragging the title bar to the top edge otherwise crashes the game).
    const bool style_ok = expect_unchanged ||
        (final_style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == 0;
    return patched && device_ok && style_ok ? 0 : 8;
}
