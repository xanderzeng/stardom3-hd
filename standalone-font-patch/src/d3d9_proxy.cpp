#include "font_hook.h"

#include <windows.h>
#include <d3d9.h>

#include <string>

namespace {

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
using Direct3DCreate9ExFn = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);

HMODULE g_system_d3d9 = nullptr;
Direct3DCreate9Fn g_create9 = nullptr;
Direct3DCreate9ExFn g_create9_ex = nullptr;

bool LoadSystemD3D9() {
    if (g_system_d3d9) {
        return g_create9 != nullptr;
    }
    wchar_t system_directory[MAX_PATH]{};
    if (!GetSystemDirectoryW(system_directory, MAX_PATH)) {
        return false;
    }
    const std::wstring path =
        std::wstring(system_directory) + L"\\d3d9.dll";
    g_system_d3d9 = LoadLibraryW(path.c_str());
    if (!g_system_d3d9) {
        return false;
    }
    g_create9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(g_system_d3d9, "Direct3DCreate9"));
    g_create9_ex = reinterpret_cast<Direct3DCreate9ExFn>(
        GetProcAddress(g_system_d3d9, "Direct3DCreate9Ex"));
    return g_create9 != nullptr;
}

}  // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk_version) {
    return LoadSystemD3D9() ? g_create9(sdk_version) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk_version,
                                             IDirect3D9Ex** d3d) {
    if (!d3d || !LoadSystemD3D9() || !g_create9_ex) {
        return D3DERR_NOTAVAILABLE;
    }
    return g_create9_ex(sdk_version, d3d);
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        stardom_font::InstallFontHook();
    } else if (reason == DLL_PROCESS_DETACH) {
        stardom_font::UninstallFontHook();
    }
    return TRUE;
}
