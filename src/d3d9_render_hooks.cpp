#include "d3d9_proxy_internal.h"
#include "render_diagnostics.h"
#include "render_hook_state.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace stardom {

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

    if (!CanReadGuiObject(g_unified_ui.schedule_root)) {
        // A missing schedule page used to trigger a complete primary-root
        // scan from every draw hook. Retry slowly while the page is absent so
        // a newly opened page is found within a bounded delay without charging
        // thousands of VirtualQuery calls to every frame.
        constexpr ULONGLONG kScheduleDiscoveryIntervalMs = 1000;
        if (g_unified_ui.last_schedule_discovery_tick != 0 &&
            now - g_unified_ui.last_schedule_discovery_tick <
                kScheduleDiscoveryIntervalMs) {
            return;
        }
        g_unified_ui.last_schedule_discovery_tick = now;
        if (!FindScheduleHighlightRows()) {
            return;
        }
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

    // Some GUI controllers write their authored coordinates after BeginScene.
    // Preserve the pre-draw correction, but run it once for the frame rather
    // than once for every submitted primitive.
    RefreshInGameCGOverlays();
    g_device_hook.in_game_cg_visible_this_frame = IsInGameCGVisible();
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
    // Preview slots are always 360x200, but their positions are computed by
    // the game from the current artist count.  Accept any such slot contained
    // by the legacy canvas instead of enumerating the 1/2/3/4-artist layouts.
    const bool announcement_preview_position =
        viewport->X <= 800 && viewport->Y <= 600 &&
        viewport->X + viewport->Width <= 810 &&
        viewport->Y + viewport->Height <= 610;
    if (main_target && g_unified_ui.announcement_active &&
        announcement_preview_size &&
        announcement_preview_position) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        D3DVIEWPORT9 scaled_preview = *viewport;
        scaled_preview.X = static_cast<DWORD>(viewport_x +
            MulDiv(static_cast<int>(viewport->X), viewport_width, 800));
        scaled_preview.Y = static_cast<DWORD>(viewport_y +
            MulDiv(static_cast<int>(viewport->Y), viewport_height, 600));
        scaled_preview.Width = static_cast<DWORD>(
            MulDiv(static_cast<int>(viewport->Width), viewport_width, 800));
        scaled_preview.Height = static_cast<DWORD>(
            MulDiv(static_cast<int>(viewport->Height), viewport_height, 600));
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

    // Ordinary maintenance is intentionally low-frequency, but announcement
    // pages must be identified before their first legacy viewport is applied;
    // otherwise one native 800x600 frame remains visible for up to a second.
    // Probe only at this viewport transition and rate-limit failed probes so
    // ordinary pages never pay for a control-tree scan on every draw call.
    if (!g_unified_ui.announcement_active) {
        static ULONGLONG last_announcement_probe_tick = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last_announcement_probe_tick >= 250) {
            last_announcement_probe_tick = now;
            if (IsAnnouncementScreenVisible()) {
                RefreshUnifiedUILayoutNow();
                Log("Unified UI announcement activated before first viewport");
            }
        }
    }

    D3DVIEWPORT9 scaled = *viewport;
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
    const bool loading_page = g_device_hook.loading_page_this_frame;
    const RECT loading_rect = g_device_hook.loading_rect_this_frame;
    const bool title_page = g_device_hook.title_page_this_frame;
    const bool studio_event_list =
        g_device_hook.studio_event_list_this_frame;
    if (!device || !g_device_hook.active_target_is_main ||
        (!loading_page && !title_page && !studio_event_list)) {
        return state;
    }

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
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

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
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
        NearlyEqual(min_y, 0.0f) && NearlyEqual(max_x, 800.0f) &&
        NearlyEqual(max_y, 600.0f);
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
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_device_hook.width, g_device_hook.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        offset_x = static_cast<float>(viewport_x);
        offset_y = static_cast<float>(viewport_y);
        for (UINT i = 0; i < kVertexCount; ++i) {
            auto* position = reinterpret_cast<float*>(
                shifted.data() + static_cast<size_t>(i) * stride);
            position[0] = offset_x +
                position[0] * viewport_width / 800.0f;
            position[1] = offset_y +
                position[1] * viewport_height / 600.0f;
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

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                             UINT start_vertex, UINT primitive_count) {
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
        if (DrawShiftedAnnouncementPrimitive(device, type, start_vertex,
                                             primitive_count, shifted_result)) {
            return shifted_result;
        }
        if (DrawCroppedToolbarBackgroundPrimitive(
                device, type, start_vertex, primitive_count,
                shifted_result)) {
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
    RunFirstDrawMaintenance();
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
    RunFirstDrawMaintenance();
    if (device == g_device_hook.device && g_device_hook.original_draw_primitive_up) {
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
        const TitleScreenClipState title_clip = BeginTitleScreenClip(device);
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
    RunFirstDrawMaintenance();
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
