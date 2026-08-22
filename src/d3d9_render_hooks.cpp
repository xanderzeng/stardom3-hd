#include "d3d9_proxy_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace stardom {

using SetViewportFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetRenderTargetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using BeginSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using DrawPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);

struct DeviceHookState {
    IDirect3DDevice9* device = nullptr;
    SetViewportFn original_set_viewport = nullptr;
    SetRenderTargetFn original_set_render_target = nullptr;
    BeginSceneFn original_begin_scene = nullptr;
    PresentFn original_present = nullptr;
    DrawPrimitiveFn original_draw_primitive = nullptr;
    DrawIndexedPrimitiveFn original_draw_indexed_primitive = nullptr;
    DrawPrimitiveUPFn original_draw_primitive_up = nullptr;
    DrawIndexedPrimitiveUPFn original_draw_indexed_primitive_up = nullptr;
    void** original_vtable = nullptr;
    UINT width = 0;
    UINT height = 0;
    bool title_pillarbox_cleared = false;
    bool title_pillarbox_logged = false;
    bool title_ready_before_draw = false;
    bool title_transition_mask_logged = false;
    UINT active_target_width = 0;
    UINT active_target_height = 0;
    IDirect3DSurface9* main_target_surface = nullptr;
    bool active_target_is_main = true;
    int ui_scale_mode = 0;
    volatile LONG logged_first_ui_viewport = 0;
    volatile LONG ui_draw_log_count = 0;
    volatile LONG suppressed_transparent_draws = 0;
    volatile LONG probed_container_draws = 0;
    bool suppress_transparent_ui = false;
    bool ui_draw_diagnostics = false;
    bool ui_container_probe = false;
    bool suppress_proxy_containers = false;
    bool gui_runtime_probe = false;
    volatile LONG gui_runtime_probe_done = 0;
};

bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
        base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool IsExecutableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

void LogProbeBytes(const unsigned char* address, size_t count) {
    char line[256]{};
    size_t used = 0;
    for (size_t i = 0; i < count && used + 4 < sizeof(line); ++i) {
        const int written = sprintf_s(
            line + used, sizeof(line) - used, "%02X ", address[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    Log("GUI xref bytes: %s", line);
}

std::vector<uintptr_t> FindBytesInModule(HMODULE module, const void* needle,
                                         size_t needle_size, bool executable_only,
                                         size_t max_results) {
    std::vector<uintptr_t> results;
    if (!module || !needle || needle_size == 0) {
        return results;
    }
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return results;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return results;
    }
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    uintptr_t cursor = base;
    while (cursor < end && results.size() < max_results) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))) {
            break;
        }
        const uintptr_t region_start = std::max(cursor, reinterpret_cast<uintptr_t>(info.BaseAddress));
        const uintptr_t queried_end = reinterpret_cast<uintptr_t>(info.BaseAddress) +
            static_cast<uintptr_t>(info.RegionSize);
        const uintptr_t region_end = std::min<uintptr_t>(end, queried_end);
        const bool usable = info.State == MEM_COMMIT && IsReadableProtection(info.Protect) &&
            (!executable_only || IsExecutableProtection(info.Protect));
        if (usable && region_end >= region_start + needle_size) {
            for (uintptr_t address = region_start;
                 address + needle_size <= region_end && results.size() < max_results; ++address) {
                if (std::memcmp(reinterpret_cast<const void*>(address), needle, needle_size) == 0) {
                    results.push_back(address);
                }
            }
        }
        if (region_end <= cursor) {
            break;
        }
        cursor = region_end;
    }
    return results;
}

void ProbeGUIStringsInModule(HMODULE module, const char* module_name) {
    if (!module) {
        return;
    }
    Log("GUI runtime probe module=%s base=%p", module_name, module);
    const char* targets[] = {"GuiRoot", "TodayDate", "GameMain", "GameShort"};
    for (const char* target : targets) {
        const auto strings = FindBytesInModule(
            module, target, std::strlen(target) + 1, false, 16);
        Log("GUI probe string=%s matches=%u", target, static_cast<unsigned>(strings.size()));
        for (uintptr_t string_address : strings) {
            Log("GUI string %s at=%p", target, reinterpret_cast<void*>(string_address));
            const uint32_t direct_value = static_cast<uint32_t>(string_address);
            const auto direct_xrefs = FindBytesInModule(
                module, &direct_value, sizeof(direct_value), true, 24);
            for (uintptr_t xref : direct_xrefs) {
                Log("GUI direct xref %s code=%p", target, reinterpret_cast<void*>(xref));
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<void*>(xref), &info, sizeof(info))) {
                    const uintptr_t region_start = reinterpret_cast<uintptr_t>(info.BaseAddress);
                    const uintptr_t region_end = region_start + info.RegionSize;
                    const uintptr_t dump_start = xref >= region_start + 8 ? xref - 8 : xref;
                    const size_t dump_size = static_cast<size_t>(std::min<uintptr_t>(32, region_end - dump_start));
                    LogProbeBytes(reinterpret_cast<const unsigned char*>(dump_start), dump_size);
                }
            }

            const auto pointer_slots = FindBytesInModule(
                module, &direct_value, sizeof(direct_value), false, 32);
            for (uintptr_t slot : pointer_slots) {
                if (std::find(direct_xrefs.begin(), direct_xrefs.end(), slot) != direct_xrefs.end()) {
                    continue;
                }
                const uint32_t slot_value = static_cast<uint32_t>(slot);
                const auto indirect_xrefs = FindBytesInModule(
                    module, &slot_value, sizeof(slot_value), true, 16);
                for (uintptr_t xref : indirect_xrefs) {
                    Log("GUI indirect xref %s slot=%p code=%p", target,
                        reinterpret_cast<void*>(slot), reinterpret_cast<void*>(xref));
                }
            }
        }
    }
}

void RunGUIRuntimeProbe() {
    Log("GUI runtime probe begin");
    ProbeGUIStringsInModule(GetModuleHandleW(nullptr), "Stardom3.exe");
    ProbeGUIStringsInModule(GetModuleHandleW(L"Stardom3.dll"), "Stardom3.dll");
    Log("GUI runtime probe end");
}

DeviceHookState g_device_hook;

void UpdatePhotoAlbumViewport(IDirect3DDevice9* device) {
    if (!device || device != g_device_hook.device ||
        !g_device_hook.original_set_viewport ||
        !g_device_hook.active_target_is_main) {
        return;
    }
    const bool album_visible = IsPhotoAlbumViewportNeeded();
    D3DVIEWPORT9 current{};
    if (FAILED(device->GetViewport(&current))) {
        return;
    }

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    if (album_visible) {
        const bool differs = current.X != static_cast<DWORD>(viewport_x) ||
            current.Y != static_cast<DWORD>(viewport_y) ||
            current.Width != static_cast<DWORD>(viewport_width) ||
            current.Height != static_cast<DWORD>(viewport_height);
        if (differs) {
            D3DVIEWPORT9 desired = current;
            desired.X = static_cast<DWORD>(viewport_x);
            desired.Y = static_cast<DWORD>(viewport_y);
            desired.Width = static_cast<DWORD>(viewport_width);
            desired.Height = static_cast<DWORD>(viewport_height);
            g_device_hook.original_set_viewport(device, &desired);
        }
        g_unified_ui.photo_album_viewport_active = true;
        if (!g_unified_ui.photo_album_viewport_logged) {
            g_unified_ui.photo_album_viewport_logged = true;
            Log("Photo album viewport aspect-fit: 0,0 800x600 -> %d,%d %dx%d",
                viewport_x, viewport_y, viewport_width, viewport_height);
        }
    } else if (g_unified_ui.photo_album_viewport_active) {
        // UIScaleMode=2 normally maps the legacy main viewport to the complete
        // output. Restore that state when leaving the album.
        D3DVIEWPORT9 restored = current;
        restored.X = 0;
        restored.Y = 0;
        restored.Width = g_device_hook.width;
        restored.Height = g_device_hook.height;
        g_device_hook.original_set_viewport(device, &restored);
        g_unified_ui.photo_album_viewport_active = false;
        Log("Photo album viewport restored to %ux%u",
            g_device_hook.width, g_device_hook.height);
    }
}

void ClearScheduleHighlightCache() {
    g_unified_ui.schedule_root = nullptr;
    std::fill(std::begin(g_unified_ui.schedule_highlight_rows),
              std::end(g_unified_ui.schedule_highlight_rows), nullptr);
    g_unified_ui.last_schedule_hover_row = -2;
}

bool FindScheduleHighlightRows() {
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        ClearScheduleHighlightCache();
        return false;
    }

    auto* primary_bytes =
        static_cast<unsigned char*>(g_unified_ui.primary_root);
    void* page = *reinterpret_cast<void**>(primary_bytes + 0xF4);
    size_t page_count = 0;
    while (CanReadGuiObject(page) && page_count++ < 4096) {
        auto* page_bytes = static_cast<unsigned char*>(page);
        const int page_width = *reinterpret_cast<int*>(page_bytes + 0x88);
        const int page_height = *reinterpret_cast<int*>(page_bytes + 0x8C);
        const bool page_visible = *(page_bytes + 0x99) != 0;
        void* inner = *reinterpret_cast<void**>(page_bytes + 0xF4);
        if (page_visible && page_width >= 798 && page_width <= 804 &&
            page_height >= 598 && page_height <= 604 &&
            CanReadGuiObject(inner)) {
            auto* inner_bytes = static_cast<unsigned char*>(inner);
            const int inner_width = *reinterpret_cast<int*>(inner_bytes + 0x88);
            const int inner_height = *reinterpret_cast<int*>(inner_bytes + 0x8C);
            if (inner_width >= 798 && inner_width <= 802 &&
                inner_height >= 598 && inner_height <= 602) {
                void* rows[7]{};
                size_t row_count = 0;
                void* child = *reinterpret_cast<void**>(inner_bytes + 0xF4);
                size_t child_count = 0;
                while (CanReadGuiObject(child) && child_count++ < 256) {
                    auto* child_bytes = static_cast<unsigned char*>(child);
                    const int child_x = *reinterpret_cast<int*>(child_bytes + 0x80);
                    const int child_y = *reinterpret_cast<int*>(child_bytes + 0x84);
                    const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
                    const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
                    if (child_x >= 29 && child_x <= 32 &&
                        child_y >= 232 && child_y <= 474 &&
                        ((child_y - 233) % 40 == 0) &&
                        child_width >= 680 && child_width <= 688 &&
                        child_height >= 38 && child_height <= 42 &&
                        row_count < std::size(rows)) {
                        rows[row_count++] = child;
                    }
                    child = *reinterpret_cast<void**>(child_bytes + 0xF8);
                }
                if (row_count == std::size(rows)) {
                    g_unified_ui.schedule_root = page;
                    std::copy(std::begin(rows), std::end(rows),
                              std::begin(g_unified_ui.schedule_highlight_rows));
                    Log("Unified UI schedule hover rows discovered page=%p", page);
                    return true;
                }
            }
        }
        page = *reinterpret_cast<void**>(page_bytes + 0xF8);
    }

    ClearScheduleHighlightCache();
    return false;
}

void UpdateScheduleDateHover(IDirect3DDevice9* device) {
    if (!device || !g_unified_ui.installed ||
        !CanReadGuiObject(g_unified_ui.primary_root)) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now == g_unified_ui.last_schedule_hover_tick) {
        return;
    }
    g_unified_ui.last_schedule_hover_tick = now;

    if (!CanReadGuiObject(g_unified_ui.schedule_root) &&
        !FindScheduleHighlightRows()) {
        return;
    }

    auto* page_bytes = static_cast<unsigned char*>(g_unified_ui.schedule_root);
    if (*(page_bytes + 0x99) == 0) {
        ClearScheduleHighlightCache();
        return;
    }

    D3DDEVICE_CREATION_PARAMETERS parameters{};
    device->GetCreationParameters(&parameters);
    HWND input_window = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (input_window) {
        GetWindowThreadProcessId(input_window, &foreground_pid);
    }
    if (foreground_pid != GetCurrentProcessId()) {
        input_window = parameters.hFocusWindow;
    }
    if (!input_window) {
        return;
    }
    POINT mouse{};
    if (!GetCursorPos(&mouse) ||
        !ScreenToClient(input_window, &mouse)) {
        return;
    }

    const int page_x = *reinterpret_cast<int*>(page_bytes + 0x80);
    const int page_y = *reinterpret_cast<int*>(page_bytes + 0x84);
    const int relative_x = mouse.x - page_x;
    const int relative_y = mouse.y - page_y;
    // The game treats the date and all four artist cells as one schedule row.
    // Highlight the date when the pointer is anywhere across that row, not
    // only when it is directly over the narrow date-number strip.
    const bool over_schedule_row = relative_x >= 25 && relative_x <= 714;

    int hovered_row = -1;
    for (void* row : g_unified_ui.schedule_highlight_rows) {
        if (!CanReadGuiObject(row)) {
            ClearScheduleHighlightCache();
            return;
        }
        auto* row_bytes = static_cast<unsigned char*>(row);
        const int row_y = *reinterpret_cast<int*>(row_bytes + 0x84);
        const bool hovered = over_schedule_row &&
            relative_y >= row_y && relative_y < row_y + 40;
        *(row_bytes + 0x99) = hovered ? 1 : 0;
        if (hovered) {
            hovered_row = (row_y - 233) / 40;
        }
    }
    if (hovered_row != g_unified_ui.last_schedule_hover_row) {
        g_unified_ui.last_schedule_hover_row = hovered_row;
        Log("Unified UI schedule hover row=%d mouse=%ld,%ld page=%d,%d",
            hovered_row, mouse.x, mouse.y, page_x, page_y);
    }
}

HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9* device) {
    // Apply schedule hover visibility after the game's per-frame input update
    // and before it traverses the GUI tree to build draw calls.
    UpdateScheduleDateHover(device);
    UpdatePhotoAlbumViewport(device);
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    if (device == g_device_hook.device &&
        g_device_hook.original_begin_scene) {
        g_device_hook.title_pillarbox_cleared = false;
        g_device_hook.title_ready_before_draw = IsTitleScreenVisible();
        return g_device_hook.original_begin_scene(device);
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device,
                                               DWORD index,
                                               IDirect3DSurface9* surface) {
    if (device != g_device_hook.device ||
        !g_device_hook.original_set_render_target) {
        return D3DERR_INVALIDCALL;
    }
    const HRESULT result = g_device_hook.original_set_render_target(
        device, index, surface);
    if (SUCCEEDED(result) && index == 0 && surface) {
        g_device_hook.active_target_is_main =
            surface == g_device_hook.main_target_surface;
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(surface->GetDesc(&desc))) {
            g_device_hook.active_target_width = desc.Width;
            g_device_hook.active_target_height = desc.Height;
        }
        if (g_device_hook.active_target_is_main &&
            (g_unified_ui.photo_album_viewport_active ||
             IsPhotoAlbumScreenVisible())) {
            UpdatePhotoAlbumViewport(device);
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    if (device != g_device_hook.device || !g_device_hook.original_set_viewport || !viewport) {
        return D3DERR_INVALIDCALL;
    }

    // Dynamic map billboards render into off-screen surfaces which can have
    // exactly the same dimensions as the back buffer. Comparing dimensions
    // therefore misclassifies those surfaces and expands their legacy
    // 800x600 viewport, leaving only a clipped strip in the billboard. Only
    // the actual back-buffer surface may receive the widescreen UI viewport.
    const bool main_target = g_device_hook.active_target_is_main;
    const bool announcement_preview_size =
        viewport->Width >= 350 && viewport->Width <= 370 &&
        viewport->Height >= 190 && viewport->Height <= 210;
    const bool announcement_preview_position =
        (viewport->X >= 20 && viewport->X <= 30 &&
         viewport->Y >= 15 && viewport->Y <= 25) ||
        (viewport->X >= 410 && viewport->X <= 420 &&
         viewport->Y >= 15 && viewport->Y <= 25) ||
        (viewport->X >= 20 && viewport->X <= 30 &&
         viewport->Y >= 245 && viewport->Y <= 255) ||
        (viewport->X >= 410 && viewport->X <= 420 &&
         viewport->Y >= 245 && viewport->Y <= 255) ||
        (viewport->X >= 215 && viewport->X <= 225 &&
         viewport->Y >= 245 && viewport->Y <= 255) ||
        // The single-artist result screen uses the same 360x200 preview,
        // but places it one row higher than the multi-artist layouts.
        (viewport->X >= 215 && viewport->X <= 225 &&
         viewport->Y >= 135 && viewport->Y <= 145) ||
        // The two-artist layout uses a diagonal pair of preview slots.
        (viewport->X >= 65 && viewport->X <= 75 &&
         viewport->Y >= 15 && viewport->Y <= 25) ||
        (viewport->X >= 365 && viewport->X <= 375 &&
         viewport->Y >= 245 && viewport->Y <= 255);
    if (main_target && announcement_preview_size &&
        announcement_preview_position) {
        D3DVIEWPORT9 shifted = *viewport;
        shifted.X += (g_device_hook.width - 800) / 2;
        shifted.Y += (g_device_hook.height - 600) / 2;
        return g_device_hook.original_set_viewport(device, &shifted);
    }

    const bool looks_like_original_ui = viewport->X == 0 && viewport->Y == 0 &&
        viewport->Width >= 790 && viewport->Width <= 810 &&
        viewport->Height >= 590 && viewport->Height <= 610;
    if (!looks_like_original_ui) {
        return g_device_hook.original_set_viewport(device, viewport);
    }

    if (!main_target) {
        return g_device_hook.original_set_viewport(device, viewport);
    }

    D3DVIEWPORT9 scaled = *viewport;
    // Result values and their rise/fall animation are emitted as transformed
    // sprites after the game selects its legacy 800x600 UI viewport. Expanding
    // that viewport scales their 200px card spacing to 480px at 1920x1080.
    // Re-evaluate the structural screen signature synchronously here so even
    // the first announcement frame uses the centered legacy viewport.
    g_unified_ui.announcement_active = IsAnnouncementScreenVisible();
    if (IsPhotoAlbumViewportNeeded()) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        scaled.X = static_cast<DWORD>(viewport_x);
        scaled.Y = static_cast<DWORD>(viewport_y);
        scaled.Width = static_cast<DWORD>(viewport_width);
        scaled.Height = static_cast<DWORD>(viewport_height);
        g_unified_ui.photo_album_viewport_active = true;
        if (!g_unified_ui.photo_album_viewport_logged) {
            g_unified_ui.photo_album_viewport_logged = true;
            Log("Photo album viewport aspect-fit: 0,0 800x600 -> %d,%d %dx%d",
                viewport_x, viewport_y, viewport_width, viewport_height);
        }
    } else if (g_unified_ui.announcement_active) {
        g_unified_ui.photo_album_viewport_active = false;
        scaled.X = (g_device_hook.width - 800) / 2;
        scaled.Y = (g_device_hook.height - 600) / 2;
        scaled.Width = 800;
        scaled.Height = 600;
    } else if (g_device_hook.ui_scale_mode == 2) {
        g_unified_ui.photo_album_viewport_active = false;
        scaled.X = 0;
        scaled.Y = 0;
        scaled.Width = g_device_hook.width;
        scaled.Height = g_device_hook.height;
    } else {
        const double source_aspect = static_cast<double>(viewport->Width) / viewport->Height;
        const double target_aspect = static_cast<double>(g_device_hook.width) / g_device_hook.height;
        if (target_aspect > source_aspect) {
            scaled.Height = g_device_hook.height;
            scaled.Width = static_cast<DWORD>(scaled.Height * source_aspect + 0.5);
            scaled.X = (g_device_hook.width - scaled.Width) / 2;
            scaled.Y = 0;
        } else {
            scaled.Width = g_device_hook.width;
            scaled.Height = static_cast<DWORD>(scaled.Width / source_aspect + 0.5);
            scaled.X = 0;
            scaled.Y = (g_device_hook.height - scaled.Height) / 2;
        }
    }

    if (InterlockedCompareExchange(&g_device_hook.logged_first_ui_viewport, 1, 0) == 0) {
        Log("UI viewport: %u,%u %ux%u -> %u,%u %ux%u (mode=%d)",
            viewport->X, viewport->Y, viewport->Width, viewport->Height,
            scaled.X, scaled.Y, scaled.Width, scaled.Height, g_device_hook.ui_scale_mode);
    }
    return g_device_hook.original_set_viewport(device, &scaled);
}

UINT PrimitiveVertexCount(D3DPRIMITIVETYPE type, UINT primitive_count) {
    switch (type) {
        case D3DPT_POINTLIST: return primitive_count;
        case D3DPT_LINELIST: return primitive_count * 2;
        case D3DPT_LINESTRIP: return primitive_count + 1;
        case D3DPT_TRIANGLELIST: return primitive_count * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return primitive_count + 2;
        default: return 0;
    }
}

bool IsPretransformedUI(IDirect3DDevice9* device, DWORD* fvf_out) {
    DWORD fvf = 0;
    if (FAILED(device->GetFVF(&fvf))) {
        return false;
    }
    if (fvf_out) {
        *fvf_out = fvf;
    }
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

bool TransformPhotoAlbumCGRawVertices(void* vertices, UINT vertex_count,
                                      UINT stride) {
    if (!vertices || vertex_count == 0 || stride < sizeof(float) * 4 ||
        !IsPhotoAlbumCGVisible()) {
        return false;
    }

    auto* bytes = static_cast<unsigned char*>(vertices);
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    for (UINT i = 0; i < vertex_count; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            bytes + static_cast<size_t>(i) * stride);
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    // Only late legacy-space overlays need conversion. Album content already
    // expressed in the 1440x1080 viewport must remain untouched.
    if (min_x < -2.0f || min_y < -2.0f ||
        max_x > 802.0f || max_y > 602.0f) {
        return false;
    }

    // Keep the caption layer at its authored 800x600 size. It is independent
    // from the enlarged CG image, so scaling its panel and glyphs makes the
    // typography look stretched. Center the complete legacy overlay canvas
    // instead, preserving every relative position and pixel dimension.
    const int overlay_x =
        (static_cast<int>(g_device_hook.width) - 800) / 2;
    const int overlay_y =
        (static_cast<int>(g_device_hook.height) - 600) / 2;
    for (UINT i = 0; i < vertex_count; ++i) {
        auto* position = reinterpret_cast<float*>(
            bytes + static_cast<size_t>(i) * stride);
        position[0] += static_cast<float>(overlay_x);
        position[1] += static_cast<float>(overlay_y);
    }

    static volatile LONG cg_draw_logs = 0;
    const LONG log_index = InterlockedIncrement(&cg_draw_logs);
    if (log_index <= 24) {
        Log("Photo album CG native overlay #%ld bounds=%.1f,%.1f..%.1f,%.1f offset=%d,%d",
            log_index, min_x, min_y, max_x, max_y,
            overlay_x, overlay_y);
    }
    return true;
}

bool DrawPhotoAlbumCGRawPrimitive(IDirect3DDevice9* device,
                                  D3DPRIMITIVETYPE type,
                                  UINT start_vertex,
                                  UINT primitive_count,
                                  HRESULT& result) {
    if (!g_device_hook.active_target_is_main ||
        !g_device_hook.original_draw_primitive_up ||
        !IsPretransformedUI(device, nullptr) ||
        !IsPhotoAlbumCGVisible()) {
        return false;
    }
    const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
    if (vertex_count == 0 || vertex_count > 16384) {
        return false;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < sizeof(float) * 4) {
        if (buffer) {
            buffer->Release();
        }
        return false;
    }
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(vertex_count) * stride;
    D3DVERTEXBUFFER_DESC desc{};
    void* source = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset),
                            static_cast<UINT>(byte_count), &source,
                            D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }

    std::vector<unsigned char> transformed(byte_count);
    std::memcpy(transformed.data(), source, byte_count);
    buffer->Unlock();
    if (!TransformPhotoAlbumCGRawVertices(
            transformed.data(), vertex_count, stride)) {
        buffer->Release();
        return false;
    }
    result = g_device_hook.original_draw_primitive_up(
        device, type, primitive_count, transformed.data(), stride);
    device->SetStreamSource(0, buffer, stream_offset, stride);
    buffer->Release();
    return true;
}

void LogUIDraw(IDirect3DDevice9* device, const char* method, D3DPRIMITIVETYPE type,
               UINT primitive_count, const void* vertices, UINT vertex_count, UINT stride) {
    DWORD fvf = 0;
    if (!IsPretransformedUI(device, &fvf)) {
        return;
    }

    const LONG index = InterlockedIncrement(&g_device_hook.ui_draw_log_count);
    constexpr LONG kMaxLogs = 160;
    if (index > kMaxLogs) {
        return;
    }

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    bool have_bounds = vertices && stride >= sizeof(float) * 4 && vertex_count > 0;
    if (have_bounds) {
        const auto* bytes = static_cast<const unsigned char*>(vertices);
        for (UINT i = 0; i < vertex_count; ++i) {
            const auto* position = reinterpret_cast<const float*>(bytes + static_cast<size_t>(i) * stride);
            min_x = std::min(min_x, position[0]);
            min_y = std::min(min_y, position[1]);
            max_x = std::max(max_x, position[0]);
            max_y = std::max(max_y, position[1]);
        }
    }

    IDirect3DBaseTexture9* texture = nullptr;
    device->GetTexture(0, &texture);
    if (have_bounds) {
        Log("UI draw #%ld %s type=%u primitives=%u vertices=%u stride=%u fvf=0x%08lX bounds=%.1f,%.1f..%.1f,%.1f texture=%p",
            index, method, static_cast<unsigned>(type), primitive_count, vertex_count, stride,
            static_cast<unsigned long>(fvf), min_x, min_y, max_x, max_y, texture);
    } else {
        Log("UI draw #%ld %s type=%u primitives=%u vertices=%u stride=%u fvf=0x%08lX bounds=unavailable texture=%p",
            index, method, static_cast<unsigned>(type), primitive_count, vertex_count, stride,
            static_cast<unsigned long>(fvf), texture);
    }
    if (texture) {
        texture->Release();
    }
}

void LogUIDrawFromStream(IDirect3DDevice9* device, const char* method,
                         D3DPRIMITIVETYPE type, UINT start_vertex, UINT primitive_count) {
    DWORD fvf = 0;
    if (!IsPretransformedUI(device, &fvf)) {
        return;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
    if (FAILED(device->GetStreamSource(0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < sizeof(float) * 4 || vertex_count == 0) {
        if (buffer) {
            buffer->Release();
        }
        LogUIDraw(device, method, type, primitive_count, nullptr, vertex_count, stride);
        return;
    }

    D3DVERTEXBUFFER_DESC desc{};
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(vertex_count) * stride;
    void* data = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset), static_cast<UINT>(byte_count),
                            &data, D3DLOCK_READONLY))) {
        LogUIDraw(device, method, type, primitive_count, nullptr, vertex_count, stride);
        buffer->Release();
        return;
    }

    LogUIDraw(device, method, type, primitive_count, data, vertex_count, stride);
    buffer->Unlock();
    buffer->Release();
}

bool IsFullyTransparentUIQuad(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                              UINT start_vertex, UINT primitive_count) {
    if ((!g_device_hook.suppress_transparent_ui && !g_device_hook.ui_container_probe &&
         !g_device_hook.suppress_proxy_containers) ||
        type != D3DPT_TRIANGLESTRIP ||
        primitive_count != 2) {
        return false;
    }
    DWORD fvf = 0;
    if (!IsPretransformedUI(device, &fvf) || (fvf & D3DFVF_DIFFUSE) == 0) {
        return false;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < 20) {
        if (buffer) {
            buffer->Release();
        }
        return false;
    }

    constexpr UINT kVertexCount = 4;
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(kVertexCount) * stride;
    D3DVERTEXBUFFER_DESC desc{};
    void* data = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset), static_cast<UINT>(byte_count),
                            &data, D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }

    bool transparent = true;
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    DWORD first_diffuse = 0;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (UINT i = 0; i < kVertexCount; ++i) {
        const auto* vertex = bytes + static_cast<size_t>(i) * stride;
        const auto* position = reinterpret_cast<const float*>(vertex);
        const DWORD diffuse = *reinterpret_cast<const DWORD*>(vertex + 16);
        if (i == 0) {
            first_diffuse = diffuse;
        }
        transparent &= (diffuse & 0xFF000000u) == 0;
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    buffer->Unlock();
    buffer->Release();

    const float width = max_x - min_x;
    const float height = max_y - min_y;
    const bool target_size =
        (std::abs(width - 320.0f) <= 2.0f && std::abs(height - 46.0f) <= 2.0f) ||
        (std::abs(width - 336.0f) <= 2.0f && std::abs(height - 35.0f) <= 2.0f);
    IDirect3DBaseTexture9* texture = nullptr;
    if (target_size) {
        device->GetTexture(0, &texture);
    }
    const bool proxy_container = target_size && texture == nullptr &&
        first_diffuse == 0xFFD6D3CEu;

    if (g_device_hook.ui_container_probe && target_size) {
        const LONG count = InterlockedIncrement(&g_device_hook.probed_container_draws);
        if (count <= 24) {
            Log("Container probe #%ld bounds=%.1f,%.1f..%.1f,%.1f size=%.1fx%.1f diffuse=0x%08lX texture=%p transparent=%d",
                count, min_x, min_y, max_x, max_y, width, height,
                static_cast<unsigned long>(first_diffuse), texture, transparent);
        }
    }

    if (g_device_hook.suppress_transparent_ui && transparent) {
        const LONG count = InterlockedIncrement(&g_device_hook.suppressed_transparent_draws);
        if (count <= 8) {
            Log("Suppressed transparent UI quad #%ld bounds=%.1f,%.1f..%.1f,%.1f",
                count, min_x, min_y, max_x, max_y);
        }
    }
    if (g_device_hook.suppress_proxy_containers && proxy_container) {
        const LONG count = InterlockedIncrement(&g_device_hook.suppressed_transparent_draws);
        if (count <= 8) {
            Log("Suppressed proxy container #%ld bounds=%.1f,%.1f..%.1f,%.1f",
                count, min_x, min_y, max_x, max_y);
        }
    }
    if (texture) {
        texture->Release();
    }
    return (g_device_hook.suppress_transparent_ui && transparent) ||
        (g_device_hook.suppress_proxy_containers && proxy_container);
}

bool NearlyEqual(float value, float expected, float tolerance = 1.0f) {
    return std::abs(value - expected) <= tolerance;
}

struct TitleScreenClipState {
    bool active = false;
    DWORD previous_enabled = FALSE;
    RECT previous_rect{};
};

TitleScreenClipState BeginTitleScreenClip(IDirect3DDevice9* device) {
    TitleScreenClipState state;
    if (!device || !g_device_hook.active_target_is_main ||
        (!IsTitleScreenVisible() && !IsTitleTutorialVisible())) {
        return state;
    }

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    RECT title_rect{
        viewport_x,
        viewport_y,
        viewport_x + viewport_width,
        viewport_y + viewport_height,
    };
    if (!g_device_hook.title_pillarbox_cleared) {
        D3DRECT bars[4]{};
        DWORD bar_count = 0;
        if (viewport_x > 0) {
            bars[bar_count++] = D3DRECT{
                0, 0, viewport_x, static_cast<LONG>(g_device_hook.height)};
        }
        if (viewport_x + viewport_width <
            static_cast<int>(g_device_hook.width)) {
            bars[bar_count++] = D3DRECT{
                viewport_x + viewport_width, 0,
                static_cast<LONG>(g_device_hook.width),
                static_cast<LONG>(g_device_hook.height)};
        }
        if (viewport_y > 0) {
            bars[bar_count++] = D3DRECT{
                viewport_x, 0, viewport_x + viewport_width, viewport_y};
        }
        if (viewport_y + viewport_height <
            static_cast<int>(g_device_hook.height)) {
            bars[bar_count++] = D3DRECT{
                viewport_x, viewport_y + viewport_height,
                viewport_x + viewport_width,
                static_cast<LONG>(g_device_hook.height)};
        }
        if (bar_count == 0 || SUCCEEDED(device->Clear(
                bar_count, bars, D3DCLEAR_TARGET,
                D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0))) {
            g_device_hook.title_pillarbox_cleared = true;
            if (!g_device_hook.title_pillarbox_logged) {
                Log("Title screen pillarbox cleared around %d,%d %dx%d",
                    viewport_x, viewport_y, viewport_width, viewport_height);
                g_device_hook.title_pillarbox_logged = true;
            }
        }
    }
    if (FAILED(device->GetRenderState(
            D3DRS_SCISSORTESTENABLE, &state.previous_enabled))) {
        return state;
    }
    if (FAILED(device->GetScissorRect(&state.previous_rect))) {
        state.previous_rect = RECT{
            0, 0,
            static_cast<LONG>(g_device_hook.width),
            static_cast<LONG>(g_device_hook.height),
        };
    }
    if (state.previous_enabled) {
        title_rect.left = std::max(title_rect.left, state.previous_rect.left);
        title_rect.top = std::max(title_rect.top, state.previous_rect.top);
        title_rect.right = std::min(title_rect.right, state.previous_rect.right);
        title_rect.bottom = std::min(title_rect.bottom, state.previous_rect.bottom);
    }
    if (title_rect.right <= title_rect.left ||
        title_rect.bottom <= title_rect.top ||
        FAILED(device->SetScissorRect(&title_rect)) ||
        FAILED(device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE))) {
        return state;
    }
    state.active = true;
    return state;
}

void EndTitleScreenClip(IDirect3DDevice9* device,
                        const TitleScreenClipState& state) {
    if (!device || !state.active) {
        return;
    }
    device->SetScissorRect(&state.previous_rect);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, state.previous_enabled);
}

bool ClearInGameCGPillarboxBeforePrimitive(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
    UINT start_vertex, UINT primitive_count) {
    if (!device || !g_device_hook.active_target_is_main ||
        !IsInGameCGVisible() || type != D3DPT_TRIANGLESTRIP ||
        primitive_count != 2 || !IsPretransformedUI(device, nullptr)) {
        return false;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < sizeof(float) * 4) {
        if (buffer) {
            buffer->Release();
        }
        return false;
    }
    constexpr UINT kVertexCount = 4;
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(kVertexCount) * stride;
    D3DVERTEXBUFFER_DESC desc{};
    void* data = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset),
                            static_cast<UINT>(byte_count), &data,
                            D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (UINT i = 0; i < kVertexCount; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            bytes + static_cast<size_t>(i) * stride);
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    buffer->Unlock();
    buffer->Release();

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    const bool cg_image_quad =
        NearlyEqual(min_x, static_cast<float>(viewport_x), 2.0f) &&
        NearlyEqual(min_y, static_cast<float>(viewport_y), 2.0f) &&
        NearlyEqual(max_x, static_cast<float>(viewport_x + viewport_width),
                    2.0f) &&
        NearlyEqual(max_y, static_cast<float>(viewport_y + viewport_height),
                    2.0f);
    if (!cg_image_quad || viewport_x <= 0) {
        return false;
    }

    IDirect3DBaseTexture9* texture = nullptr;
    device->GetTexture(0, &texture);
    const bool textured = texture != nullptr;
    if (texture) {
        texture->Release();
    }
    if (!textured) {
        return false;
    }

    const D3DRECT bars[2] = {
        {0, 0, viewport_x, static_cast<LONG>(g_device_hook.height)},
        {viewport_x + viewport_width, 0,
         static_cast<LONG>(g_device_hook.width),
         static_cast<LONG>(g_device_hook.height)},
    };
    const HRESULT result = device->Clear(
        2, bars, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    static volatile LONG pillarbox_logs = 0;
    const LONG count = InterlockedIncrement(&pillarbox_logs);
    if (count <= 4) {
        Log("Unified UI in-game CG pillarbox #%ld viewport=%d,%d %dx%d result=0x%08lX",
            count, viewport_x, viewport_y, viewport_width, viewport_height,
            static_cast<unsigned long>(result));
    }
    return SUCCEEDED(result);
}

bool IsAnnouncementLegacyQuad(float min_x, float min_y,
                              float max_x, float max_y) {
    const bool background = NearlyEqual(min_x, 0.0f) &&
        NearlyEqual(min_y, 0.0f) && NearlyEqual(max_x, 800.0f) &&
        NearlyEqual(max_y, 600.0f);
    const bool preview_left = NearlyEqual(min_x, 25.0f) &&
        NearlyEqual(min_y, 20.0f) && NearlyEqual(max_x, 385.0f) &&
        NearlyEqual(max_y, 220.0f);
    const bool preview_right = NearlyEqual(min_x, 415.0f) &&
        NearlyEqual(min_y, 20.0f) && NearlyEqual(max_x, 775.0f) &&
        NearlyEqual(max_y, 220.0f);
    const bool preview_bottom = NearlyEqual(min_x, 220.0f) &&
        NearlyEqual(min_y, 250.0f) && NearlyEqual(max_x, 580.0f) &&
        NearlyEqual(max_y, 450.0f);
    const bool preview_single = NearlyEqual(min_x, 220.0f) &&
        NearlyEqual(min_y, 140.0f) && NearlyEqual(max_x, 580.0f) &&
        NearlyEqual(max_y, 340.0f);
    const bool preview_pair_top = NearlyEqual(min_x, 70.0f) &&
        NearlyEqual(min_y, 20.0f) && NearlyEqual(max_x, 430.0f) &&
        NearlyEqual(max_y, 220.0f);
    const bool preview_pair_bottom = NearlyEqual(min_x, 370.0f) &&
        NearlyEqual(min_y, 250.0f) && NearlyEqual(max_x, 730.0f) &&
        NearlyEqual(max_y, 450.0f);
    const bool preview_bottom_left = NearlyEqual(min_x, 25.0f) &&
        NearlyEqual(min_y, 250.0f) && NearlyEqual(max_x, 385.0f) &&
        NearlyEqual(max_y, 450.0f);
    const bool preview_bottom_right = NearlyEqual(min_x, 415.0f) &&
        NearlyEqual(min_y, 250.0f) && NearlyEqual(max_x, 775.0f) &&
        NearlyEqual(max_y, 450.0f);
    return background || preview_left || preview_right || preview_bottom ||
        preview_single || preview_pair_top || preview_pair_bottom ||
        preview_bottom_left || preview_bottom_right;
}

bool DrawShiftedAnnouncementPrimitive(IDirect3DDevice9* device,
                                      D3DPRIMITIVETYPE type,
                                      UINT start_vertex,
                                      UINT primitive_count,
                                      HRESULT& result) {
    if (g_device_hook.active_target_width != g_device_hook.width ||
        g_device_hook.active_target_height != g_device_hook.height ||
        type != D3DPT_TRIANGLESTRIP || primitive_count != 2 ||
        !g_device_hook.original_draw_primitive_up ||
        !IsPretransformedUI(device, nullptr)) {
        return false;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < sizeof(float) * 4) {
        if (buffer) {
            buffer->Release();
        }
        return false;
    }

    constexpr UINT kVertexCount = 4;
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(kVertexCount) * stride;
    D3DVERTEXBUFFER_DESC desc{};
    void* data = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset),
                            static_cast<UINT>(byte_count), &data,
                            D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }

    std::vector<unsigned char> shifted(byte_count);
    std::memcpy(shifted.data(), data, byte_count);
    buffer->Unlock();

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    for (UINT i = 0; i < kVertexCount; ++i) {
        auto* position = reinterpret_cast<float*>(
            shifted.data() + static_cast<size_t>(i) * stride);
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    if (!IsAnnouncementLegacyQuad(min_x, min_y, max_x, max_y)) {
        buffer->Release();
        return false;
    }

    const bool full_legacy_canvas = NearlyEqual(min_x, 0.0f) &&
        NearlyEqual(min_y, 0.0f) && NearlyEqual(max_x, 800.0f) &&
        NearlyEqual(max_y, 600.0f);
    const bool album_snapshot = full_legacy_canvas &&
        IsPhotoAlbumViewportNeeded();
    g_unified_ui.announcement_active = IsAnnouncementScreenVisible();
    if (!album_snapshot && !g_unified_ui.announcement_active) {
        buffer->Release();
        return false;
    }

    float offset_x = 0.0f;
    float offset_y = 0.0f;
    if (album_snapshot) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        for (UINT i = 0; i < kVertexCount; ++i) {
            auto* position = reinterpret_cast<float*>(
                shifted.data() + static_cast<size_t>(i) * stride);
            position[0] = static_cast<float>(viewport_x) +
                position[0] * viewport_width / 800.0f;
            position[1] = static_cast<float>(viewport_y) +
                position[1] * viewport_height / 600.0f;
        }

        // The transition texture is a downsample of the complete output, so
        // it already contains the album's pillar bars. Cropping its UV range
        // to the live aspect-fit viewport removes those embedded bars before
        // the quad is expanded; otherwise the visible snapshot is 25% narrower
        // than the live album even though both quads have the same dimensions.
        DWORD fvf = 0;
        if (SUCCEEDED(device->GetFVF(&fvf))) {
            size_t texture_offset = sizeof(float) * 4;
            if (fvf & D3DFVF_PSIZE) {
                texture_offset += sizeof(float);
            }
            if (fvf & D3DFVF_DIFFUSE) {
                texture_offset += sizeof(DWORD);
            }
            if (fvf & D3DFVF_SPECULAR) {
                texture_offset += sizeof(DWORD);
            }
            const DWORD texture_count =
                (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
            if (texture_count >= 1 &&
                texture_offset + sizeof(float) * 2 <= stride) {
                float min_u = FLT_MAX;
                float max_u = -FLT_MAX;
                for (UINT i = 0; i < kVertexCount; ++i) {
                    const auto* uv = reinterpret_cast<const float*>(
                        shifted.data() + static_cast<size_t>(i) * stride +
                        texture_offset);
                    min_u = std::min(min_u, uv[0]);
                    max_u = std::max(max_u, uv[0]);
                }
                const float uv_range = max_u - min_u;
                const float crop_start = static_cast<float>(viewport_x) /
                    g_device_hook.width;
                const float crop_width = static_cast<float>(viewport_width) /
                    g_device_hook.width;
                if (uv_range > 0.0f) {
                    for (UINT i = 0; i < kVertexCount; ++i) {
                        auto* uv = reinterpret_cast<float*>(
                            shifted.data() + static_cast<size_t>(i) * stride +
                            texture_offset);
                        const float normalized_u =
                            (uv[0] - min_u) / uv_range;
                        uv[0] = min_u + uv_range *
                            (crop_start + normalized_u * crop_width);
                    }
                }
            }
        }
        offset_x = static_cast<float>(viewport_x);
        offset_y = static_cast<float>(viewport_y);
    } else {
        offset_x = static_cast<float>(
            (static_cast<int>(g_device_hook.width) - 800) / 2);
        offset_y = static_cast<float>(
            (static_cast<int>(g_device_hook.height) - 600) / 2);
        for (UINT i = 0; i < kVertexCount; ++i) {
            auto* position = reinterpret_cast<float*>(
                shifted.data() + static_cast<size_t>(i) * stride);
            position[0] += offset_x;
            position[1] += offset_y;
        }
    }

    result = g_device_hook.original_draw_primitive_up(
        device, type, primitive_count, shifted.data(), stride);
    // DrawPrimitiveUP clears stream 0 by contract; restore the game's stream
    // binding so subsequent GUI batches continue to use their shared buffer.
    device->SetStreamSource(0, buffer, stream_offset, stride);
    buffer->Release();

    static volatile LONG shifted_draws = 0;
    const LONG count = InterlockedIncrement(&shifted_draws);
    if (count <= 12) {
        Log("Corrected legacy draw #%ld kind=%s bounds=%.1f,%.1f..%.1f,%.1f offset=%.0f,%.0f uv-crop=%s",
            count, album_snapshot ? "photo-album-snapshot" : "announcement",
            min_x, min_y, max_x, max_y, offset_x, offset_y,
            album_snapshot ? "viewport" : "none");
    }
    return true;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                             UINT start_vertex, UINT primitive_count) {
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    UpdateScheduleDateHover(device);
    UpdatePhotoAlbumViewport(device);
    if (device == g_device_hook.device && g_device_hook.original_draw_primitive) {
        if (g_device_hook.gui_runtime_probe &&
            GetTickCount64() - g_attach_tick >= 12000 &&
            InterlockedCompareExchange(&g_device_hook.gui_runtime_probe_done, 1, 0) == 0) {
            RunGUIRuntimeProbe();
        }
        if (IsFullyTransparentUIQuad(device, type, start_vertex, primitive_count)) {
            return D3D_OK;
        }
        ClearInGameCGPillarboxBeforePrimitive(
            device, type, start_vertex, primitive_count);
        HRESULT shifted_result = D3D_OK;
        if (DrawPhotoAlbumCGRawPrimitive(device, type, start_vertex,
                                         primitive_count, shifted_result)) {
            return shifted_result;
        }
        if (DrawShiftedAnnouncementPrimitive(device, type, start_vertex,
                                             primitive_count, shifted_result)) {
            return shifted_result;
        }
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDrawFromStream(device, "DrawPrimitive", type, start_vertex, primitive_count);
        }
        const TitleScreenClipState title_clip = BeginTitleScreenClip(device);
        const HRESULT result = g_device_hook.original_draw_primitive(
            device, type, start_vertex, primitive_count);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source,
                                      const RECT* destination, HWND override_window,
                                      const RGNDATA* dirty_region) {
    RefreshUnifiedUILayout();
    UpdateScheduleDateHover(device);
    UpdatePhotoAlbumViewport(device);
    if (device == g_device_hook.device && g_device_hook.original_present) {
        const bool late_title_frame =
            g_unified_ui.title_screen_mode != 0 &&
            !g_device_hook.title_ready_before_draw &&
            (IsTitleScreenVisible() || IsOpeningTitleTransitionPending());
        if (late_title_frame) {
            IDirect3DSurface9* target = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target) {
                const HRESULT fill_result = device->ColorFill(
                    target, nullptr, D3DCOLOR_XRGB(0, 0, 0));
                target->Release();
                if (SUCCEEDED(fill_result) &&
                    !g_device_hook.title_transition_mask_logged) {
                    Log("Masked native title transition frame until aspect-fit layout was ready");
                    g_device_hook.title_transition_mask_logged = true;
                }
            }
        }
        return g_device_hook.original_present(
            device, source, destination, override_window, dirty_region);
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                                    INT base_vertex, UINT min_vertex, UINT num_vertices,
                                                    UINT start_index, UINT primitive_count) {
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    UpdatePhotoAlbumViewport(device);
    if (device == g_device_hook.device && g_device_hook.original_draw_indexed_primitive) {
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawIndexedPrimitive", type, primitive_count, nullptr, num_vertices, 0);
        }
        const TitleScreenClipState title_clip = BeginTitleScreenClip(device);
        const HRESULT result = g_device_hook.original_draw_indexed_primitive(
            device, type, base_vertex, min_vertex, num_vertices, start_index, primitive_count);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                               UINT primitive_count, const void* vertices, UINT stride) {
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    UpdateScheduleDateHover(device);
    UpdatePhotoAlbumViewport(device);
    if (device == g_device_hook.device && g_device_hook.original_draw_primitive_up) {
        const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
        if (IsPretransformedUI(device, nullptr) && IsPhotoAlbumCGVisible() &&
            vertices && vertex_count > 0 && vertex_count <= 16384) {
            const size_t byte_count =
                static_cast<size_t>(vertex_count) * stride;
            std::vector<unsigned char> transformed(byte_count);
            std::memcpy(transformed.data(), vertices, byte_count);
            if (TransformPhotoAlbumCGRawVertices(
                    transformed.data(), vertex_count, stride)) {
                return g_device_hook.original_draw_primitive_up(
                    device, type, primitive_count,
                    transformed.data(), stride);
            }
        }
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawPrimitiveUP", type, primitive_count, vertices,
                      PrimitiveVertexCount(type, primitive_count), stride);
        }
        const TitleScreenClipState title_clip = BeginTitleScreenClip(device);
        const HRESULT result = g_device_hook.original_draw_primitive_up(
            device, type, primitive_count, vertices, stride);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT min_vertex, UINT num_vertices,
    UINT primitive_count, const void* indices, D3DFORMAT index_format,
    const void* vertices, UINT stride) {
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    UpdatePhotoAlbumViewport(device);
    if (device == g_device_hook.device && g_device_hook.original_draw_indexed_primitive_up) {
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawIndexedPrimitiveUP", type, primitive_count, vertices, num_vertices, stride);
        }
        const TitleScreenClipState title_clip = BeginTitleScreenClip(device);
        const HRESULT result = g_device_hook.original_draw_indexed_primitive_up(
            device, type, min_vertex, num_vertices, primitive_count, indices,
            index_format, vertices, stride);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

bool PatchVtableSlot(void** vtable, size_t index, void* replacement, void** original_out) {
    DWORD old_protection = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old_protection)) {
        return false;
    }
    *original_out = vtable[index];
    vtable[index] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
    return true;
}

bool InstallUIViewportHook(IDirect3DDevice9* device, UINT width, UINT height, int ui_scale_mode) {
    if (!device || g_device_hook.device) {
        return false;
    }

    void** original = *reinterpret_cast<void***>(device);
    g_device_hook.device = device;
    g_device_hook.original_vtable = original;
    g_device_hook.width = width;
    g_device_hook.height = height;
    g_device_hook.active_target_width = width;
    g_device_hook.active_target_height = height;
    IDirect3DSurface9* main_target = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &main_target)) && main_target) {
        g_device_hook.main_target_surface = main_target;
        g_device_hook.active_target_is_main = true;
    } else {
        g_device_hook.active_target_is_main = false;
    }
    g_device_hook.ui_scale_mode = ui_scale_mode;
    const bool target_ok = PatchVtableSlot(
        original, 37, reinterpret_cast<void*>(&HookSetRenderTarget),
        reinterpret_cast<void**>(&g_device_hook.original_set_render_target));
    const bool viewport_ok = PatchVtableSlot(
        original, 47, reinterpret_cast<void*>(&HookSetViewport),
        reinterpret_cast<void**>(&g_device_hook.original_set_viewport));
    Log("Installed UI target/viewport hooks for %ux%u output (mode=%d target=%d viewport=%d)",
        width, height, ui_scale_mode, target_ok, viewport_ok);
    return target_ok && viewport_ok;
}

bool InstallUIDrawHooks(IDirect3DDevice9* device, bool diagnostics, bool suppress_transparent,
                        bool container_probe, bool suppress_proxy_containers,
                        bool gui_runtime_probe) {
    if (!device || (g_device_hook.device && g_device_hook.device != device)) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(device);
    g_device_hook.device = device;
    g_device_hook.original_vtable = vtable;
    g_device_hook.suppress_transparent_ui = suppress_transparent;
    g_device_hook.ui_draw_diagnostics = diagnostics;
    g_device_hook.ui_container_probe = container_probe;
    g_device_hook.suppress_proxy_containers = suppress_proxy_containers;
    g_device_hook.gui_runtime_probe = gui_runtime_probe;
    bool ok = true;
    ok &= PatchVtableSlot(vtable, 41, reinterpret_cast<void*>(&HookBeginScene),
                          reinterpret_cast<void**>(&g_device_hook.original_begin_scene));
    ok &= PatchVtableSlot(vtable, 17, reinterpret_cast<void*>(&HookPresent),
                          reinterpret_cast<void**>(&g_device_hook.original_present));
    ok &= PatchVtableSlot(vtable, 81, reinterpret_cast<void*>(&HookDrawPrimitive),
                          reinterpret_cast<void**>(&g_device_hook.original_draw_primitive));
    ok &= PatchVtableSlot(vtable, 82, reinterpret_cast<void*>(&HookDrawIndexedPrimitive),
                          reinterpret_cast<void**>(&g_device_hook.original_draw_indexed_primitive));
    ok &= PatchVtableSlot(vtable, 83, reinterpret_cast<void*>(&HookDrawPrimitiveUP),
                          reinterpret_cast<void**>(&g_device_hook.original_draw_primitive_up));
    ok &= PatchVtableSlot(vtable, 84, reinterpret_cast<void*>(&HookDrawIndexedPrimitiveUP),
                          reinterpret_cast<void**>(&g_device_hook.original_draw_indexed_primitive_up));
    Log("Installed UI draw hooks: %s diagnostics=%d suppressTransparent=%d containerProbe=%d suppressProxy=%d runtimeProbe=%d",
        ok ? "ok" : "partial failure", diagnostics, suppress_transparent,
        container_probe, suppress_proxy_containers, gui_runtime_probe);
    return ok;
}


}  // namespace stardom
