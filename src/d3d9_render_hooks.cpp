#include "d3d9_proxy_internal.h"
#include "gui_object.h"
#include "render_diagnostics.h"
#include "render_hook_state.h"
#include "font_outline.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace stardom {

namespace {
std::map<void**, std::array<void*, 119>> original_device_tables;
template<class Fn> Fn OriginalDeviceMethod(IDirect3DDevice9* device, size_t slot) {
    return reinterpret_cast<Fn>(original_device_tables.at(
        *reinterpret_cast<void***>(device))[slot]);
}
}
bool PatchVtableSlot(void** vtable, size_t index, void* replacement, void** original_out);

bool CapturePhoneOverlayDebugFrame(IDirect3DDevice9* device) {
    IDirect3DSurface9* back_buffer = nullptr;
    if (FAILED(device->GetBackBuffer(
            0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer)) || !back_buffer) {
        Log("Phone overlay capture failed: GetBackBuffer");
        return false;
    }

    D3DSURFACE_DESC desc{};
    HRESULT result = back_buffer->GetDesc(&desc);
    if (FAILED(result) ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) {
        Log("Phone overlay capture failed: unsupported back buffer format=%d result=0x%08X",
            static_cast<int>(desc.Format), static_cast<unsigned int>(result));
        back_buffer->Release();
        return false;
    }

    IDirect3DSurface9* system_surface = nullptr;
    result = device->CreateOffscreenPlainSurface(
        desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
        &system_surface, nullptr);
    if (FAILED(result) || !system_surface) {
        Log("Phone overlay capture failed: CreateOffscreenPlainSurface result=0x%08X",
            static_cast<unsigned int>(result));
        back_buffer->Release();
        return false;
    }
    result = device->GetRenderTargetData(back_buffer, system_surface);
    back_buffer->Release();
    if (FAILED(result)) {
        Log("Phone overlay capture failed: GetRenderTargetData result=0x%08X",
            static_cast<unsigned int>(result));
        system_surface->Release();
        return false;
    }

    D3DLOCKED_RECT locked{};
    result = system_surface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(result)) {
        Log("Phone overlay capture failed: LockRect result=0x%08X",
            static_cast<unsigned int>(result));
        system_surface->Release();
        return false;
    }

    wchar_t executable_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
    std::wstring capture_path(executable_path);
    const size_t separator = capture_path.find_last_of(L"\\/");
    capture_path.resize(separator == std::wstring::npos ? 0 : separator + 1);
    capture_path += L"Stardom3.PhoneOverlay.Debug.bmp";

    HANDLE file = CreateFileW(capture_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool saved = file != INVALID_HANDLE_VALUE;
    const DWORD row_bytes = desc.Width * 4;
    const DWORD image_bytes = row_bytes * desc.Height;
    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + image_bytes;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = static_cast<LONG>(desc.Width);
    info_header.biHeight = -static_cast<LONG>(desc.Height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = image_bytes;
    DWORD written = 0;
    if (saved) {
        saved = WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) &&
            written == sizeof(file_header) &&
            WriteFile(file, &info_header, sizeof(info_header), &written, nullptr) &&
            written == sizeof(info_header);
    }
    for (UINT y = 0; saved && y < desc.Height; ++y) {
        const auto* row = static_cast<const unsigned char*>(locked.pBits) +
            static_cast<size_t>(y) * locked.Pitch;
        saved = WriteFile(file, row, row_bytes, &written, nullptr) &&
            written == row_bytes;
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    system_surface->UnlockRect();
    system_surface->Release();
    Log("Phone overlay capture %s path=%ls size=%ux%u",
        saved ? "saved" : "failed", capture_path.c_str(),
        desc.Width, desc.Height);
    return saved;
}

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

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                              static_cast<int>(g_device_hook.height));
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
    if (IsDebugModeEnabled()) {
        g_unified_ui.last_schedule_hover_row = -2;
    }
}

void* FindScheduleHighlightRows(void* start_page, size_t maximum_pages) {
    GuiObjectReadBatch read_batch;
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        ClearScheduleHighlightCache();
        return nullptr;
    }

    auto* primary_bytes =
        static_cast<unsigned char*>(g_unified_ui.primary_root);
    void* page = start_page ? start_page :
        *reinterpret_cast<void**>(primary_bytes + 0xF4);
    size_t page_count = 0;
    while (CanReadGuiObject(page) && page_count++ < maximum_pages) {
        auto* page_bytes = static_cast<unsigned char*>(page);
        const int page_width = *reinterpret_cast<int*>(page_bytes + 0x88);
        const int page_height = *reinterpret_cast<int*>(page_bytes + 0x8C);
        void* inner = *reinterpret_cast<void**>(page_bytes + 0xF4);
        if (page_width >= 798 && page_width <= 804 &&
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
                    if (IsDebugModeEnabled()) {
                        Log("Unified UI schedule hover rows discovered page=%p", page);
                    }
                    return nullptr;
                }
            }
        }
        page = *reinterpret_cast<void**>(page_bytes + 0xF8);
    }

    return page_count >= maximum_pages && CanReadGuiObject(page) ?
        page : nullptr;
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

    if (!CanReadGuiObject(g_unified_ui.schedule_root)) {
        // Construction/move activity starts discovery immediately. A bounded
        // fallback cycle covers visibility-only changes without scanning all
        // 4096 root children in one frame.
        const bool layout_activity = g_unified_ui.processed_count !=
            g_unified_ui.schedule_discovery_processed_count;
        if (g_unified_ui.schedule_discovery_root !=
                g_unified_ui.primary_root) {
            g_unified_ui.schedule_discovery_root =
                g_unified_ui.primary_root;
            g_unified_ui.schedule_discovery_cursor = nullptr;
            g_unified_ui.schedule_discovery_active = false;
        }
        constexpr ULONGLONG kScheduleDiscoveryCycleIntervalMs = 500;
        if (!g_unified_ui.schedule_discovery_active &&
            (layout_activity ||
             g_unified_ui.last_schedule_discovery_cycle_tick == 0 ||
             now - g_unified_ui.last_schedule_discovery_cycle_tick >=
                 kScheduleDiscoveryCycleIntervalMs)) {
            g_unified_ui.schedule_discovery_active = true;
            g_unified_ui.schedule_discovery_cursor = nullptr;
            g_unified_ui.schedule_discovery_processed_count =
                g_unified_ui.processed_count;
        }
        if (!g_unified_ui.schedule_discovery_active) {
            return;
        }
        constexpr size_t kScheduleDiscoveryPagesPerFrame = 512;
        g_unified_ui.schedule_discovery_cursor = FindScheduleHighlightRows(
            g_unified_ui.schedule_discovery_cursor,
            kScheduleDiscoveryPagesPerFrame);
        if (CanReadGuiObject(g_unified_ui.schedule_root)) {
            g_unified_ui.schedule_discovery_active = false;
        } else if (!g_unified_ui.schedule_discovery_cursor) {
            g_unified_ui.schedule_discovery_active = false;
            g_unified_ui.last_schedule_discovery_cycle_tick = now;
            return;
        }
        if (!CanReadGuiObject(g_unified_ui.schedule_root)) {
            return;
        }
    }

    auto* page_bytes = static_cast<unsigned char*>(g_unified_ui.schedule_root);
    if (*(page_bytes + 0x99) == 0) {
        // Retain the structurally verified hidden page. When the game toggles
        // visibility directly, hover handling becomes active next frame with
        // no discovery scan or construction event required.
        if (IsDebugModeEnabled()) {
            g_unified_ui.last_schedule_hover_row = -2;
        }
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
    if (IsDebugModeEnabled() &&
        hovered_row != g_unified_ui.last_schedule_hover_row) {
        g_unified_ui.last_schedule_hover_row = hovered_row;
        Log("Unified UI schedule hover row=%d mouse=%ld,%ld page=%d,%d",
            hovered_row, mouse.x, mouse.y, page_x, page_y);
    }
}

HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9* device) {
    if (device != g_device_hook.device || !g_device_hook.original_begin_scene) return OriginalDeviceMethod<BeginSceneFn>(device, 41)(device);
    // Apply schedule hover visibility after the game's per-frame input update
    // and before it traverses the GUI tree to build draw calls.
    UpdateScheduleDateHover(device);
    UpdatePhotoAlbumViewport(device);
    RefreshInGameCGOverlays();
    RefreshUnifiedUILayout();
    g_device_hook.in_game_cg_visible_this_frame = IsInGameCGVisible();
    g_device_hook.first_draw_maintenance_done = false;
    if (device == g_device_hook.device &&
        g_device_hook.original_begin_scene) {
        g_device_hook.title_pillarbox_cleared = false;
        g_device_hook.training_pillarbox_cleared = false;
        g_device_hook.loading_background_cleared = false;
        g_device_hook.loading_page_this_frame = GetLoadingScreenRect(
            g_device_hook.loading_rect_this_frame);
        g_device_hook.title_page_this_frame =
            IsTitleScreenVisible() || IsTitleTutorialVisible();
        g_device_hook.studio_event_list_this_frame =
            IsStudioEventListScreenVisible();
        g_device_hook.airport_selection_this_frame =
            IsAirportSelectionScreenVisible();
        g_device_hook.title_ready_before_draw = IsTitleScreenVisible();
        return g_device_hook.original_begin_scene(device);
    }
    return D3DERR_INVALIDCALL;
}

void RunFirstDrawMaintenance() {
    if (g_device_hook.first_draw_maintenance_done) {
        return;
    }
    g_device_hook.first_draw_maintenance_done = true;

    // Some notification controls finish their font-animation setup after
    // BeginScene. Discovering and reflowing the whole subtree there overwrites
    // that one-shot initialization. Advance discovery here instead: controller
    // updates are complete, while no UI primitive has been submitted yet.
    if (!g_unified_ui.announcement_active) {
        constexpr size_t kAnnouncementDiscoveryNodesPerFrame = 512;
        if (DiscoverAnnouncementScreenIncremental(
                kAnnouncementDiscoveryNodesPerFrame)) {
            RefreshUnifiedUILayoutNow();
            Log("Unified UI announcement activated before first draw");
        }
    }

    // Some GUI controllers write their authored coordinates after BeginScene.
    // Preserve the pre-draw correction, but run it once for the frame rather
    // than once for every submitted primitive.
    // The game can rewrite retained overlay positions after BeginScene, so
    // correct those objects again before the first draw. Surface discovery is
    // budgeted once per frame in HookBeginScene and must not advance twice.
    RefreshInGameCGOverlays(false);
    // Credit scrolling pauses briefly while EndGame changes the artist
    // summary. No GuiMove is emitted during that pause, so explicitly restore
    // the fitted render coordinates after the controller update rather than
    // exposing the native coordinates retained for its -600 cutoff.
    RefreshEndGameCreditsBeforeDraw();
    // BababaCallOut writes BtnPhone back to its parked (-40,-40) coordinate
    // during the controller update after BeginScene. Restore the retained
    // ringing button here, after controller updates and immediately before
    // the first UI primitive is submitted.
    RefreshPhoneOverlayButtonBeforeDraw();
    g_device_hook.in_game_cg_visible_this_frame = IsInGameCGVisible();
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device,
                                               DWORD index,
                                               IDirect3DSurface9* surface) {
    if (device != g_device_hook.device || !g_device_hook.original_set_render_target) return OriginalDeviceMethod<SetRenderTargetFn>(device, 37)(device, index, surface);
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
    if (device != g_device_hook.device || !g_device_hook.original_set_viewport) return OriginalDeviceMethod<SetViewportFn>(device, 47)(device, viewport);
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
    // Preview slots are always 360x200, but their positions are computed by
    // the game from the current artist count.  Accept any such slot contained
    // by the legacy canvas instead of enumerating the 1/2/3/4-artist layouts.
    const bool announcement_preview_position =
        viewport->X <= LegacyCanvas::width && viewport->Y <= LegacyCanvas::height &&
        viewport->X + viewport->Width <= 810 &&
        viewport->Y + viewport->Height <= 610;
    if (main_target && g_unified_ui.announcement_active &&
        announcement_preview_size &&
        announcement_preview_position) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                                  static_cast<int>(g_device_hook.height));
        D3DVIEWPORT9 scaled_preview = *viewport;
        scaled_preview.X = static_cast<DWORD>(viewport_x +
            MulDiv(static_cast<int>(viewport->X), viewport_width, LegacyCanvas::width));
        scaled_preview.Y = static_cast<DWORD>(viewport_y +
            MulDiv(static_cast<int>(viewport->Y), viewport_height, LegacyCanvas::height));
        scaled_preview.Width = static_cast<DWORD>(
            MulDiv(static_cast<int>(viewport->Width), viewport_width, LegacyCanvas::width));
        scaled_preview.Height = static_cast<DWORD>(
            MulDiv(static_cast<int>(viewport->Height), viewport_height, LegacyCanvas::height));
        return g_device_hook.original_set_viewport(device, &scaled_preview);
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

    // Advance at most one bounded discovery chunk at this viewport transition.
    // Construction activity starts a cycle immediately; visibility-only page
    // changes use the short fallback cycle without scanning 4096 nodes at once.
    if (!g_unified_ui.announcement_active) {
        const ULONGLONG now = GetTickCount64();
        if (now != g_device_hook.last_announcement_probe_advance_tick) {
            g_device_hook.last_announcement_probe_advance_tick = now;
            constexpr size_t kAnnouncementDiscoveryNodesPerStep = 512;
            if (DiscoverAnnouncementScreenIncremental(
                    kAnnouncementDiscoveryNodesPerStep)) {
                RefreshUnifiedUILayoutNow();
                Log("Unified UI announcement activated before first viewport");
            }
        }
    }

    D3DVIEWPORT9 scaled = *viewport;
    if (IsPhotoAlbumViewportNeeded()) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                                  static_cast<int>(g_device_hook.height));
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
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                                  static_cast<int>(g_device_hook.height));
        scaled.X = static_cast<DWORD>(viewport_x);
        scaled.Y = static_cast<DWORD>(viewport_y);
        scaled.Width = static_cast<DWORD>(viewport_width);
        scaled.Height = static_cast<DWORD>(viewport_height);
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
    if (IsDebugModeEnabled()) {
        static bool phone_viewport_logged = false;
        static D3DVIEWPORT9 previous_input{};
        static D3DVIEWPORT9 previous_output{};
        const bool phone_visible = IsPhoneOverlayButtonVisible();
        if (!phone_visible) {
            phone_viewport_logged = false;
        } else if (!phone_viewport_logged ||
                   std::memcmp(viewport, &previous_input, sizeof(*viewport)) != 0 ||
                   std::memcmp(&scaled, &previous_output, sizeof(scaled)) != 0) {
            DWORD scissor_enabled = FALSE;
            RECT scissor{};
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor_enabled);
            device->GetScissorRect(&scissor);
            Log("Phone overlay viewport in=%u,%u %ux%u out=%u,%u %ux%u "
                "scissor=%lu rect=%ld,%ld..%ld,%ld announcement=%d",
                viewport->X, viewport->Y, viewport->Width, viewport->Height,
                scaled.X, scaled.Y, scaled.Width, scaled.Height,
                static_cast<unsigned long>(scissor_enabled),
                scissor.left, scissor.top, scissor.right, scissor.bottom,
                g_unified_ui.announcement_active ? 1 : 0);
            previous_input = *viewport;
            previous_output = scaled;
            phone_viewport_logged = true;
        }
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

bool IsAirportDateHudVertexRange(IDirect3DDevice9* device,
                                 const void* vertices,
                                 UINT vertex_count, UINT stride) {
    (void)device;
    if (!g_device_hook.airport_selection_this_frame || !vertices ||
        vertex_count == 0 || stride < sizeof(float) * 4) {
        return false;
    }

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    const auto* bytes = static_cast<const unsigned char*>(vertices);
    for (UINT i = 0; i < vertex_count; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            bytes + static_cast<size_t>(i) * stride);
        if (!std::isfinite(position[0]) || !std::isfinite(position[1])) {
            return false;
        }
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }

    constexpr float kDateWidth = 336.0f;
    constexpr float kDateHeight = 35.0f;
    constexpr float kTolerance = 1.0f;
    const float output_width = static_cast<float>(g_device_hook.width);
    const bool date_hud =
        min_x >= output_width - kDateWidth - kTolerance &&
        max_x <= output_width + kTolerance &&
        min_y >= -kTolerance && max_y <= kDateHeight + kTolerance;
    return date_hud;
}

bool IsAirportDateHudPrimitiveFromStream(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
    UINT start_vertex, UINT primitive_count) {
    if (!g_device_hook.airport_selection_this_frame) {
        return false;
    }
    const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (vertex_count == 0 ||
        FAILED(device->GetStreamSource(
            0, &buffer, &stream_offset, &stride)) ||
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
    void* data = nullptr;
    if (FAILED(buffer->GetDesc(&desc)) || byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset),
                            static_cast<UINT>(byte_count), &data,
                            D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }
    const bool date_hud = IsAirportDateHudVertexRange(
        device, data, vertex_count, stride);
    buffer->Unlock();
    buffer->Release();
    return date_hud;
}

bool IsAirportDateHudIndexedPrimitiveFromStream(
    IDirect3DDevice9* device, INT base_vertex,
    UINT min_vertex, UINT num_vertices) {
    if (!g_device_hook.airport_selection_this_frame || num_vertices == 0) {
        return false;
    }
    const int64_t first_vertex = static_cast<int64_t>(base_vertex) +
        static_cast<int64_t>(min_vertex);
    if (first_vertex < 0) {
        return false;
    }
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(
            0, &buffer, &stream_offset, &stride)) ||
        !buffer || stride < sizeof(float) * 4) {
        if (buffer) {
            buffer->Release();
        }
        return false;
    }
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(first_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(num_vertices) * stride;
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
    const bool date_hud = IsAirportDateHudVertexRange(
        device, data, num_vertices, stride);
    buffer->Unlock();
    buffer->Release();
    return date_hud;
}

bool ScaleAirportAirplaneVertices(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
    UINT primitive_count, const void* vertices, UINT stride,
    std::vector<unsigned char>& scaled) {
    if (!g_device_hook.airport_selection_this_frame ||
        type != D3DPT_TRIANGLESTRIP || primitive_count != 2 || !vertices ||
        stride < sizeof(float) * 4) {
        return false;
    }
    constexpr UINT kVertexCount = 4;
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    const auto* source = static_cast<const unsigned char*>(vertices);
    for (UINT i = 0; i < kVertexCount; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            source + static_cast<size_t>(i) * stride);
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    if (std::abs((max_x - min_x) - 54.0f) > 1.0f ||
        std::abs((max_y - min_y) - 58.0f) > 1.0f) {
        return false;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                              static_cast<int>(g_device_hook.height));
    const float center_x = (min_x + max_x) * 0.5f;
    const float center_y = (min_y + max_y) * 0.5f;
    scaled.assign(source, source + static_cast<size_t>(kVertexCount) * stride);
    for (UINT i = 0; i < kVertexCount; ++i) {
        auto* position = reinterpret_cast<float*>(
            scaled.data() + static_cast<size_t>(i) * stride);
        // The controller already submits the airplane on the fitted route.
        // Preserve that live center and scale only the visual dimensions so
        // the sprite does not drift down-right along the animation.
        position[0] = center_x + (position[0] - center_x) *
            static_cast<float>(viewport_width) / static_cast<float>(LegacyCanvas::width);
        position[1] = center_y + (position[1] - center_y) *
            static_cast<float>(viewport_height) / static_cast<float>(LegacyCanvas::height);
    }
    return true;
}

bool DrawScaledAirportAirplanePrimitive(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
    UINT start_vertex, UINT primitive_count, HRESULT& result) {
    if (!g_device_hook.airport_selection_this_frame ||
        type != D3DPT_TRIANGLESTRIP || primitive_count != 2) {
        return false;
    }
    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(
            0, &buffer, &stream_offset, &stride)) ||
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
    std::vector<unsigned char> scaled;
    const bool airplane = ScaleAirportAirplaneVertices(
        device, type, primitive_count, data, stride, scaled);
    buffer->Unlock();
    if (!airplane) {
        buffer->Release();
        return false;
    }
    result = g_device_hook.original_draw_primitive_up(
        device, type, primitive_count, scaled.data(), stride);
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

TitleScreenClipState BeginTitleScreenClip(
    IDirect3DDevice9* device, bool preserve_airport_date_hud = false) {
    TitleScreenClipState state;
    const bool loading_page = g_device_hook.loading_page_this_frame;
    const RECT loading_rect = g_device_hook.loading_rect_this_frame;
    const bool title_page = g_device_hook.title_page_this_frame;
    const bool studio_event_list =
        g_device_hook.studio_event_list_this_frame;
    const bool airport_selection =
        g_device_hook.airport_selection_this_frame;
    if (!device || !g_device_hook.active_target_is_main ||
        (!loading_page && !title_page && !studio_event_list &&
         !airport_selection)) {
        return state;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                              static_cast<int>(g_device_hook.height));
    RECT title_rect = loading_page ? loading_rect : RECT{
        viewport_x, viewport_y,
        viewport_x + viewport_width, viewport_y + viewport_height};
    if (loading_page && !g_device_hook.loading_background_cleared) {
        if (SUCCEEDED(device->Clear(
                0, nullptr, D3DCLEAR_TARGET,
                D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0))) {
            g_device_hook.loading_background_cleared = true;
            if (!g_device_hook.loading_background_logged) {
                Log("Loading page output background cleared behind %ld,%ld %ldx%ld",
                    loading_rect.left, loading_rect.top,
                    loading_rect.right - loading_rect.left,
                    loading_rect.bottom - loading_rect.top);
                g_device_hook.loading_background_logged = true;
            }
        }
    }
    if (!loading_page && !g_device_hook.title_pillarbox_cleared) {
        D3DRECT bars[4]{};
        DWORD bar_count = 0;
        if (viewport_x > 0) {
            bars[bar_count++] = D3DRECT{
                0, 0, viewport_x, static_cast<LONG>(g_device_hook.height)};
        }
        if (viewport_x + viewport_width <
            static_cast<int>(g_device_hook.width)) {
            // TodayDate is a root-level 336x35 HUD at the output top-right.
            // Airport rendering may start another scene after drawing it, so
            // a later pillarbox Clear must never erase its right-hand part.
            const LONG right_bar_top = airport_selection ? 35 : 0;
            bars[bar_count++] = D3DRECT{
                viewport_x + viewport_width, right_bar_top,
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
                Log("Aspect-fit page pillarbox cleared around %d,%d %dx%d",
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
    if (airport_selection && preserve_airport_date_hud &&
        !loading_page && !title_page && !studio_event_list) {
        // The game can already have the airport viewport scissor enabled when
        // the root-level TodayDate HUD is submitted. Merely skipping our own
        // SetScissorRect leaves that inherited clip active, so explicitly
        // disable it for this draw and restore it in EndTitleScreenClip.
        if (FAILED(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE))) {
            return state;
        }
        state.active = true;
        return state;
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
        !g_device_hook.in_game_cg_visible_this_frame ||
        type != D3DPT_TRIANGLESTRIP ||
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

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                              static_cast<int>(g_device_hook.height));
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
        NearlyEqual(min_y, 0.0f) && NearlyEqual(max_x, static_cast<float>(LegacyCanvas::width)) &&
        NearlyEqual(max_y, static_cast<float>(LegacyCanvas::height));
    const bool preview = NearlyEqual(max_x - min_x, 360.0f) &&
        NearlyEqual(max_y - min_y, 200.0f) && min_x >= -1.0f &&
        min_y >= -1.0f && max_x <= 801.0f && max_y <= 601.0f;
    return background || preview;
}

bool CropToolbarBackgroundVertices(IDirect3DDevice9* device,
                                   D3DPRIMITIVETYPE type,
                                   UINT primitive_count,
                                   const void* vertices, UINT stride,
                                   std::vector<unsigned char>& cropped) {
    if (!device || type != D3DPT_TRIANGLESTRIP || primitive_count != 2 ||
        !vertices || stride < sizeof(float) * 4 ||
        !g_device_hook.active_target_is_main) {
        return false;
    }

    RECT toolbar_rect{};
    float texture_width_ratio = 1.0f;
    if (!GetToolbarBackgroundRenderRect(toolbar_rect,
                                        texture_width_ratio) ||
        !IsPretransformedUI(device, nullptr)) {
        return false;
    }

    constexpr UINT kVertexCount = 4;
    const auto* source = static_cast<const unsigned char*>(vertices);
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    for (UINT i = 0; i < kVertexCount; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            source + static_cast<size_t>(i) * stride);
        min_x = std::min(min_x, position[0]);
        min_y = std::min(min_y, position[1]);
        max_x = std::max(max_x, position[0]);
        max_y = std::max(max_y, position[1]);
    }
    if (!NearlyEqual(min_x, static_cast<float>(toolbar_rect.left), 2.0f) ||
        !NearlyEqual(min_y, static_cast<float>(toolbar_rect.top), 2.0f) ||
        !NearlyEqual(max_x, static_cast<float>(toolbar_rect.right), 2.0f) ||
        !NearlyEqual(max_y, static_cast<float>(toolbar_rect.bottom), 2.0f)) {
        return false;
    }

    DWORD fvf = 0;
    if (FAILED(device->GetFVF(&fvf))) {
        return false;
    }
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
    if (texture_count < 1 ||
        texture_offset + sizeof(float) * 2 > stride) {
        return false;
    }

    cropped.assign(source, source + static_cast<size_t>(kVertexCount) * stride);
    float min_u = FLT_MAX;
    float max_u = -FLT_MAX;
    for (UINT i = 0; i < kVertexCount; ++i) {
        const auto* uv = reinterpret_cast<const float*>(
            source + static_cast<size_t>(i) * stride + texture_offset);
        min_u = std::min(min_u, uv[0]);
        max_u = std::max(max_u, uv[0]);
    }
    const float uv_range = max_u - min_u;
    if (uv_range <= 0.0f) {
        cropped.clear();
        return false;
    }
    for (UINT i = 0; i < kVertexCount; ++i) {
        auto* uv = reinterpret_cast<float*>(
            cropped.data() + static_cast<size_t>(i) * stride +
            texture_offset);
        uv[0] = min_u + (uv[0] - min_u) * texture_width_ratio;
    }
    return true;
}

bool DrawCroppedToolbarBackgroundPrimitive(IDirect3DDevice9* device,
                                           D3DPRIMITIVETYPE type,
                                           UINT start_vertex,
                                           UINT primitive_count,
                                           HRESULT& result) {
    if (!device || !g_device_hook.original_draw_primitive_up ||
        type != D3DPT_TRIANGLESTRIP || primitive_count != 2) {
        return false;
    }
    RECT toolbar_rect{};
    float texture_width_ratio = 1.0f;
    if (!GetToolbarBackgroundRenderRect(toolbar_rect,
                                        texture_width_ratio)) {
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

    std::vector<unsigned char> cropped;
    const bool matched = CropToolbarBackgroundVertices(
        device, type, primitive_count, data, stride, cropped);
    buffer->Unlock();
    if (!matched) {
        buffer->Release();
        return false;
    }

    result = g_device_hook.original_draw_primitive_up(
        device, type, primitive_count, cropped.data(), stride);
    // DrawPrimitiveUP clears stream 0 by contract.
    device->SetStreamSource(0, buffer, stream_offset, stride);
    buffer->Release();
    return true;
}

void ClearTrainingPillarbox(IDirect3DDevice9* device) {
    if (!device || g_device_hook.training_pillarbox_cleared ||
        !g_device_hook.active_target_is_main ||
        !IsTrainingScreenPillarboxNeeded()) {
        return;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                              static_cast<int>(g_device_hook.height));
    if (viewport_x <= 0 || viewport_x + viewport_width >=
            static_cast<int>(g_device_hook.width)) {
        return;
    }

    const D3DRECT bars[2] = {
        {0, 0, viewport_x, static_cast<LONG>(g_device_hook.height)},
        {viewport_x + viewport_width, 0,
         static_cast<LONG>(g_device_hook.width),
         static_cast<LONG>(g_device_hook.height)},
    };
    if (SUCCEEDED(device->Clear(
            2, bars, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0))) {
        g_device_hook.training_pillarbox_cleared = true;
        if (!g_device_hook.training_pillarbox_logged) {
            Log("Training screen pillarbox cleared around %d,%d %dx%d",
                viewport_x, viewport_y, viewport_width, viewport_height);
            g_device_hook.training_pillarbox_logged = true;
        }
    }
}

bool DrawShiftedAnnouncementPrimitive(IDirect3DDevice9* device,
                                      D3DPRIMITIVETYPE type,
                                      UINT start_vertex,
                                      UINT primitive_count,
                                      HRESULT& result) {
    if ((!g_unified_ui.announcement_active &&
         !g_unified_ui.photo_album_viewport_active) ||
        g_device_hook.active_target_width != g_device_hook.width ||
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
    const bool full_legacy_canvas = NearlyEqual(min_x, 0.0f) &&
        NearlyEqual(min_y, 0.0f) && NearlyEqual(max_x, static_cast<float>(LegacyCanvas::width)) &&
        NearlyEqual(max_y, static_cast<float>(LegacyCanvas::height));
    const bool known_announcement_quad = IsAnnouncementLegacyQuad(
        min_x, min_y, max_x, max_y);
    const bool album_snapshot = full_legacy_canvas &&
        IsPhotoAlbumViewportNeeded();
    const bool announcement_quad = g_unified_ui.announcement_active &&
        known_announcement_quad;
    if (!album_snapshot && !announcement_quad) {
        buffer->Release();
        return false;
    }

    float offset_x = 0.0f;
    float offset_y = 0.0f;
    if (album_snapshot) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                                  static_cast<int>(g_device_hook.height));
        for (UINT i = 0; i < kVertexCount; ++i) {
            auto* position = reinterpret_cast<float*>(
                shifted.data() + static_cast<size_t>(i) * stride);
            position[0] = static_cast<float>(viewport_x) +
                position[0] * viewport_width / static_cast<float>(LegacyCanvas::width);
            position[1] = static_cast<float>(viewport_y) +
                position[1] * viewport_height / static_cast<float>(LegacyCanvas::height);
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
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_device_hook.width),
                                  static_cast<int>(g_device_hook.height));
        offset_x = static_cast<float>(viewport_x);
        offset_y = static_cast<float>(viewport_y);
        for (UINT i = 0; i < kVertexCount; ++i) {
            auto* position = reinterpret_cast<float*>(
                shifted.data() + static_cast<size_t>(i) * stride);
            position[0] = offset_x +
                position[0] * viewport_width / static_cast<float>(LegacyCanvas::width);
            position[1] = offset_y +
                position[1] * viewport_height / static_cast<float>(LegacyCanvas::height);
        }
    }

    if (announcement_quad && full_legacy_canvas) {
        // Clear after the office scene has rendered but before the fitted
        // training background, so only the two regions outside 4:3 turn black.
        ClearTrainingPillarbox(device);
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

bool NormalizeAwardsScreenVertices(IDirect3DDevice9* device,
                                    const void* vertices,
                                    UINT vertex_count, UINT stride,
                                    std::vector<unsigned char>& normalized) {
    if (!device || !vertices || vertex_count == 0 ||
        stride < sizeof(float) * 4 ||
        g_device_hook.active_target_is_main ||
        g_device_hook.active_target_width != 400 ||
        g_device_hook.active_target_height != 300 ||
        !IsAwardsCeremonyVisible() ||
        !IsPretransformedUI(device, nullptr)) {
        return false;
    }

    const float root_x = static_cast<float>(
        (static_cast<int>(g_device_hook.width) - LegacyCanvas::width) / 2);
    const float root_y = static_cast<float>(
        (static_cast<int>(g_device_hook.height) - LegacyCanvas::height) / 2);
    if (NearlyEqual(root_x, 0.0f) && NearlyEqual(root_y, 0.0f)) {
        return false;
    }

    constexpr float kTolerance = 2.0f;
    const auto* source = static_cast<const unsigned char*>(vertices);
    for (UINT i = 0; i < vertex_count; ++i) {
        const auto* position = reinterpret_cast<const float*>(
            source + static_cast<size_t>(i) * stride);
        if (position[0] < root_x - kTolerance ||
            position[0] > root_x + 400.0f + kTolerance ||
            position[1] < root_y - kTolerance ||
            position[1] > root_y + 300.0f + kTolerance) {
            return false;
        }
    }

    const size_t byte_count = static_cast<size_t>(vertex_count) * stride;
    normalized.resize(byte_count);
    std::memcpy(normalized.data(), vertices, byte_count);
    for (UINT i = 0; i < vertex_count; ++i) {
        auto* position = reinterpret_cast<float*>(
            normalized.data() + static_cast<size_t>(i) * stride);
        position[0] -= root_x;
        position[1] -= root_y;
    }
    return true;
}

bool DrawNormalizedAwardsScreenPrimitive(IDirect3DDevice9* device,
                                          D3DPRIMITIVETYPE type,
                                          UINT start_vertex,
                                          UINT primitive_count,
                                          HRESULT& result) {
    if (!device || !g_device_hook.original_draw_primitive_up) {
        return false;
    }

    IDirect3DVertexBuffer9* buffer = nullptr;
    UINT stream_offset = 0;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(
            0, &buffer, &stream_offset, &stride)) || !buffer) {
        return false;
    }

    const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
    const size_t byte_offset = static_cast<size_t>(stream_offset) +
        static_cast<size_t>(start_vertex) * stride;
    const size_t byte_count = static_cast<size_t>(vertex_count) * stride;
    D3DVERTEXBUFFER_DESC desc{};
    void* data = nullptr;
    if (vertex_count == 0 || FAILED(buffer->GetDesc(&desc)) ||
        byte_offset > desc.Size ||
        byte_count > static_cast<size_t>(desc.Size) - byte_offset ||
        FAILED(buffer->Lock(static_cast<UINT>(byte_offset),
                            static_cast<UINT>(byte_count), &data,
                            D3DLOCK_READONLY))) {
        buffer->Release();
        return false;
    }

    std::vector<unsigned char> normalized;
    const bool corrected = NormalizeAwardsScreenVertices(
        device, data, vertex_count, stride, normalized);
    buffer->Unlock();
    if (!corrected) {
        buffer->Release();
        return false;
    }

    result = g_device_hook.original_draw_primitive_up(
        device, type, primitive_count, normalized.data(), stride);
    device->SetStreamSource(0, buffer, stream_offset, stride);
    buffer->Release();

    return true;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                             UINT start_vertex, UINT primitive_count) {
    if (device != g_device_hook.device || !g_device_hook.original_draw_primitive) return OriginalDeviceMethod<DrawPrimitiveFn>(device, 81)(device, type, start_vertex, primitive_count);
    RunFirstDrawMaintenance();
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
        if (DrawNormalizedAwardsScreenPrimitive(
                device, type, start_vertex, primitive_count,
                shifted_result)) {
            return shifted_result;
        }
        if (DrawShiftedAnnouncementPrimitive(device, type, start_vertex,
                                             primitive_count, shifted_result)) {
            return shifted_result;
        }
        if (DrawCroppedToolbarBackgroundPrimitive(
                device, type, start_vertex, primitive_count,
                shifted_result)) {
            return shifted_result;
        }
        if (DrawScaledAirportAirplanePrimitive(
                device, type, start_vertex, primitive_count,
                shifted_result)) {
            return shifted_result;
        }
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDrawFromStream(device, "DrawPrimitive", type, start_vertex, primitive_count);
        }
        const bool airport_date_hud = IsAirportDateHudPrimitiveFromStream(
            device, type, start_vertex, primitive_count);
        const TitleScreenClipState title_clip = BeginTitleScreenClip(
            device, airport_date_hud);
        const FontOutlineDraw outline(device,
            primitive_count == 2 &&
            (type == D3DPT_TRIANGLEFAN || type == D3DPT_TRIANGLESTRIP) ? 4 : 0);
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
    if (device != g_device_hook.device || !g_device_hook.original_present) return OriginalDeviceMethod<PresentFn>(device, 17)(device, source, destination, override_window, dirty_region);
    if (device == g_device_hook.device && g_device_hook.original_present) {
        if (IsDebugModeEnabled()) {
            static bool phone_dialogue_capture_attempted = false;
            const bool phone_dialogue_visible =
                IsPhoneOverlayDialogueVisible();
            if (!phone_dialogue_visible) {
                phone_dialogue_capture_attempted = false;
            } else if (!phone_dialogue_capture_attempted) {
                phone_dialogue_capture_attempted = true;
                CapturePhoneOverlayDebugFrame(device);
            }
        }
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
    if (device != g_device_hook.device || !g_device_hook.original_draw_indexed_primitive) return OriginalDeviceMethod<DrawIndexedPrimitiveFn>(device, 82)(device, type, base_vertex, min_vertex, num_vertices, start_index, primitive_count);
    RunFirstDrawMaintenance();
    if (device == g_device_hook.device && g_device_hook.original_draw_indexed_primitive) {
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawIndexedPrimitive", type, primitive_count, nullptr, num_vertices, 0);
        }
        const bool airport_date_hud =
            IsAirportDateHudIndexedPrimitiveFromStream(
                device, base_vertex, min_vertex, num_vertices);
        const TitleScreenClipState title_clip = BeginTitleScreenClip(
            device, airport_date_hud);
        const FontOutlineDraw outline(device,
            num_vertices == 4 && primitive_count == 2 &&
            (type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLEFAN ||
             type == D3DPT_TRIANGLESTRIP) ? 4 : 0);
        const HRESULT result = g_device_hook.original_draw_indexed_primitive(
            device, type, base_vertex, min_vertex, num_vertices, start_index, primitive_count);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                               UINT primitive_count, const void* vertices, UINT stride) {
    if (device != g_device_hook.device || !g_device_hook.original_draw_primitive_up) return OriginalDeviceMethod<DrawPrimitiveUPFn>(device, 83)(device, type, primitive_count, vertices, stride);
    RunFirstDrawMaintenance();
    if (device == g_device_hook.device && g_device_hook.original_draw_primitive_up) {
        std::vector<unsigned char> normalized;
        const UINT vertex_count = PrimitiveVertexCount(type, primitive_count);
        if (NormalizeAwardsScreenVertices(
                device, vertices, vertex_count, stride, normalized)) {
            return g_device_hook.original_draw_primitive_up(
                device, type, primitive_count, normalized.data(), stride);
        }
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawPrimitiveUP", type, primitive_count, vertices,
                      PrimitiveVertexCount(type, primitive_count), stride);
        }
        std::vector<unsigned char> cropped;
        const void* draw_vertices = vertices;
        if (CropToolbarBackgroundVertices(
                device, type, primitive_count, vertices, stride, cropped)) {
            draw_vertices = cropped.data();
        }
        std::vector<unsigned char> scaled_airplane;
        if (ScaleAirportAirplaneVertices(
                device, type, primitive_count, draw_vertices, stride,
                scaled_airplane)) {
            draw_vertices = scaled_airplane.data();
        }
        const bool airport_date_hud = IsAirportDateHudVertexRange(
            device, draw_vertices,
            PrimitiveVertexCount(type, primitive_count), stride);
        const TitleScreenClipState title_clip = BeginTitleScreenClip(
            device, airport_date_hud);
        const FontOutlineDraw outline(device, draw_vertices,
            (type == D3DPT_TRIANGLEFAN || type == D3DPT_TRIANGLESTRIP) &&
            primitive_count == 2 ? 4 : 0, stride);
        const HRESULT result = g_device_hook.original_draw_primitive_up(
            device, type, primitive_count, draw_vertices, stride);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(
    IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT min_vertex, UINT num_vertices,
    UINT primitive_count, const void* indices, D3DFORMAT index_format,
    const void* vertices, UINT stride) {
    if (device != g_device_hook.device || !g_device_hook.original_draw_indexed_primitive_up) return OriginalDeviceMethod<DrawIndexedPrimitiveUPFn>(device, 84)(device, type, min_vertex, num_vertices, primitive_count, indices, index_format, vertices, stride);
    RunFirstDrawMaintenance();
    if (device == g_device_hook.device && g_device_hook.original_draw_indexed_primitive_up) {
        if (g_device_hook.ui_draw_diagnostics) {
            LogUIDraw(device, "DrawIndexedPrimitiveUP", type, primitive_count, vertices, num_vertices, stride);
        }
        const bool airport_date_hud = IsAirportDateHudVertexRange(
            device, vertices, num_vertices, stride);
        const TitleScreenClipState title_clip = BeginTitleScreenClip(
            device, airport_date_hud);
        const FontOutlineDraw outline(device, vertices,
            min_vertex == 0 && num_vertices == 4 && primitive_count == 2 &&
            (type == D3DPT_TRIANGLELIST || type == D3DPT_TRIANGLEFAN ||
             type == D3DPT_TRIANGLESTRIP) ? 4 : 0, stride);
        const HRESULT result = g_device_hook.original_draw_indexed_primitive_up(
            device, type, min_vertex, num_vertices, primitive_count, indices,
            index_format, vertices, stride);
        EndTitleScreenClip(device, title_clip);
        return result;
    }
    return D3DERR_INVALIDCALL;
}

namespace {
using ResetFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ReleaseFn = ULONG (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
struct DeviceLifecycle {
    void** original = nullptr;
    Config config{};
    HWND window = nullptr;
};
std::map<IDirect3DDevice9*, DeviceLifecycle> device_lifecycles;
bool BindLifecycleTable(IDirect3DDevice9* device, DeviceLifecycle& lifecycle);

void RefreshMainTarget(IDirect3DDevice9* device) {
    g_device_hook.main_target_surface = nullptr;
    g_device_hook.active_target_is_main = false;
    g_device_hook.active_target_width = 0;
    g_device_hook.active_target_height = 0;
    IDirect3DSurface9* target = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target))) {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(target->GetDesc(&desc))) {
            g_device_hook.main_target_surface = target;
            g_device_hook.active_target_width = desc.Width;
            g_device_hook.active_target_height = desc.Height;
            g_device_hook.active_target_is_main = true;
        }
        // Borrow identity only: retaining the implicit surface prevents Reset
        // and can keep the device alive. Clear it before Reset/final Release.
        target->Release();
    }
    g_device_hook.first_draw_maintenance_done = false;
    g_device_hook.title_pillarbox_cleared = false;
    g_device_hook.training_pillarbox_cleared = false;
    g_device_hook.loading_background_cleared = false;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device,
                                     D3DPRESENT_PARAMETERS* parameters) {
    const auto found = device_lifecycles.find(device);
    if (found == device_lifecycles.end()) return OriginalDeviceMethod<ResetFn>(device, 16)(device, parameters);
    if (!parameters) return D3DERR_INVALIDCALL;
    auto& lifecycle = found->second;
    auto patched = *parameters;
    NormalizePresentation(patched, lifecycle.config);
    const HWND window = patched.hDeviceWindow ? patched.hDeviceWindow : lifecycle.window;
    const auto& resolution = lifecycle.config.resolution;
    ResizeClientArea(window,
        patched.Windowed ? resolution.width : patched.BackBufferWidth,
        patched.Windowed ? resolution.height : patched.BackBufferHeight,
        lifecycle.config.borderless, !patched.Windowed);
    if (device == g_device_hook.device) {
        g_device_hook.main_target_surface = nullptr;
        g_device_hook.active_target_is_main = false;
        g_device_hook.active_target_width = 0;
        g_device_hook.active_target_height = 0;
    }
    const HRESULT result = reinterpret_cast<ResetFn>(lifecycle.original[16])(device, &patched);
    // The runtime can switch to a different vtable on both failed and successful
    // Reset. In particular, the lost-device table must intercept the next retry.
    if (!BindLifecycleTable(device, lifecycle)) Log("Failed to rebind lifecycle hooks after Reset");
    if (SUCCEEDED(result)) {
        *parameters = patched;
        SyncEngineDisplayMode(lifecycle.config);
        lifecycle.window = window;
        if (device == g_device_hook.device) {
            const auto& config = lifecycle.config;
            if (config.native_render && config.ui_scale_mode != 0) {
                InstallUIViewportHook(device, resolution.width, resolution.height, config.ui_scale_mode);
            }
            if (config.native_render && g_device_hook.original_begin_scene) {
                InstallUIDrawHooks(device, config.ui_draw_diagnostics,
                    config.suppress_transparent_ui, config.ui_container_probe,
                    config.suppress_proxy_containers, config.gui_runtime_probe);
            }
            RefreshMainTarget(device);
        }
        ResizeClientArea(window,
            patched.Windowed ? resolution.width : patched.BackBufferWidth,
            patched.Windowed ? resolution.height : patched.BackBufferHeight,
            lifecycle.config.borderless, !patched.Windowed);
    }
    if (IsDebugModeEnabled()) {
        Log("Reset: device=%p result=0x%08lX output=%dx%d", device,
            static_cast<unsigned long>(result), resolution.width, resolution.height);
    }
    return result;
}

ULONG STDMETHODCALLTYPE HookDeviceRelease(IDirect3DDevice9* device) {
    const auto found = device_lifecycles.find(device);
    if (found == device_lifecycles.end()) return OriginalDeviceMethod<ReleaseFn>(device, 2)(device);
    const auto original = reinterpret_cast<ReleaseFn>(found->second.original[2]);
    const ULONG refs = original(device);
    if (!refs) {
        if (g_device_hook.device == device) g_device_hook = DeviceHookState{};
        device_lifecycles.erase(device);
    }
    return refs;
}

bool BindLifecycleTable(IDirect3DDevice9* device, DeviceLifecycle& lifecycle) {
    auto* table = *reinterpret_cast<void***>(device);
    auto inserted = original_device_tables.try_emplace(table);
    if (inserted.second) std::copy_n(table, inserted.first->second.size(), inserted.first->second.begin());
    lifecycle.original = inserted.first->second.data();
    void* ignored = nullptr;
    return PatchVtableSlot(table, 2, reinterpret_cast<void*>(&HookDeviceRelease), &ignored) &&
        PatchVtableSlot(table, 16, reinterpret_cast<void*>(&HookReset), &ignored);
}
}  // namespace

bool InstallDeviceLifecycleHooks(IDirect3DDevice9* device, const Config& config, HWND window) {
    if (!device) return false;
    if (device_lifecycles.find(device) != device_lifecycles.end()) return true;
    // Rendering adaptation follows the newest game device; other devices pass
    // through to the originals even when D3D9 shares their vtable.
    g_device_hook = DeviceHookState{};
    auto& lifecycle = device_lifecycles[device];
    lifecycle.config = config;
    lifecycle.window = window;
    // Preserve private runtime slots beyond IDirect3DDevice9's public methods.
    // D3D9 uses those slots internally, including during final Release.
    if (!BindLifecycleTable(device, lifecycle)) return false;
    g_device_hook.device = device;
    g_device_hook.width = config.resolution.width;
    g_device_hook.height = config.resolution.height;
    return true;
}

bool PatchVtableSlot(void** vtable, size_t index, void* replacement, void** original_out) {
    if (vtable[index] == replacement) {
        *original_out = original_device_tables.at(vtable)[index];
        return true;
    }
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
    if (!device || (g_device_hook.device && g_device_hook.device != device)) {
        return false;
    }

    void** original = *reinterpret_cast<void***>(device);
    g_device_hook.device = device;
    g_device_hook.original_vtable = original;
    g_device_hook.width = width;
    g_device_hook.height = height;
    g_device_hook.active_target_width = width;
    g_device_hook.active_target_height = height;
    RefreshMainTarget(device);
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
