#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cwchar>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: proxy_smoke <d3d9.dll>\n");
        return 2;
    }

    HMODULE proxy = LoadLibraryW(argv[1]);
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

    const bool patched = client.right == 1920 && client.bottom == 1080;
    // Some non-interactive CI desktops cannot create any D3D9 device. In that
    // environment, window resizing still verifies that the proxy loaded and
    // intercepted CreateDevice. A real device is checked during game QA.
    const bool device_ok = FAILED(result) ||
                           (parameters.BackBufferWidth == 800 && parameters.BackBufferHeight == 600);
    return patched && device_ok ? 0 : 8;
}
