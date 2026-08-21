#include "d3d9_proxy_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace stardom {

using BinkOpenFn = void* (WINAPI*)(const char*, std::uint32_t);
using BinkCloseFn = void (WINAPI*)(void*);
using BinkBufferOpenFn = void* (WINAPI*)(HWND, std::uint32_t, std::uint32_t,
                                        std::uint32_t);
using BinkBufferCloseFn = void (WINAPI*)(void*);
using BinkBufferSetOffsetFn = int (WINAPI*)(void*, int, int);
using BinkBufferSetScaleFn = int (WINAPI*)(void*, std::uint32_t,
                                           std::uint32_t);

struct OpeningVideoState {
    BinkOpenFn original_open = nullptr;
    BinkCloseFn original_close = nullptr;
    BinkBufferOpenFn original_buffer_open = nullptr;
    BinkBufferCloseFn original_buffer_close = nullptr;
    BinkBufferSetOffsetFn original_set_offset = nullptr;
    BinkBufferSetScaleFn original_set_scale = nullptr;
    void* bink = nullptr;
    void* buffer = nullptr;
    HWND window = nullptr;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    ULONGLONG closed_tick = 0;
    bool attempted = false;
    bool installed = false;
};

OpeningVideoState g_opening_video;

bool IsOpeningTitleTransitionPending() {
    return g_opening_video.closed_tick != 0 &&
        GetTickCount64() - g_opening_video.closed_tick < 5000;
}

bool IsOpeningVideoPath(const char* path) {
    if (!path || !*path) {
        return false;
    }
    const char* name = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            name = cursor + 1;
        }
    }
    return _stricmp(name, "star.bik") == 0;
}

void GetOpeningVideoPlacement(HWND window, std::uint32_t source_width,
                              std::uint32_t source_height, int& x, int& y,
                              std::uint32_t& width, std::uint32_t& height) {
    x = 0;
    y = 0;
    width = source_width;
    height = source_height;
    if (!window || !source_width || !source_height) {
        return;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return;
    }
    const int output_width = client.right - client.left;
    const int output_height = client.bottom - client.top;
    if (output_width <= 0 || output_height <= 0) {
        return;
    }

    const std::uint64_t width_limited_height =
        static_cast<std::uint64_t>(output_width) * source_height / source_width;
    if (width_limited_height <= static_cast<std::uint64_t>(output_height)) {
        width = static_cast<std::uint32_t>(output_width);
        height = static_cast<std::uint32_t>(width_limited_height);
    } else {
        height = static_cast<std::uint32_t>(output_height);
        width = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(output_height) * source_width /
            source_height);
    }
    x = (output_width - static_cast<int>(width)) / 2;
    y = (output_height - static_cast<int>(height)) / 2;
}

void ApplyOpeningVideoPlacement() {
    if (!g_opening_video.buffer || !g_opening_video.original_set_scale ||
        !g_opening_video.original_set_offset) {
        return;
    }
    int x = 0;
    int y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GetOpeningVideoPlacement(
        g_opening_video.window, g_opening_video.source_width,
        g_opening_video.source_height, x, y, width, height);
    g_opening_video.original_set_scale(g_opening_video.buffer, width, height);
    g_opening_video.original_set_offset(g_opening_video.buffer, x, y);
    Log("Opening video Bink buffer: source=%ux%u output=%ux%u offset=%d,%d",
        g_opening_video.source_width, g_opening_video.source_height,
        width, height, x, y);
}

void* WINAPI HookBinkOpen(const char* path, std::uint32_t flags) {
    void* bink = g_opening_video.original_open(path, flags);
    if (bink && IsOpeningVideoPath(path)) {
        g_opening_video.bink = bink;
        g_opening_video.closed_tick = 0;
        Log("Opening video detected: %s handle=%p", path, bink);
    }
    return bink;
}

void WINAPI HookBinkClose(void* bink) {
    const bool opening_video = bink && bink == g_opening_video.bink;
    g_opening_video.original_close(bink);
    if (opening_video) {
        g_opening_video.bink = nullptr;
        g_opening_video.buffer = nullptr;
        g_opening_video.window = nullptr;
        g_opening_video.closed_tick = GetTickCount64();
        Log("Opening video closed");
    }
}

void* WINAPI HookBinkBufferOpen(HWND window, std::uint32_t width,
                                std::uint32_t height, std::uint32_t flags) {
    void* buffer = g_opening_video.original_buffer_open(
        window, width, height, flags);
    if (buffer && g_opening_video.bink && !g_opening_video.buffer) {
        g_opening_video.buffer = buffer;
        g_opening_video.window = window;
        g_opening_video.source_width = width;
        g_opening_video.source_height = height;
        ApplyOpeningVideoPlacement();
    }
    return buffer;
}

void WINAPI HookBinkBufferClose(void* buffer) {
    const bool opening_video = buffer && buffer == g_opening_video.buffer;
    g_opening_video.original_buffer_close(buffer);
    if (opening_video) {
        g_opening_video.buffer = nullptr;
        g_opening_video.window = nullptr;
    }
}

int WINAPI HookBinkBufferSetOffset(void* buffer, int x, int y) {
    if (buffer != g_opening_video.buffer) {
        return g_opening_video.original_set_offset(buffer, x, y);
    }
    int centered_x = 0;
    int centered_y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    GetOpeningVideoPlacement(
        g_opening_video.window, g_opening_video.source_width,
        g_opening_video.source_height, centered_x, centered_y, width, height);
    return g_opening_video.original_set_offset(
        buffer, centered_x, centered_y);
}

int WINAPI HookBinkBufferSetScale(void* buffer, std::uint32_t width,
                                  std::uint32_t height) {
    if (buffer != g_opening_video.buffer) {
        return g_opening_video.original_set_scale(buffer, width, height);
    }
    int x = 0;
    int y = 0;
    std::uint32_t scaled_width = 0;
    std::uint32_t scaled_height = 0;
    GetOpeningVideoPlacement(
        g_opening_video.window, g_opening_video.source_width,
        g_opening_video.source_height, x, y, scaled_width, scaled_height);
    return g_opening_video.original_set_scale(
        buffer, scaled_width, scaled_height);
}

bool PatchImport(HMODULE module, const char* imported_dll,
                 const char* imported_name, void* replacement,
                 void** original) {
    if (!module || !imported_dll || !imported_name || !replacement ||
        !original) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto& import_directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_directory.VirtualAddress) {
        return false;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + import_directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dll_name = reinterpret_cast<const char*>(
            base + descriptor->Name);
        if (_stricmp(dll_name, imported_dll) != 0 ||
            !descriptor->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++functions) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            imported_name) != 0) {
                continue;
            }
            DWORD old_protection = 0;
            if (!VirtualProtect(&functions->u1.Function,
                                sizeof(functions->u1.Function),
                                PAGE_READWRITE, &old_protection)) {
                return false;
            }
            *original = reinterpret_cast<void*>(functions->u1.Function);
            functions->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&functions->u1.Function,
                           sizeof(functions->u1.Function), old_protection,
                           &ignored);
            FlushInstructionCache(GetCurrentProcess(),
                                  &functions->u1.Function,
                                  sizeof(functions->u1.Function));
            return true;
        }
    }
    return false;
}

bool InstallOpeningVideoHooks() {
    if (g_opening_video.attempted) {
        return g_opening_video.installed;
    }
    g_opening_video.attempted = true;
    HMODULE executable = GetModuleHandleW(nullptr);
    bool ok = true;
    ok &= PatchImport(executable, "binkw32.dll", "_BinkOpen@8",
        reinterpret_cast<void*>(&HookBinkOpen),
        reinterpret_cast<void**>(&g_opening_video.original_open));
    ok &= PatchImport(executable, "binkw32.dll", "_BinkClose@4",
        reinterpret_cast<void*>(&HookBinkClose),
        reinterpret_cast<void**>(&g_opening_video.original_close));
    ok &= PatchImport(executable, "binkw32.dll", "_BinkBufferOpen@16",
        reinterpret_cast<void*>(&HookBinkBufferOpen),
        reinterpret_cast<void**>(&g_opening_video.original_buffer_open));
    ok &= PatchImport(executable, "binkw32.dll", "_BinkBufferClose@4",
        reinterpret_cast<void*>(&HookBinkBufferClose),
        reinterpret_cast<void**>(&g_opening_video.original_buffer_close));
    ok &= PatchImport(executable, "binkw32.dll", "_BinkBufferSetOffset@12",
        reinterpret_cast<void*>(&HookBinkBufferSetOffset),
        reinterpret_cast<void**>(&g_opening_video.original_set_offset));
    ok &= PatchImport(executable, "binkw32.dll", "_BinkBufferSetScale@12",
        reinterpret_cast<void*>(&HookBinkBufferSetScale),
        reinterpret_cast<void**>(&g_opening_video.original_set_scale));
    g_opening_video.installed = ok;
    return ok;
}

}  // namespace stardom
