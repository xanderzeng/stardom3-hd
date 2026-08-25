#pragma once

#include <windows.h>

#include <cstddef>

namespace stardom {

// Runtime context shared by the GUI layout adapters. The legacy global name is
// retained so injected callbacks can migrate without changing their ABI.
struct LayoutContext {
    static constexpr size_t processed_capacity = 16384;
    static constexpr size_t processed_slot_count = processed_capacity * 2;

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
    // Fixed open-addressed set used from the GUI move hook and full-tree
    // maintenance. A 50% maximum load avoids the old linear scan of up to
    // 16384 entries on every lookup.
    void* processed_slots[processed_slot_count]{};
    size_t processed_count = 0;
    void* primary_root = nullptr;
    ULONGLONG last_refresh_tick = 0;
    size_t last_refresh_processed_count = 0;
    volatile LONG transformed_count = 0;
    void* schedule_root = nullptr;
    void* schedule_highlight_rows[7]{};
    ULONGLONG last_schedule_hover_tick = 0;
    ULONGLONG last_schedule_discovery_tick = 0;
    size_t last_schedule_discovery_processed_count = 0;
    int last_schedule_hover_row = -2;
    void* photo_album_root = nullptr;
    bool photo_album_viewport_logged = false;
    bool photo_album_viewport_active = false;
    ULONGLONG last_photo_album_visible_tick = 0;
    struct PhotoAlbumAnimationSequence {
        void* object = nullptr;
        bool active = false;
        int source_x = 0;
        int source_y = 0;
        int target_native_x = 0;
        int target_native_y = 0;
    } photo_album_sequences[16]{};
    int photo_album_animation_direction = 0;
    int title_screen_mode = 0;
    void* title_screen_root = nullptr;
    bool title_screen_logged = false;
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
    size_t last_in_game_cg_discovery_processed_count = 0;
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

using UnifiedUILayoutState = LayoutContext;
extern LayoutContext g_unified_ui;

}  // namespace stardom
