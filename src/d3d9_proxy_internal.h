#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <string>

namespace stardom {

struct Config {
    UINT width = 1920;
    UINT height = 1080;
    bool enabled = true;
    bool borderless = false;
    bool native_render = false;
    int ui_scale_mode = 1;
    bool ui_draw_diagnostics = false;
    bool suppress_transparent_ui = false;
    bool ui_container_probe = false;
    bool suppress_proxy_containers = true;
    bool gui_runtime_probe = false;
    bool unified_ui_layout = true;
};


struct UnifiedUILayoutState {
    UINT width = 0;
    UINT height = 0;
    void* roots[256]{};
    size_t root_count = 0;
    void* group_canvases[256]{};
    size_t group_canvas_count = 0;
    void* world_roots[256]{};
    size_t world_root_count = 0;
    void* world_projected_objects[512]{};
    size_t world_projected_object_count = 0;
    void* world_move_sources[64]{};
    size_t world_move_source_count = 0;
    void* processed[16384]{};
    size_t processed_count = 0;
    void* primary_root = nullptr;
    ULONGLONG last_refresh_tick = 0;
    volatile LONG transformed_count = 0;
    volatile LONG map_label_move_count = 0;
    volatile LONG world_status_bubble_move_count = 0;
    volatile LONG toolbar_animation_log_count = 0;
    void* schedule_root = nullptr;
    void* schedule_highlight_rows[7]{};
    ULONGLONG last_schedule_hover_tick = 0;
    int last_schedule_hover_row = -2;
    void* photo_album_root = nullptr;
    bool photo_album_viewport_logged = false;
    bool photo_album_viewport_active = false;
    ULONGLONG last_photo_album_visible_tick = 0;
    volatile LONG photo_album_animation_log_count = 0;
    struct PhotoAlbumAnimationSequence {
        void* object = nullptr;
        bool active = false;
        int source_x = 0;
        int source_y = 0;
        int target_native_x = 0;
        int target_native_y = 0;
    } photo_album_sequences[16]{};
    int photo_album_animation_direction = 0;
    void* in_game_cg_root = nullptr;
    void* in_game_cg_caption = nullptr;
    int in_game_cg_caption_native_y = 342;
    void* in_game_cg_item_notice = nullptr;
    int in_game_cg_item_notice_native_x = 0;
    int in_game_cg_item_notice_native_y = 316;
    bool in_game_cg_logged = false;
    bool in_game_cg_caption_logged = false;
    bool in_game_cg_item_notice_logged = false;
    ULONGLONG last_in_game_cg_overlay_refresh_tick = 0;
    struct InGameCGNativeOverlay {
        void* object = nullptr;
        int native_x = 0;
        int native_y = 0;
        int width = 0;
        int height = 0;
        bool logged = false;
    } in_game_cg_native_overlays[16]{};
    struct ToolbarAnimationSequence {
        void* object = nullptr;
        bool active = false;
        bool legacy_space = false;
        int source_start_x = 0;
        int target_width = 320;
        bool last_open = false;
        bool closing = false;
    } toolbar_sequences[4]{};
    int toolbar_width = 320;
    void* trampoline = nullptr;
    bool announcement_active = false;
    bool installed = false;
};

extern UnifiedUILayoutState g_unified_ui;
extern ULONGLONG g_attach_tick;

void Log(const char* format, ...);
void LogProbeBytes(const unsigned char* address, size_t count);

bool CanReadGuiObject(const void* object);
void RefreshUnifiedUILayout();
void RefreshInGameCGOverlays();
bool InstallUnifiedUILayoutHook(UINT width, UINT height);
bool PatchMapLocationProjectionBounds(UINT width, UINT height);
bool PatchAirportLocationLabelFilter();
bool PatchMapLocationVisibilityGuards();
bool InstallMapLocationDiagnosticHook();
void GetPhotoAlbumViewport(UINT output_width, UINT output_height,
                           int& x, int& y, int& width, int& height);
bool IsPhotoAlbumViewportNeeded();
bool IsPhotoAlbumCGVisible();
bool IsPhotoAlbumScreenVisible();
bool IsInGameCGVisible();
bool IsAnnouncementScreenVisible();

bool InstallUIViewportHook(IDirect3DDevice9* device, UINT width, UINT height,
                           int ui_scale_mode);
bool InstallUIDrawHooks(IDirect3DDevice9* device, bool diagnostics,
                        bool suppress_transparent, bool container_probe,
                        bool suppress_proxy_containers, bool gui_runtime_probe);

}  // namespace stardom
