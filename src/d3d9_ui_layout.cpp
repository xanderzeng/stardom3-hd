#include "d3d9_proxy_internal.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace stardom {

UnifiedUILayoutState g_unified_ui;

using GuiMoveFn = void(__thiscall*)(void*, int, int);
using MapLocationUpdateFn = bool(__thiscall*)(void*, void*);

struct MapLocationDiagnosticState {
    void* trampoline = nullptr;
    void* last_location = nullptr;
    bool installed = false;
};

MapLocationDiagnosticState g_map_location_diagnostic;

bool IsKnownLayoutRoot(void* object) {
    for (size_t i = 0; i < g_unified_ui.root_count; ++i) {
        if (g_unified_ui.roots[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberLayoutRoot(void* object) {
    if (!object || IsKnownLayoutRoot(object) ||
        g_unified_ui.root_count >= std::size(g_unified_ui.roots)) {
        return;
    }
    g_unified_ui.roots[g_unified_ui.root_count++] = object;
    Log("Unified UI root discovered: object=%p output=%ux%u", object,
        g_unified_ui.width, g_unified_ui.height);
}

bool IsObjectInList(void* object, void* const* objects, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (objects[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberWorldProjectedObject(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.world_projected_objects,
                                  g_unified_ui.world_projected_object_count) ||
        g_unified_ui.world_projected_object_count >=
            std::size(g_unified_ui.world_projected_objects)) {
        return;
    }
    g_unified_ui.world_projected_objects[
        g_unified_ui.world_projected_object_count++] = object;
}

bool IsWorldProjectedObject(void* object) {
    return IsObjectInList(object, g_unified_ui.world_projected_objects,
                          g_unified_ui.world_projected_object_count);
}

void RememberWorldMoveSource(void* source_return) {
    if (!source_return ||
        IsObjectInList(source_return, g_unified_ui.world_move_sources,
                       g_unified_ui.world_move_source_count) ||
        g_unified_ui.world_move_source_count >=
            std::size(g_unified_ui.world_move_sources)) {
        return;
    }
    g_unified_ui.world_move_sources[g_unified_ui.world_move_source_count++] =
        source_return;
    Log("Unified UI learned world move source return=%p", source_return);
}

bool IsWorldMoveSource(void* source_return) {
    return IsObjectInList(source_return, g_unified_ui.world_move_sources,
                          g_unified_ui.world_move_source_count);
}

void RememberGroupCanvas(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.group_canvases,
                                  g_unified_ui.group_canvas_count) ||
        g_unified_ui.group_canvas_count >= std::size(g_unified_ui.group_canvases)) {
        return;
    }
    g_unified_ui.group_canvases[g_unified_ui.group_canvas_count++] = object;
    Log("Unified UI centered page=%p", object);
}

bool IsGroupCanvas(void* object) {
    return IsObjectInList(object, g_unified_ui.group_canvases,
                          g_unified_ui.group_canvas_count);
}

void RememberWorldRoot(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.world_roots,
                                  g_unified_ui.world_root_count) ||
        g_unified_ui.world_root_count >= std::size(g_unified_ui.world_roots)) {
        return;
    }
    g_unified_ui.world_roots[g_unified_ui.world_root_count++] = object;
    Log("Unified UI world overlay=%p", object);
}

bool IsWorldRoot(void* object) {
    return IsObjectInList(object, g_unified_ui.world_roots,
                          g_unified_ui.world_root_count);
}

bool IsProcessedLayoutObject(void* object) {
    for (size_t i = 0; i < g_unified_ui.processed_count; ++i) {
        if (g_unified_ui.processed[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberProcessedLayoutObject(void* object) {
    if (!object || IsProcessedLayoutObject(object) ||
        g_unified_ui.processed_count >= std::size(g_unified_ui.processed)) {
        return;
    }
    g_unified_ui.processed[g_unified_ui.processed_count++] = object;
}

int TransformAnchoredCoordinate(int position, int extent, int legacy_size,
                                int output_size) {
    if (output_size <= legacy_size || position < 0 || position > legacy_size) {
        return position;
    }

    const int center_twice = position * 2 + std::max(extent, 0);
    const int first_third_twice = (legacy_size * 2) / 3;
    const int second_third_twice = (legacy_size * 4) / 3;
    const int expansion = output_size - legacy_size;
    if (center_twice < first_third_twice) {
        return position;
    }
    if (center_twice > second_third_twice) {
        return position + expansion;
    }
    return position + expansion / 2;
}

void TransformRootChildPosition(int width, int height, int& x, int& y) {
    // Treat a coordinate pair as one coordinate space. Dynamic GUI objects
    // sometimes feed their already-transformed/output-space x back through
    // this hook while y still happens to fall below 600. Transforming the
    // axes independently then creates a mixed pair (for example 1531,420 ->
    // 1531,900). It also makes repeated show/hide or animation callbacks
    // non-idempotent. Only coordinates wholly inside the legacy canvas are
    // eligible for legacy edge anchoring; staging and output-space positions
    // pass through unchanged.
    if (x < 0 || x > 800 || y < 0 || y > 600) {
        return;
    }
    const int vertical_center_twice = y * 2 + std::max(height, 0);
    const bool bottom_group = vertical_center_twice > (600 * 4) / 3;
    if (bottom_group && x >= 400 && x <= 800) {
        x += static_cast<int>(g_unified_ui.width) - 800;
    } else {
        x = TransformAnchoredCoordinate(x, width, 800,
            static_cast<int>(g_unified_ui.width));
    }
    y = TransformAnchoredCoordinate(y, height, 600,
        static_cast<int>(g_unified_ui.height));
}

void TransformWorldPosition(int width, int height, int& x, int& y) {
    const int center_x_twice = x * 2 + std::max(width, 0);
    const int center_y_twice = y * 2 + std::max(height, 0);
    x = static_cast<int>((static_cast<int64_t>(center_x_twice) *
        g_unified_ui.width) / (800 * 2)) - width / 2;
    y = static_cast<int>((static_cast<int64_t>(center_y_twice) *
        g_unified_ui.height) / (600 * 2)) - height / 2;
}

void TransformWorldAnchorPosition(int& x, int& y) {
    // Compact speech bubbles are positioned from the actor's 3D projection.
    // That projection already uses the active widescreen viewport, so its
    // coordinates are final screen pixels. The bubble only needs to bypass
    // the generic legacy-UI edge anchoring performed below.
    (void)x;
    (void)y;
}

// The character interaction wheel is a 460x460 world-space canvas attached
// directly to the primary GUI root.  The game already moves its contents from
// the selected character's 3D projection, so applying a screen-edge anchor to
// this canvas shifts the complete wheel to the far right on widescreen.
bool IsWorldInteractionCanvas(void* object, void* parent,
                              int width, int height) {
    return object && parent == g_unified_ui.primary_root &&
        width >= 450 && width <= 470 &&
        height >= 450 && height <= 470;
}

bool CanReadGuiObject(const void* object) {
    if (!object) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(object, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(object);
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end >= start + 0xFC;
}

bool IsRadialInteractionWheel(void* object, void* parent,
                              int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 150 || width > 170 || height < 60 || height > 80) {
        return false;
    }

    // Four 32x32 buttons form the shallow arc displayed above a selected
    // character. Match the child structure so unrelated 160x70 widgets keep
    // their normal screen anchoring.
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t count = 0;
    while (CanReadGuiObject(child) && count < 5) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        if (child_width < 30 || child_width > 34 ||
            child_height < 30 || child_height > 34) {
            return false;
        }
        ++count;
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }
    return count == 4 && child == nullptr;
}

bool IsMapLocationLabel(void* object, void* parent,
                        int width, int height) {
    // Location labels are allocated before their single text child is
    // attached. Requiring that child here lets early map locations fall
    // through to the generic edge-anchor pass and later disappear offscreen.
    // The 172x36 direct-root surface is unique to this set of map labels.
    return CanReadGuiObject(object) && parent == g_unified_ui.primary_root &&
        width >= 165 && width <= 180 && height >= 32 && height <= 40;
}

bool IsWorldStatusBubble(void* object, void* parent,
                         int width, int height) {
    // Compact tags above characters are direct-root world overlays. Some are
    // leaf "..." bubbles; ActiveTag has one memo child and changes its width
    // with the text (observed as 36, 43 and 60 pixels). Match structure and a
    // bounded height instead of one fixed size so auto-sizing cannot make the
    // same object fall back into legacy anchoring.
    // Their positions come from the active 3D viewport and are already output
    // pixels.  A projected point can still numerically fall inside 800x600;
    // letting that point reach the generic legacy-anchor path then adds the
    // widescreen expansion a second time (for example 704,463 -> 1824,943).
    // Match the compact tag surface so every instance uses the same
    // projection-space pass-through, regardless of its current screen point.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 28 || width > 140 || height < 18 || height > 24) {
        return false;
    }
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    if (!child) {
        return true;
    }
    if (!CanReadGuiObject(child)) {
        return false;
    }
    auto* child_bytes = static_cast<unsigned char*>(child);
    const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
    const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
    void* grandchild = *reinterpret_cast<void**>(child_bytes + 0xF4);
    void* next_child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    return !grandchild && !next_child && child_width >= 20 &&
        child_width <= 140 && child_height >= 18 && child_height <= 24;
}

bool IsSmallWorldDialogueBubble(void* object, void* parent,
                                int width, int height) {
    if (!CanReadGuiObject(object) ||
        (!IsWorldRoot(parent) && !IsKnownLayoutRoot(parent)) ||
        width < 160 || width > 230 || height < 95 || height > 135) {
        return false;
    }

    // The compact NPC speech bubble has a 32x32 continue arrow followed by
    // its text area. Match that structure so unrelated world overlays retain
    // center-based placement.
    void* first = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    auto* first_bytes = static_cast<unsigned char*>(first);
    const int first_width = *reinterpret_cast<int*>(first_bytes + 0x88);
    const int first_height = *reinterpret_cast<int*>(first_bytes + 0x8C);
    void* second = *reinterpret_cast<void**>(first_bytes + 0xF8);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    const int second_width = *reinterpret_cast<int*>(second_bytes + 0x88);
    const int second_height = *reinterpret_cast<int*>(second_bytes + 0x8C);
    return first_width >= 30 && first_width <= 34 &&
        first_height >= 30 && first_height <= 34 &&
        second_width >= 80 && second_width <= 190 &&
        second_height >= 35 && second_height <= 85;
}

bool IsTitleTutorialPage(void* object, void* parent,
                         int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // The visible new-player tutorial is a separate page above the title
    // screen. It has two interchangeable 400x530 character/stage layers, a
    // central prompt, the return/preset control in the top-right corner, and
    // a full-width lower explanation layer. Matching this actual overlay is
    // important: the title page underneath has a different 800x600 structure.
    int stage_layers = 0;
    bool central_prompt = false;
    bool compact_control = false;
    bool lower_explanation = false;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 24) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = *reinterpret_cast<int*>(bytes + 0x80);
        const int child_y = *reinterpret_cast<int*>(bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(bytes + 0x8C);
        stage_layers += child_x >= 198 && child_x <= 202 &&
            child_y >= -2 && child_y <= 2 &&
            child_width >= 398 && child_width <= 402 &&
            child_height >= 528 && child_height <= 532;
        central_prompt |= child_x >= 333 && child_x <= 337 &&
            child_y >= 369 && child_y <= 373 &&
            child_width >= 128 && child_width <= 132 &&
            child_height >= 53 && child_height <= 57;
        compact_control |= child_x >= 690 && child_x <= 694 &&
            child_y >= -1 && child_y <= 3 &&
            child_width >= 104 && child_width <= 108 &&
            child_height >= 58 && child_height <= 62;
        lower_explanation |= child_x >= -2 && child_x <= 2 &&
            child_y >= 356 && child_y <= 360 &&
            child_width >= 798 && child_width <= 802 &&
            child_height >= 240 && child_height <= 244;
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return visited == 5 && stage_layers == 2 && central_prompt &&
        compact_control && lower_explanation;
}

extern void* g_title_tutorial_root;

bool HasTitleTutorialPage() {
    if (CanReadGuiObject(g_title_tutorial_root)) {
        return true;
    }
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        return false;
    }
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(g_unified_ui.primary_root) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 512) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int width = *reinterpret_cast<int*>(bytes + 0x88);
        const int height = *reinterpret_cast<int*>(bytes + 0x8C);
        if (IsTitleTutorialPage(
                child, g_unified_ui.primary_root, width, height)) {
            return true;
        }
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return false;
}

bool IsTitleTutorialDialogueBubble(void* object, void* parent,
                                   int width, int height, int x, int y) {
    return parent == g_unified_ui.primary_root &&
        x >= 318 && x <= 322 && y >= 58 && y <= 62 &&
        IsSmallWorldDialogueBubble(object, parent, width, height) &&
        HasTitleTutorialPage();
}

bool IsTitleTutorialProfileDropdown(void* object, void* parent,
                                    int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 58 || width > 82 || height < 133 || height > 137 ||
        !HasTitleTutorialPage()) {
        return false;
    }

    // Birthday month/day and blood-type selectors share one seven-child
    // popup structure: five 25px rows and two 19x29 scroll arrows. They are
    // direct-root overlays positioned in the tutorial's original 800x600
    // coordinates, so they must follow the centered legacy canvas rather
    // than the generic bottom-edge anchoring rule.
    int rows = 0;
    int arrows = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = *reinterpret_cast<int*>(bytes + 0x80);
        const int child_y = *reinterpret_cast<int*>(bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(bytes + 0x8C);
        rows += child_x >= 3 && child_x <= 7 &&
            child_y >= 3 && child_y <= 107 &&
            child_width >= width - 33 && child_width <= width - 29 &&
            child_height >= 23 && child_height <= 27;
        arrows += child_x >= width - 24 && child_x <= width - 20 &&
            (child_y >= 1 && child_y <= 5 ||
             child_y >= 101 && child_y <= 105) &&
            child_width >= 17 && child_width <= 21 &&
            child_height >= 27 && child_height <= 31;
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return visited == 7 && rows == 5 && arrows == 2;
}

bool IsTitleTutorialQuestionPage(void* object, void* parent,
                                 int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602 ||
        !HasTitleTutorialPage()) {
        return false;
    }

    // Tutorial questions are created as a second direct-root 800x600 page,
    // rather than as children of the already-scaled tutorial artwork. The
    // page contains three 200x90 answer buttons along its lower edge and one
    // 464x151 question panel. Match the complete four-child signature so the
    // answer controls receive the same aspect-fit transform as the artwork.
    int answer_buttons = 0;
    bool question_panel = false;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = *reinterpret_cast<int*>(bytes + 0x80);
        const int child_y = *reinterpret_cast<int*>(bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(bytes + 0x8C);
        const bool answer_x =
            (child_x >= 48 && child_x <= 52) ||
            (child_x >= 298 && child_x <= 302) ||
            (child_x >= 548 && child_x <= 552);
        answer_buttons += answer_x && child_y >= 498 && child_y <= 502 &&
            child_width >= 198 && child_width <= 202 &&
            child_height >= 88 && child_height <= 92;
        question_panel |= child_x >= 168 && child_x <= 172 &&
            child_y >= 268 && child_y <= 272 &&
            child_width >= 462 && child_width <= 466 &&
            child_height >= 149 && child_height <= 153;
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return visited == 4 && answer_buttons == 3 && question_panel;
}

bool IsEventPublicationPanel(void* object, void* parent,
                             int width, int height) {
    // Newspaper and magazine events use the only 510x350 direct-root panel.
    // It is a modal overlay inside the centered legacy canvas, not a
    // lower-right HUD widget.
    return object && parent == g_unified_ui.primary_root &&
        width >= 510 && width <= 512 && height >= 348 && height <= 352;
}

bool IsScheduleSecondaryPanel(void* object, void* parent,
                              int width, int height) {
    // The schedule detail picker is a unique 222x381 direct-root panel. The
    // generic layout treats its legacy x=430 as right anchored and adds the
    // complete widescreen expansion. It belongs to the centered schedule
    // page instead, so it must receive only the legacy-canvas center offset.
    return object && parent == g_unified_ui.primary_root &&
        width >= 218 && width <= 226 &&
        height >= 375 && height <= 386;
}

bool IsScheduleJobHoverTooltip(void* object, void* parent,
                               int width, int height) {
    // A schedule job hover card is composed of two direct-root surfaces. The
    // compact surface is placed in legacy page coordinates, while the detail
    // surface can already receive an output-space x from the secondary
    // window. Match both surfaces; the transform below accepts either input.
    if (!object || parent != g_unified_ui.primary_root) {
        return false;
    }
    const bool detail_surface = width >= 168 && width <= 174 &&
        height >= 136 && height <= 142;
    const bool compact_surface = width >= 136 && width <= 142 &&
        height >= 78 && height <= 84;
    return detail_surface || compact_surface;
}

bool IsFullscreenLeafSurface(void* object, void* parent,
                             int width, int height) {
    // Scene transitions use a direct-root, childless 802x602 surface as the
    // opaque fade/loading curtain. Ordinary legacy pages have descendants and
    // must remain centered at their native size; this leaf surface must cover
    // the complete output instead.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 804 || height < 598 || height > 604) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    const int x = *reinterpret_cast<int*>(bytes + 0x80);
    const int y = *reinterpret_cast<int*>(bytes + 0x84);
    void* first_child = *reinterpret_cast<void**>(bytes + 0xF4);
    return x >= -2 && x <= 2 && y >= -2 && y <= 2 && !first_child;
}

bool IsTitleScreenRoot(void* object, void* parent, int width, int height) {
    // The title page has a stable and unusually specific direct-child
    // structure: seven square menu buttons, a version label, two complete
    // background layers, two long animated middle strips, and two 800x310
    // foreground layers. Match the complete signature so ordinary 800x600
    // menus can never opt into title-only scaling by accident.
    if (g_unified_ui.title_screen_mode == 0 ||
        !CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }

    // The attract animation mirrors the menu between the left and right side
    // as the featured character changes. Positions therefore cannot be part
    // of the signature; the seven 100x100 controls and the unusually specific
    // layer stack are sufficient to identify this page.
    int menu_buttons = 0;
    int full_canvas_layers = 0;
    int animated_strips = 0;
    int foreground_layers = 0;
    bool version_label = false;
    size_t direct_children = 0;

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    while (CanReadGuiObject(child) && direct_children++ < 32) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_x = *reinterpret_cast<int*>(child_bytes + 0x80);
        const int child_y = *reinterpret_cast<int*>(child_bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        menu_buttons += child_x >= 15 && child_x <= 685 &&
            (std::abs(child_y - 35) <= 2 || std::abs(child_y - 135) <= 2) &&
            child_width >= 98 && child_width <= 102 &&
            child_height >= 98 && child_height <= 102;
        version_label |= child_x >= 3 && child_x <= 7 &&
            child_y >= 3 && child_y <= 7 &&
            child_width >= 145 && child_width <= 155 &&
            child_height >= 18 && child_height <= 22;
        full_canvas_layers += child_x >= -2 && child_x <= 2 &&
            child_y >= -2 && child_y <= 2 &&
            child_width >= 798 && child_width <= 802 &&
            child_height >= 598 && child_height <= 602;
        animated_strips += child_y >= 181 && child_y <= 185 &&
            child_width >= 1990 && child_width <= 2010 &&
            child_height >= 308 && child_height <= 318;
        foreground_layers += child_x >= -2 && child_x <= 2 &&
            child_y >= 165 && child_y <= 181 &&
            child_width >= 798 && child_width <= 802 &&
            child_height >= 305 && child_height <= 315;
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }

    return menu_buttons == 7 && version_label &&
        full_canvas_layers == 2 && animated_strips == 2 &&
        foreground_layers == 2;
}

bool IsPhotoAlbumRoot(void* object, void* parent, int width, int height) {
    // The Star Photo Album is a full-canvas interactive scene. Its 3D table
    // and book follow the expanded 800x600 viewport, so the GUI page and hit
    // rectangles must use the same anisotropic mapping instead of the normal
    // centered-page rule. Match its unique bottom filmstrip/book structure.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    bool filmstrip = false;
    bool book_page = false;
    bool exit_button = false;
    auto* bytes = static_cast<unsigned char*>(object);
    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 32) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int x = *reinterpret_cast<int*>(child_bytes + 0x80);
        const int y = *reinterpret_cast<int*>(child_bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        filmstrip |= x >= -2 && x <= 2 && y >= 504 && y <= 512 &&
            child_width >= 798 && child_width <= 802 &&
            child_height >= 88 && child_height <= 96;
        book_page |= x >= 395 && x <= 405 && y >= 25 && y <= 40 &&
            child_width >= 350 && child_width <= 360 &&
            child_height >= 440 && child_height <= 450;
        exit_button |= x >= 685 && x <= 695 && y >= 550 && y <= 560 &&
            child_width >= 88 && child_width <= 96 &&
            child_height >= 28 && child_height <= 36;
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }
    return filmstrip && book_page && exit_button;
}

void GetPhotoAlbumViewport(UINT output_width, UINT output_height,
                           int& x, int& y, int& width, int& height);

bool IsInGameCGRoot(void* object, void* parent, int width, int height) {
    // Story CGs are a direct-root 800x600 page containing exactly two
    // childless 800x600 image surfaces (the current and next frame). This is
    // distinct from the interactive photo album, whose root has many controls.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    void* first = *reinterpret_cast<void**>(bytes + 0xF4);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    auto* first_bytes = static_cast<unsigned char*>(first);
    void* second = *reinterpret_cast<void**>(first_bytes + 0xF8);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    if (*reinterpret_cast<void**>(second_bytes + 0xF8) != nullptr) {
        return false;
    }
    for (auto* child_bytes : {first_bytes, second_bytes}) {
        const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        if (child_width < 798 || child_width > 802 ||
            child_height < 598 || child_height > 602 ||
            *reinterpret_cast<void**>(child_bytes + 0xF4) != nullptr) {
            return false;
        }
    }
    return true;
}

bool IsInGameCGCaption(void* object, void* parent, int width, int height) {
    // The story-CG narration bar is a separate direct-root 800x76 surface
    // with one inset 780x60 text child. Keep the bar at native size while the
    // image behind it is enlarged uniformly.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 72 || height > 80) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    if (!CanReadGuiObject(child)) {
        return false;
    }
    auto* child_bytes = static_cast<unsigned char*>(child);
    const int child_x = *reinterpret_cast<int*>(child_bytes + 0x80);
    const int child_y = *reinterpret_cast<int*>(child_bytes + 0x84);
    const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
    const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
    return child_x >= 0 && child_x <= 20 && child_y >= 0 && child_y <= 20 &&
        child_width >= 760 && child_width <= 790 &&
        child_height >= 50 && child_height <= 70 &&
        *reinterpret_cast<void**>(child_bytes + 0xF8) == nullptr;
}

bool IsInGameCGItemNotice(void* object, void* parent,
                          int width, int height) {
    // Item acquisition during story CGs uses a separate 800x128 root. Its two
    // children are the 380x60 memo and the 200x140 item illustration.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 122 || height > 134) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    void* first = *reinterpret_cast<void**>(bytes + 0xF4);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    auto* first_bytes = static_cast<unsigned char*>(first);
    void* second = *reinterpret_cast<void**>(first_bytes + 0xF8);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    const int first_width = *reinterpret_cast<int*>(first_bytes + 0x88);
    const int first_height = *reinterpret_cast<int*>(first_bytes + 0x8C);
    const int second_width = *reinterpret_cast<int*>(second_bytes + 0x88);
    const int second_height = *reinterpret_cast<int*>(second_bytes + 0x8C);
    return first_width >= 370 && first_width <= 390 &&
        first_height >= 55 && first_height <= 65 &&
        second_width >= 190 && second_width <= 210 &&
        second_height >= 130 && second_height <= 150 &&
        *reinterpret_cast<void**>(second_bytes + 0xF8) == nullptr;
}

bool IsInGameCGVisible() {
    if (!CanReadGuiObject(g_unified_ui.in_game_cg_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_unified_ui.in_game_cg_root) +
        0x99) != 0;
}

void GetInGameCGCaptionPosition(int native_y, int& x, int& y) {
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    // Match the photo-album CG caption treatment: preserve the complete
    // authored 800x600 overlay and translate it as one native-size canvas.
    // Do not scale y through the 1440x1080 image viewport; doing so feeds an
    // already moved value back into the next refresh and pushes the bar down.
    x = viewport_x + (viewport_width - 800) / 2;
    y = native_y + (static_cast<int>(g_unified_ui.height) - 600) / 2;
}

void GetInGameCGNativeOverlayPosition(int native_x, int native_y,
                                      int& x, int& y) {
    x = native_x + (static_cast<int>(g_unified_ui.width) - 800) / 2;
    y = native_y + (static_cast<int>(g_unified_ui.height) - 600) / 2;
}

UnifiedUILayoutState::InGameCGNativeOverlay*
FindInGameCGNativeOverlay(void* object) {
    for (auto& overlay : g_unified_ui.in_game_cg_native_overlays) {
        if (overlay.object == object) {
            return &overlay;
        }
    }
    return nullptr;
}

bool IsInGameCGNativeRootStrip(void* object, void* parent,
                               int width, int height) {
    // Only direct-root, full-canvas-width strips are eligible. Nested 800-wide
    // controls inherit the translation of their already-centred 800x600 page
    // and must not receive a second offset.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 32 || height > 180) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    return CanReadGuiObject(*reinterpret_cast<void**>(bytes + 0xF4));
}

UnifiedUILayoutState::InGameCGNativeOverlay*
RegisterInGameCGNativeOverlay(void* object, int native_x, int native_y,
                              int width, int height) {
    if (auto* existing = FindInGameCGNativeOverlay(object)) {
        return existing;
    }
    // A full-width authored strip starts at the legacy canvas edge. Requiring
    // x to remain close to zero prevents unrelated screen-space UI from being
    // captured merely because a story CG happens to be visible behind it.
    if (native_x < -32 || native_x > 32 ||
        native_y < -200 || native_y > 600) {
        return nullptr;
    }
    UnifiedUILayoutState::InGameCGNativeOverlay* free_slot = nullptr;
    for (auto& overlay : g_unified_ui.in_game_cg_native_overlays) {
        if (!overlay.object || !CanReadGuiObject(overlay.object)) {
            free_slot = &overlay;
            break;
        }
    }
    if (!free_slot) {
        return nullptr;
    }
    *free_slot = {};
    free_slot->object = object;
    free_slot->native_x = native_x;
    free_slot->native_y = native_y;
    free_slot->width = width;
    free_slot->height = height;
    return free_slot;
}

void DiscoverInGameCGSurfaces(void* root) {
    if (!CanReadGuiObject(root)) {
        return;
    }
    void* visible_cg = IsInGameCGVisible() ?
        g_unified_ui.in_game_cg_root : nullptr;
    void* visible_caption = nullptr;
    void* visible_item_notice = nullptr;
    int caption_native_y = 342;
    int item_notice_native_x = 0;
    int item_notice_native_y = 316;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(root) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 4096) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = *reinterpret_cast<int*>(bytes + 0x80);
        const int y = *reinterpret_cast<int*>(bytes + 0x84);
        const int width = *reinterpret_cast<int*>(bytes + 0x88);
        const int height = *reinterpret_cast<int*>(bytes + 0x8C);
        if (IsInGameCGRoot(child, root, width, height)) {
            visible_cg = child;
        } else if (IsInGameCGCaption(child, root, width, height)) {
            visible_caption = child;
            caption_native_y = y;
        } else if (IsInGameCGItemNotice(child, root, width, height)) {
            visible_item_notice = child;
            item_notice_native_x = x;
            item_notice_native_y = y;
        } else if (visible_cg &&
                   IsInGameCGNativeRootStrip(child, root, width, height)) {
            RegisterInGameCGNativeOverlay(child, x, y, width, height);
        }
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    if (visible_cg) {
        g_unified_ui.in_game_cg_root = visible_cg;
        if (visible_caption) {
            const bool new_caption =
                visible_caption != g_unified_ui.in_game_cg_caption;
            g_unified_ui.in_game_cg_caption = visible_caption;
            if (new_caption) {
                g_unified_ui.in_game_cg_caption_native_y = caption_native_y;
                g_unified_ui.in_game_cg_caption_logged = false;
            }
        }
        if (visible_item_notice) {
            const bool new_notice =
                visible_item_notice != g_unified_ui.in_game_cg_item_notice;
            g_unified_ui.in_game_cg_item_notice = visible_item_notice;
            if (new_notice) {
                g_unified_ui.in_game_cg_item_notice_native_x =
                    item_notice_native_x;
                g_unified_ui.in_game_cg_item_notice_native_y =
                    item_notice_native_y;
                g_unified_ui.in_game_cg_item_notice_logged = false;
            }
        }
    }
}

void LayoutInGameCGRoot(void* object) {
    if (!CanReadGuiObject(object) || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    auto* bytes = static_cast<unsigned char*>(object);
    *reinterpret_cast<int*>(bytes + 0x88) = viewport_width;
    *reinterpret_cast<int*>(bytes + 0x8C) = viewport_height;
    original(object, viewport_x, viewport_y);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 2) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        *reinterpret_cast<int*>(child_bytes + 0x88) = viewport_width;
        *reinterpret_cast<int*>(child_bytes + 0x8C) = viewport_height;
        original(child, 0, 0);
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }
}

bool IsPhotoAlbumDescendant(void* object) {
    void* cursor = object;
    for (int depth = 0; CanReadGuiObject(cursor) && depth < 10; ++depth) {
        if (cursor == g_unified_ui.photo_album_root) {
            return true;
        }
        cursor = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(cursor) + 0xF0);
    }
    return false;
}

void GetPhotoAlbumViewport(UINT output_width, UINT output_height,
                           int& x, int& y, int& width, int& height) {
    // Preserve the native 4:3 aspect. At 1920x1080 this produces a centered
    // 1440x1080 content area instead of the previous 2.4x/1.8x stretch.
    if (static_cast<uint64_t>(output_width) * 600 <=
        static_cast<uint64_t>(output_height) * 800) {
        width = static_cast<int>(output_width);
        height = MulDiv(width, 600, 800);
    } else {
        height = static_cast<int>(output_height);
        width = MulDiv(height, 800, 600);
    }
    x = (static_cast<int>(output_width) - width) / 2;
    y = (static_cast<int>(output_height) - height) / 2;
}

bool IsTitleScreenDescendant(void* object) {
    void* cursor = object;
    for (int depth = 0; CanReadGuiObject(cursor) && depth < 10; ++depth) {
        if (cursor == g_unified_ui.title_screen_root) {
            return true;
        }
        cursor = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(cursor) + 0xF0);
    }
    return false;
}

bool IsTitleScreenVisible() {
    if (g_unified_ui.title_screen_mode == 0 ||
        !CanReadGuiObject(g_unified_ui.title_screen_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_unified_ui.title_screen_root) +
        0x99) != 0;
}

bool IsTitleTutorialVisible() {
    if (g_unified_ui.title_screen_mode == 0 ||
        !CanReadGuiObject(g_title_tutorial_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_title_tutorial_root) + 0x99) != 0;
}

struct TitleNativeGeometry {
    void* object = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

TitleNativeGeometry g_title_native_geometry[1024]{};
size_t g_title_native_geometry_count = 0;
void* g_title_native_root = nullptr;

struct TitleStripMotionState {
    bool pair_valid = false;
    void* primary_object = nullptr;
    void* follower_object = nullptr;
    int primary_native_x = 0;
    int follower_native_x = 0;
    int native_step = -2;
    ULONGLONG last_tick = 0;
    unsigned int step_accumulator = 0;
};

TitleStripMotionState g_title_strip_motion;

void ResetTitleNativeGeometry(void* root) {
    if (g_title_native_root == root) {
        return;
    }
    g_title_native_root = root;
    g_title_native_geometry_count = 0;
    g_title_strip_motion = {};
}

TitleNativeGeometry* FindTitleNativeGeometry(void* object) {
    for (size_t i = 0; i < g_title_native_geometry_count; ++i) {
        if (g_title_native_geometry[i].object == object) {
            return &g_title_native_geometry[i];
        }
    }
    return nullptr;
}

TitleNativeGeometry* RememberTitleNativeGeometry(void* object) {
    if (!CanReadGuiObject(object)) {
        return nullptr;
    }
    if (TitleNativeGeometry* existing = FindTitleNativeGeometry(object)) {
        return existing;
    }
    if (g_title_native_geometry_count >= std::size(g_title_native_geometry)) {
        return nullptr;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    TitleNativeGeometry& geometry =
        g_title_native_geometry[g_title_native_geometry_count++];
    geometry.object = object;
    geometry.x = *reinterpret_cast<int*>(bytes + 0x80);
    geometry.y = *reinterpret_cast<int*>(bytes + 0x84);
    geometry.width = *reinterpret_cast<int*>(bytes + 0x88);
    geometry.height = *reinterpret_cast<int*>(bytes + 0x8C);
    return &geometry;
}

bool IsTitleStripGeometry(const TitleNativeGeometry* native) {
    return native && native->width >= 1990 && native->width <= 2010 &&
        native->height >= 308 && native->height <= 318;
}

void RegisterTitleStrip(void* object, const TitleNativeGeometry* native) {
    if (!object || !IsTitleStripGeometry(native)) {
        return;
    }
    if (native->x < 1000) {
        if (g_title_strip_motion.primary_object != object) {
            g_title_strip_motion.primary_object = object;
            g_title_strip_motion.primary_native_x = native->x;
        }
    } else {
        g_title_strip_motion.follower_object = object;
    }
    if (g_title_strip_motion.primary_object &&
        g_title_strip_motion.follower_object) {
        g_title_strip_motion.pair_valid = true;
        g_title_strip_motion.follower_native_x =
            g_title_strip_motion.primary_native_x < 0 ?
                g_title_strip_motion.primary_native_x + 2000 :
                g_title_strip_motion.primary_native_x - 2000;
    }
}

void UpdateTitleStripMotion(ULONGLONG now) {
    if (!g_title_strip_motion.pair_valid || !IsTitleScreenVisible() ||
        !CanReadGuiObject(g_title_strip_motion.primary_object) ||
        !CanReadGuiObject(g_title_strip_motion.follower_object) ||
        !g_unified_ui.trampoline) {
        g_title_strip_motion.last_tick = now;
        return;
    }

    if (g_title_strip_motion.last_tick == 0) {
        g_title_strip_motion.last_tick = now;
        return;
    }
    // The native controller advances by two pixels at its 30 Hz update rate.
    // Drive that same state once per elapsed native tick because scaling the
    // objects makes the controller's position feedback unusable. Clamp long
    // gaps so restoring a minimized or paused game never jumps the strip.
    const ULONGLONG elapsed =
        std::min<ULONGLONG>(now - g_title_strip_motion.last_tick, 100);
    g_title_strip_motion.last_tick = now;
    g_title_strip_motion.step_accumulator +=
        static_cast<unsigned int>(elapsed) * 30;
    unsigned int steps = g_title_strip_motion.step_accumulator / 1000;
    g_title_strip_motion.step_accumulator %= 1000;
    if (steps == 0) {
        return;
    }

    while (steps-- > 0) {
        g_title_strip_motion.primary_native_x +=
            g_title_strip_motion.native_step;
        if (g_title_strip_motion.native_step > 0 &&
            g_title_strip_motion.primary_native_x >= 1000) {
            g_title_strip_motion.primary_native_x = -1000;
        } else if (g_title_strip_motion.native_step < 0 &&
                   g_title_strip_motion.primary_native_x <= -1000) {
            g_title_strip_motion.primary_native_x = 1000;
        }
    }
    g_title_strip_motion.follower_native_x =
        g_title_strip_motion.primary_native_x < 0 ?
            g_title_strip_motion.primary_native_x + 2000 :
            g_title_strip_motion.primary_native_x - 2000;

    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    TitleNativeGeometry* primary =
        FindTitleNativeGeometry(g_title_strip_motion.primary_object);
    TitleNativeGeometry* follower =
        FindTitleNativeGeometry(g_title_strip_motion.follower_object);
    if (!primary || !follower) {
        return;
    }
    original(g_title_strip_motion.primary_object,
        MulDiv(g_title_strip_motion.primary_native_x, viewport_width, 800),
        MulDiv(primary->y, viewport_height, 600));
    original(g_title_strip_motion.follower_object,
        MulDiv(g_title_strip_motion.follower_native_x, viewport_width, 800),
        MulDiv(follower->y, viewport_height, 600));
}

void ScaleTitleScreenSubtree(void* object, int depth = 0) {
    if (!CanReadGuiObject(object) || depth > 8 || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int& width = *reinterpret_cast<int*>(bytes + 0x88);
    int& height = *reinterpret_cast<int*>(bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    if (depth == 0) {
        ResetTitleNativeGeometry(object);
        width = viewport_width;
        height = viewport_height;
        original(object, viewport_x, viewport_y);
    } else {
        const bool newly_discovered = FindTitleNativeGeometry(object) == nullptr;
        TitleNativeGeometry* native = RememberTitleNativeGeometry(object);
        RegisterTitleStrip(object, native);
        if (native) {
            width = MulDiv(native->width, viewport_width, 800);
            height = MulDiv(native->height, viewport_height, 600);
            // Existing objects may currently be between animation keyframes.
            // Only translate a newly discovered object here; later movement is
            // handled by HookGuiMove without snapping the animation backward.
            if (newly_discovered) {
                original(object,
                    MulDiv(native->x, viewport_width, 800),
                    MulDiv(native->y, viewport_height, 600));
            }
        }
    }
    RememberProcessedLayoutObject(object);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 512) {
        void* next = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(child) + 0xF8);
        ScaleTitleScreenSubtree(child, depth + 1);
        child = next;
    }
}

TitleNativeGeometry g_title_tutorial_geometry[64]{};
size_t g_title_tutorial_geometry_count = 0;
void* g_title_tutorial_root = nullptr;
TitleNativeGeometry g_title_tutorial_bubble_geometry[16]{};
size_t g_title_tutorial_bubble_geometry_count = 0;
void* g_title_tutorial_bubble = nullptr;
TitleNativeGeometry g_title_tutorial_question_geometry[16]{};
size_t g_title_tutorial_question_geometry_count = 0;
void* g_title_tutorial_question_root = nullptr;

TitleNativeGeometry* FindGeometry(TitleNativeGeometry* geometries,
                                  size_t count, void* object) {
    for (size_t i = 0; i < count; ++i) {
        if (geometries[i].object == object) {
            return &geometries[i];
        }
    }
    return nullptr;
}

TitleNativeGeometry* RememberGeometry(TitleNativeGeometry* geometries,
                                      size_t capacity, size_t& count,
                                      void* object) {
    if (!CanReadGuiObject(object)) {
        return nullptr;
    }
    if (TitleNativeGeometry* existing = FindGeometry(
            geometries, count, object)) {
        return existing;
    }
    if (count >= capacity) {
        return nullptr;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    TitleNativeGeometry& geometry = geometries[count++];
    geometry.object = object;
    geometry.x = *reinterpret_cast<int*>(bytes + 0x80);
    geometry.y = *reinterpret_cast<int*>(bytes + 0x84);
    geometry.width = *reinterpret_cast<int*>(bytes + 0x88);
    geometry.height = *reinterpret_cast<int*>(bytes + 0x8C);
    return &geometry;
}

void ResetTitleTutorialGeometry(void* root) {
    if (g_title_tutorial_root == root) {
        return;
    }
    g_title_tutorial_root = root;
    g_title_tutorial_geometry_count = 0;
}

bool IsTitleTutorialDescendant(void* object) {
    void* cursor = object;
    for (int depth = 0; CanReadGuiObject(cursor) && depth < 10; ++depth) {
        if (cursor == g_title_tutorial_root) {
            return true;
        }
        cursor = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(cursor) + 0xF0);
    }
    return false;
}

void ScaleTitleTutorialSubtree(void* object, int depth = 0) {
    if (!CanReadGuiObject(object) || depth > 8 || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int& width = *reinterpret_cast<int*>(bytes + 0x88);
    int& height = *reinterpret_cast<int*>(bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    if (depth == 0) {
        ResetTitleTutorialGeometry(object);
        width = viewport_width;
        height = viewport_height;
        original(object, viewport_x, viewport_y);
    } else {
        TitleNativeGeometry* native = RememberGeometry(
            g_title_tutorial_geometry,
            std::size(g_title_tutorial_geometry),
            g_title_tutorial_geometry_count, object);
        if (native) {
            width = MulDiv(native->width, viewport_width, 800);
            height = MulDiv(native->height, viewport_height, 600);
            original(object,
                MulDiv(native->x, viewport_width, 800),
                MulDiv(native->y, viewport_height, 600));
        }
    }
    RememberProcessedLayoutObject(object);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 128) {
        void* next = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(child) + 0xF8);
        ScaleTitleTutorialSubtree(child, depth + 1);
        child = next;
    }
}

void ResetTitleTutorialBubbleGeometry(void* bubble) {
    if (g_title_tutorial_bubble == bubble) {
        return;
    }
    g_title_tutorial_bubble = bubble;
    g_title_tutorial_bubble_geometry_count = 0;
}

void ScaleTitleTutorialBubbleSubtree(void* object, int depth = 0) {
    if (!CanReadGuiObject(object) || depth > 4 || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int& width = *reinterpret_cast<int*>(bytes + 0x88);
    int& height = *reinterpret_cast<int*>(bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    TitleNativeGeometry* native = RememberGeometry(
        g_title_tutorial_bubble_geometry,
        std::size(g_title_tutorial_bubble_geometry),
        g_title_tutorial_bubble_geometry_count, object);
    if (native) {
        width = MulDiv(native->width, viewport_width, 800);
        height = MulDiv(native->height, viewport_height, 600);
        const int scaled_x = MulDiv(native->x, viewport_width, 800);
        const int scaled_y = MulDiv(native->y, viewport_height, 600);
        original(object,
            depth == 0 ? viewport_x + scaled_x : scaled_x,
            depth == 0 ? viewport_y + scaled_y : scaled_y);
    }
    RememberProcessedLayoutObject(object);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        void* next = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(child) + 0xF8);
        ScaleTitleTutorialBubbleSubtree(child, depth + 1);
        child = next;
    }
}

void ResetTitleTutorialQuestionGeometry(void* root) {
    if (g_title_tutorial_question_root == root) {
        return;
    }
    g_title_tutorial_question_root = root;
    g_title_tutorial_question_geometry_count = 0;
}

bool IsTitleTutorialQuestionDescendant(void* object) {
    void* cursor = object;
    for (int depth = 0; CanReadGuiObject(cursor) && depth < 6; ++depth) {
        if (cursor == g_title_tutorial_question_root) {
            return true;
        }
        cursor = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(cursor) + 0xF0);
    }
    return false;
}

void ScaleTitleTutorialQuestionSubtree(void* object, int depth = 0) {
    if (!CanReadGuiObject(object) || depth > 4 || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int& width = *reinterpret_cast<int*>(bytes + 0x88);
    int& height = *reinterpret_cast<int*>(bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    if (depth == 0) {
        ResetTitleTutorialQuestionGeometry(object);
        width = viewport_width;
        height = viewport_height;
        original(object, viewport_x, viewport_y);
    } else {
        TitleNativeGeometry* native = RememberGeometry(
            g_title_tutorial_question_geometry,
            std::size(g_title_tutorial_question_geometry),
            g_title_tutorial_question_geometry_count, object);
        if (native) {
            width = MulDiv(native->width, viewport_width, 800);
            height = MulDiv(native->height, viewport_height, 600);
            original(object,
                MulDiv(native->x, viewport_width, 800),
                MulDiv(native->y, viewport_height, 600));
        }
    }
    RememberProcessedLayoutObject(object);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        void* next = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(child) + 0xF8);
        ScaleTitleTutorialQuestionSubtree(child, depth + 1);
        child = next;
    }
}

bool IsPhotoAlbumScreenVisible() {
    if (!CanReadGuiObject(g_unified_ui.photo_album_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_unified_ui.photo_album_root) +
        0x99) != 0;
}

bool IsPhotoAlbumCGVisible() {
    if (!IsPhotoAlbumScreenVisible()) {
        return false;
    }
    auto* root_bytes =
        static_cast<unsigned char*>(g_unified_ui.photo_album_root);
    void* canvas = *reinterpret_cast<void**>(root_bytes + 0xF4);
    if (!CanReadGuiObject(canvas)) {
        return false;
    }
    auto* canvas_bytes = static_cast<unsigned char*>(canvas);
    const int x = *reinterpret_cast<int*>(canvas_bytes + 0x80);
    const int y = *reinterpret_cast<int*>(canvas_bytes + 0x84);
    const int width = *reinterpret_cast<int*>(canvas_bytes + 0x88);
    const int height = *reinterpret_cast<int*>(canvas_bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    return *(canvas_bytes + 0x99) != 0 && x == 0 && y == 0 &&
        width == viewport_width && height == viewport_height;
}

bool IsPhotoAlbumViewportNeeded() {
    const ULONGLONG now = GetTickCount64();
    if (IsPhotoAlbumScreenVisible()) {
        g_unified_ui.last_photo_album_visible_tick = now;
        return true;
    }

    // Changing the selected portrait briefly hides the album root while its
    // page contents are rebuilt. Keep the aspect-fit viewport through that
    // short gap so one legacy 800x600 frame cannot flash between two album
    // frames. A real exit still restores the normal viewport shortly after.
    constexpr ULONGLONG kAlbumTransitionGraceMs = 900;
    return g_unified_ui.photo_album_viewport_active &&
        g_unified_ui.last_photo_album_visible_tick != 0 &&
        now - g_unified_ui.last_photo_album_visible_tick <=
            kAlbumTransitionGraceMs;
}

bool TransformPhotoAlbumCarouselFrame(void* object, void* call_site,
                                      int current_x, int current_y,
                                      int viewport_width, int viewport_height,
                                      int& x, int& y) {
    HMODULE executable = GetModuleHandleW(nullptr);
    const bool intermediate = executable && call_site ==
        static_cast<unsigned char*>(static_cast<void*>(executable)) +
            0x118AC3;

    UnifiedUILayoutState::PhotoAlbumAnimationSequence* sequence = nullptr;
    for (auto& candidate : g_unified_ui.photo_album_sequences) {
        if (candidate.object == object) {
            sequence = &candidate;
            break;
        }
        if (!sequence && candidate.object == nullptr) {
            sequence = &candidate;
        }
    }

    if (!intermediate) {
        if (sequence) {
            sequence->object = object;
            sequence->active = false;
        }
        x = MulDiv(x, viewport_width, 800);
        y = MulDiv(y, viewport_height, 600);

        bool any_active = false;
        for (const auto& candidate : g_unified_ui.photo_album_sequences) {
            any_active |= candidate.active;
        }
        if (!any_active) {
            g_unified_ui.photo_album_animation_direction = 0;
        }
        return false;
    }

    if (!sequence) {
        x = MulDiv(x, viewport_width, 800);
        y = MulDiv(y, viewport_height, 600);
        return false;
    }
    if (!sequence->object) {
        sequence->object = object;
    }
    if (!sequence->active) {
        sequence->active = true;
        sequence->source_x = current_x;
        sequence->source_y = current_y;
        if (g_unified_ui.photo_album_animation_direction == 0) {
            // Portraits are updated from left to right. The leftmost portrait
            // uniquely reveals the carousel direction because its native
            // target lies on the opposite side of its scaled start point.
            g_unified_ui.photo_album_animation_direction =
                x >= current_x ? 1 : -1;
        }
        const int native_source_x = MulDiv(current_x, 800, viewport_width);
        sequence->target_native_x = native_source_x +
            g_unified_ui.photo_album_animation_direction * 60;
        sequence->target_native_y = MulDiv(
            current_y, 600, viewport_height);
    }

    // The game's interpolator mixes a scaled source with a native target.
    // Recover its progress in that mixed space, then apply the same progress
    // between two consistently scaled endpoints.
    const int mixed_denominator =
        sequence->target_native_x - sequence->source_x;
    double progress = mixed_denominator != 0 ?
        static_cast<double>(x - sequence->source_x) /
            mixed_denominator : 1.0;
    progress = std::clamp(progress, 0.0, 1.0);
    const int target_x = MulDiv(
        sequence->target_native_x, viewport_width, 800);
    const int target_y = MulDiv(
        sequence->target_native_y, viewport_height, 600);
    x = static_cast<int>(std::lround(
        sequence->source_x +
        (target_x - sequence->source_x) * progress));
    y = static_cast<int>(std::lround(
        sequence->source_y +
        (target_y - sequence->source_y) * progress));
    return true;
}

void ScalePhotoAlbumSubtree(void* object, int depth = 0) {
    if (!CanReadGuiObject(object) || depth > 8 || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int& x = *reinterpret_cast<int*>(bytes + 0x80);
    int& y = *reinterpret_cast<int*>(bytes + 0x84);
    int& width = *reinterpret_cast<int*>(bytes + 0x88);
    int& height = *reinterpret_cast<int*>(bytes + 0x8C);
    int viewport_x = 0;
    int viewport_y = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
        viewport_x, viewport_y, viewport_width, viewport_height);
    if (depth == 0) {
        width = viewport_width;
        height = viewport_height;
        original(object, viewport_x, viewport_y);
    } else {
        const int scaled_x = MulDiv(x, viewport_width, 800);
        const int scaled_y = MulDiv(y, viewport_height, 600);
        width = MulDiv(width, viewport_width, 800);
        height = MulDiv(height, viewport_height, 600);
        original(object, scaled_x, scaled_y);
    }
    RememberProcessedLayoutObject(object);

    void* child = *reinterpret_cast<void**>(bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 512) {
        void* next = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(child) + 0xF8);
        ScalePhotoAlbumSubtree(child, depth + 1);
        child = next;
    }
}

void TransformScheduleJobHoverTooltip(int& x, int& y) {
    const int center_x =
        (static_cast<int>(g_unified_ui.width) - 800) / 2;
    const int center_y =
        (static_cast<int>(g_unified_ui.height) - 600) / 2;
    // Artist-column legacy positions end at x=595. At 1920x1080 their
    // translated positions begin at x=685. Keeping a small gap around the
    // center offset makes this idempotent when the game has already converted
    // the detail card's x to output space.
    if (x < center_x + 90) {
        x += center_x;
    }
    y += center_y;
}

void TransformCenteredScheduleOverlay(int& x, int& y) {
    // The picker is horizontally attached to the selected artist column. The
    // game therefore supplies several different legacy x positions; forcing
    // x=430 pins it to the third artist. Translate an incoming legacy x once,
    // while accepting an x that is already in the centered output canvas so
    // repeated animation/layout callbacks cannot accumulate the offset.
    const int center_x = (static_cast<int>(g_unified_ui.width) - 800) / 2;
    if (x < center_x + 90) {
        x += center_x;
    }
    // Its vertical animation/staging coordinates are not useful after the
    // schedule page is centered; keep the settled legacy baseline instead.
    y = 167 + (static_cast<int>(g_unified_ui.height) - 600) / 2;
}

bool IsMapDynamicDisplayProxy(void* object, void* parent,
                              int width, int height) {
    // The city billboards are fed by two square GUI render canvases. They are
    // moved between an off-screen staging origin and an output-space atlas
    // position every time their image changes. Treating these canvases as
    // ordinary root UI adds the widescreen offset a second time and clips the
    // billboard texture to a black panel with a narrow image strip.
    return object && parent == g_unified_ui.primary_root &&
        width >= 508 && width <= 516 && height >= 508 && height <= 516;
}

bool IsSmallDialoguePortrait(void* object, void* parent,
                             int width, int height) {
    return object && parent == g_unified_ui.primary_root &&
        width >= 120 && width <= 128 && height >= 140 && height <= 148;
}

void PlaceCenteredLegacyOverlay(int legacy_x, int legacy_y,
                                int& x, int& y) {
    x = legacy_x + (static_cast<int>(g_unified_ui.width) - 800) / 2;
    y = legacy_y + (static_cast<int>(g_unified_ui.height) - 600) / 2;
}

void TransformTitleTutorialProfileDropdown(void* object, int& x, int& y) {
    if (IsProcessedLayoutObject(object) && CanReadGuiObject(object)) {
        auto* bytes = static_cast<unsigned char*>(object);
        const int current_x = *reinterpret_cast<int*>(bytes + 0x80);
        const int current_y = *reinterpret_cast<int*>(bytes + 0x84);
        if (x == current_x && y == current_y) {
            return;
        }
    }
    if (x < 0 || x > 800 || y < 0 || y > 600) {
        return;
    }
    x += (static_cast<int>(g_unified_ui.width) - 800) / 2;
    y += (static_cast<int>(g_unified_ui.height) - 600) / 2;
}

// The visible announcement control layer is a centered 800x600 root with
// three caption objects and the four 100x120 result cards.  Its textured
// background and preview panes are submitted separately as raw XYZRHW draws,
// so this signature is used to enable the targeted render-layer correction.
bool IsAnnouncementControlRoot(void* object, void* parent,
                               int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 790 || width > 810 || height < 590 || height > 610) {
        return false;
    }

    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }

    size_t captions = 0;
    size_t result_cards = 0;
    size_t count = 0;
    void* child = *reinterpret_cast<void**>(
        bytes + 0xF4);
    while (CanReadGuiObject(child) && count < 20) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_x = *reinterpret_cast<int*>(child_bytes + 0x80);
        const int child_y = *reinterpret_cast<int*>(child_bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        if (child_width >= 140 && child_width <= 150 &&
            child_height >= 18 && child_height <= 22) {
            ++captions;
        }
        if (child_y >= 465 && child_y <= 485 &&
            child_width >= 95 && child_width <= 105 &&
            child_height >= 115 && child_height <= 125) {
            ++result_cards;
        }
        ++count;
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }
    return child == nullptr && captions >= 3 && result_cards >= 3;
}

bool IsAnnouncementScreenVisible() {
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        return false;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_unified_ui.primary_root);
    void* child = *reinterpret_cast<void**>(root_bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 4096) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int width = *reinterpret_cast<int*>(child_bytes + 0x88);
        const int height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        if (IsAnnouncementControlRoot(child, g_unified_ui.primary_root,
                                      width, height)) {
            return true;
        }
        child = *reinterpret_cast<void**>(child_bytes + 0xF8);
    }
    return false;
}

bool IsPhoneOverlayRoot(void* object, void* parent,
                        int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width != 0 || height != 0) {
        return false;
    }
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = *reinterpret_cast<int*>(bytes + 0x80);
        const int y = *reinterpret_cast<int*>(bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(bytes + 0x8C);
        if (x >= 0 && x <= 8 && y >= 550 && y <= 565 &&
            child_width == 40 && child_height == 40) {
            return true;
        }
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return false;
}

int CountVisibleToolbarSlots(void* object, void* parent,
                             int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        !((width >= 310 && width <= 330) ||
          (width >= 225 && width <= 235)) ||
        height < 40 || height > 50) {
        return 0;
    }
    int visible_count = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = *reinterpret_cast<int*>(bytes + 0x80);
        const int y = *reinterpret_cast<int*>(bytes + 0x84);
        const int child_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int child_height = *reinterpret_cast<int*>(bytes + 0x8C);
        const bool visible = *reinterpret_cast<unsigned char*>(bytes + 0x99) != 0;
        if (visible && x >= 0 && x <= 320 && y >= 0 && y <= 10 &&
            child_width >= 35 && child_width <= 42 &&
            child_height >= 35 && child_height <= 42) {
            ++visible_count;
        }
        child = *reinterpret_cast<void**>(bytes + 0xF8);
    }
    return visible_count;
}

bool IsFiveSlotToolbar(void* object, void* parent,
                       int width, int height) {
    return CountVisibleToolbarSlots(object, parent, width, height) == 5;
}

bool IsSevenSlotToolbar(void* object, void* parent,
                        int width, int height) {
    return CountVisibleToolbarSlots(object, parent, width, height) == 7;
}

bool IsToolbarAnimationObject(void* parent, int width, int height,
                              int x, int y) {
    (void)x;
    (void)y;
    if (parent != g_unified_ui.primary_root) {
        return false;
    }
    const bool toolbar = ((width >= 310 && width <= 330) ||
                          (width >= 225 && width <= 235)) &&
        height >= 40 && height <= 50;
    const bool toggle = width >= 25 && width <= 32 &&
        height >= 28 && height <= 35;
    return toolbar || toggle;
}

void TransformToolbarAnimationPosition(int width, int height, int& x, int& y,
                                       bool force_legacy = false) {
    (void)width;
    (void)height;
    const int expansion_x = static_cast<int>(g_unified_ui.width) - 800;
    const int expansion_y = static_cast<int>(g_unified_ui.height) - 600;

    // The animation controller supplies every intermediate point in the
    // original 800x600 coordinate system. Translate the complete trajectory
    // by the widescreen expansion instead of snapping it to either endpoint.
    // Coordinates already emitted in output space are left untouched so a
    // periodic reflow cannot accumulate the offset.
    // An incoming legacy animation frame always has an x coordinate in the
    // original 800px canvas. Use x as the coordinate-space discriminator for
    // both axes because the animation's vertical arc can temporarily leave
    // the old 540..700 range.
    if (force_legacy) {
        x += expansion_x;
        y += expansion_y;
    }
}

bool TransformToolbarSequenceFrame(void* object, int width, int height,
                                   int& x, int& y, void* call_site) {
    const uintptr_t executable = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));
    const bool intermediate_frame = reinterpret_cast<uintptr_t>(call_site) ==
        executable + 0x118AC3;
    const bool open_endpoint = reinterpret_cast<uintptr_t>(call_site) ==
        executable + 0x118A4B;

    UnifiedUILayoutState::ToolbarAnimationSequence* sequence = nullptr;
    for (auto& candidate : g_unified_ui.toolbar_sequences) {
        if (candidate.object == object) {
            sequence = &candidate;
            break;
        }
        if (!sequence && candidate.object == nullptr) {
            sequence = &candidate;
        }
    }
    if (!sequence) {
        TransformToolbarAnimationPosition(
            width, height, x, y, x < 1000);
        return false;
    }
    if (!sequence->object) {
        sequence->object = object;
    }

    if (!intermediate_frame) {
        sequence->active = false;
        sequence->last_open = open_endpoint;
        TransformToolbarAnimationPosition(
            width, height, x, y, x < 1000);
        return false;
    }
    if (!sequence->active) {
        sequence->active = true;
        sequence->legacy_space = x < 1000;
        sequence->target_width = g_unified_ui.toolbar_width;
        sequence->closing = sequence->last_open;
        const bool toolbar = (width >= 225 && width <= 330) &&
            height >= 40 && height <= 50;
        sequence->source_start_x = x;
    }

    const bool toolbar = (width >= 225 && width <= 330) &&
        height >= 40 && height <= 50;
    const int active_width = sequence->target_width;
    const int source_end_x = sequence->legacy_space ?
        (toolbar ? 800 - active_width : 800 - active_width - 28) :
        (toolbar ? 800 - active_width : 800 - active_width - 28);
    const int denominator = std::max(
        1, sequence->source_start_x - source_end_x);
    const double progress = std::clamp(
        static_cast<double>(sequence->source_start_x - x) / denominator,
        0.0, 1.0);
    const int closed_x = toolbar ?
        static_cast<int>(g_unified_ui.width) :
        static_cast<int>(g_unified_ui.width) - 28;
    const int travel = static_cast<int>(active_width * progress + 0.5);
    x = sequence->closing ?
        closed_x - active_width + travel : closed_x - travel;
    y = toolbar ? static_cast<int>(g_unified_ui.height) - height :
                  static_cast<int>(g_unified_ui.height) - 32;
    return true;
}

void ReflowExistingRootChildren(void* root, int depth = 0) {
    if (!CanReadGuiObject(root) || depth > 8 || !g_unified_ui.trampoline) {
        return;
    }
    if (root == g_unified_ui.photo_album_root) {
        return;
    }
    if (root == g_unified_ui.primary_root) {
        DiscoverInGameCGSurfaces(root);
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* root_bytes = static_cast<unsigned char*>(root);
    void* child = *reinterpret_cast<void**>(root_bytes + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 4096) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        void* next = *reinterpret_cast<void**>(child_bytes + 0xF8);
        int& x = *reinterpret_cast<int*>(child_bytes + 0x80);
        int& y = *reinterpret_cast<int*>(child_bytes + 0x84);
        int& width = *reinterpret_cast<int*>(child_bytes + 0x88);
        int& height = *reinterpret_cast<int*>(child_bytes + 0x8C);
        const bool legacy_canvas = width >= 790 && width <= 810 &&
            height >= 590 && height <= 610;
        const bool known_canvas = IsKnownLayoutRoot(child);
        if (child == g_unified_ui.in_game_cg_root && IsInGameCGVisible()) {
            LayoutInGameCGRoot(child);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_logged) {
                int viewport_x = 0;
                int viewport_y = 0;
                int viewport_width = 0;
                int viewport_height = 0;
                GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                    viewport_x, viewport_y, viewport_width, viewport_height);
                Log("Unified UI in-game CG aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                    child, viewport_x, viewport_y,
                    viewport_width, viewport_height);
                g_unified_ui.in_game_cg_logged = true;
            }
        } else if (child == g_unified_ui.in_game_cg_caption &&
                   IsInGameCGVisible()) {
            int new_x = 0;
            int new_y = 0;
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_caption_logged) {
                Log("Unified UI in-game CG native caption self=%p y=%d -> %d,%d",
                    child, g_unified_ui.in_game_cg_caption_native_y,
                    new_x, new_y);
                g_unified_ui.in_game_cg_caption_logged = true;
            }
        } else if (child == g_unified_ui.in_game_cg_item_notice &&
                   IsInGameCGVisible()) {
            int new_x = 0;
            int new_y = 0;
            GetInGameCGNativeOverlayPosition(
                g_unified_ui.in_game_cg_item_notice_native_x,
                g_unified_ui.in_game_cg_item_notice_native_y,
                new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_item_notice_logged) {
                Log("Unified UI in-game CG native item notice self=%p source=%d,%d -> %d,%d",
                    child,
                    g_unified_ui.in_game_cg_item_notice_native_x,
                    g_unified_ui.in_game_cg_item_notice_native_y,
                    new_x, new_y);
                g_unified_ui.in_game_cg_item_notice_logged = true;
            }
        } else if (child == g_unified_ui.title_screen_root &&
                   IsTitleScreenVisible()) {
            // The attract animation can restore the root rectangle or replace
            // individual visual children after the first layout pass. Reapply
            // the stored native geometry idempotently once per refresh.
            ScaleTitleScreenSubtree(child);
            RememberProcessedLayoutObject(child);
        } else if (IsTitleScreenRoot(child, root, width, height)) {
            if (g_unified_ui.title_screen_root != child) {
                ResetTitleNativeGeometry(child);
            }
            g_unified_ui.title_screen_root = child;
            RememberLayoutRoot(child);
            ScaleTitleScreenSubtree(child);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.title_screen_logged) {
                int viewport_x = 0;
                int viewport_y = 0;
                int viewport_width = 0;
                int viewport_height = 0;
                GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                    viewport_x, viewport_y, viewport_width, viewport_height);
                Log("Unified UI title screen aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                    child, viewport_x, viewport_y,
                    viewport_width, viewport_height);
                g_unified_ui.title_screen_logged = true;
            }
        } else if (child == g_title_tutorial_root) {
            ScaleTitleTutorialSubtree(child);
            RememberProcessedLayoutObject(child);
        } else if (IsTitleTutorialPage(child, root, width, height)) {
            ResetTitleTutorialGeometry(child);
            RememberLayoutRoot(child);
            ScaleTitleTutorialSubtree(child);
            RememberProcessedLayoutObject(child);
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            Log("Unified UI title tutorial aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                child, viewport_x, viewport_y,
                viewport_width, viewport_height);
        } else if (child == g_title_tutorial_question_root) {
            ScaleTitleTutorialQuestionSubtree(child);
            RememberProcessedLayoutObject(child);
        } else if (IsTitleTutorialQuestionPage(
                       child, root, width, height)) {
            ResetTitleTutorialQuestionGeometry(child);
            RememberLayoutRoot(child);
            ScaleTitleTutorialQuestionSubtree(child);
            RememberProcessedLayoutObject(child);
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            Log("Unified UI title tutorial question aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                child, viewport_x, viewport_y,
                viewport_width, viewport_height);
        } else if (IsPhotoAlbumRoot(child, root, width, height)) {
            g_unified_ui.photo_album_root = child;
            RememberLayoutRoot(child);
            ScalePhotoAlbumSubtree(child);
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            Log("Unified UI photo album aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                child, viewport_x, viewport_y,
                viewport_width, viewport_height);
        } else if (IsFullscreenLeafSurface(child, root, width, height)) {
            const int old_width = width;
            const int old_height = height;
            width = static_cast<int>(g_unified_ui.width);
            height = static_cast<int>(g_unified_ui.height);
            original(child, 0, 0);
            RememberProcessedLayoutObject(child);
            Log("Unified UI fullscreen leaf self=%p rect=0,0 %dx%d -> %ux%u",
                child, old_width, old_height,
                g_unified_ui.width, g_unified_ui.height);
        } else if (IsPhoneOverlayRoot(child, root, width, height)) {
            RememberLayoutRoot(child);
            ReflowExistingRootChildren(child, depth + 1);
        } else if (IsSevenSlotToolbar(child, root, width, height)) {
            if (g_unified_ui.toolbar_width != 320) {
                Log("Unified UI toolbar mode=7 width=320");
            }
            g_unified_ui.toolbar_width = 320;
            width = 320;
            original(child,
                static_cast<int>(g_unified_ui.width) - width,
                static_cast<int>(g_unified_ui.height) - height);
            RememberProcessedLayoutObject(child);
        } else if (IsFiveSlotToolbar(child, root, width, height)) {
            if (g_unified_ui.toolbar_width != 230) {
                Log("Unified UI toolbar mode=5 width=230");
            }
            g_unified_ui.toolbar_width = 230;
            width = 230;
            original(child,
                static_cast<int>(g_unified_ui.width) - width,
                static_cast<int>(g_unified_ui.height) - height);
            RememberProcessedLayoutObject(child);
        } else if (IsToolbarAnimationObject(root, width, height, x, y)) {
            if (!IsProcessedLayoutObject(child)) {
                int new_x = x;
                int new_y = y;
                TransformToolbarAnimationPosition(
                    width, height, new_x, new_y, new_x < 1000);
                original(child, new_x, new_y);
                RememberProcessedLayoutObject(child);
            }
        } else if (legacy_canvas) {
            size_t child_count = 0;
            void* only_child = *reinterpret_cast<void**>(child_bytes + 0xF4);
            void* cursor = only_child;
            while (CanReadGuiObject(cursor) && child_count < 3) {
                ++child_count;
                cursor = *reinterpret_cast<void**>(
                    static_cast<unsigned char*>(cursor) + 0xF8);
            }
            if (child_count == 0) {
                child = next;
                continue;
            }

            auto* only_bytes = static_cast<unsigned char*>(only_child);
            const int only_x = *reinterpret_cast<int*>(only_bytes + 0x80);
            const int only_y = *reinterpret_cast<int*>(only_bytes + 0x84);
            const int only_width = *reinterpret_cast<int*>(only_bytes + 0x88);
            const int only_height = *reinterpret_cast<int*>(only_bytes + 0x8C);
            const bool small_single_overlay = child_count == 1 &&
                only_width >= 0 && only_width < 400 &&
                only_height >= 0 && only_height < 400;

            if (!small_single_overlay) {
                RememberGroupCanvas(child);
                RememberProcessedLayoutObject(child);
                const int page_x = (static_cast<int>(g_unified_ui.width) - 800) / 2;
                const int page_y = (static_cast<int>(g_unified_ui.height) - 600) / 2;
                original(child, page_x, page_y);
                child = next;
                continue;
            }

            if (legacy_canvas) {
                width = static_cast<int>(g_unified_ui.width);
                height = static_cast<int>(g_unified_ui.height);
                RememberLayoutRoot(child);
                const int center_x_twice = only_x * 2 + only_width;
                const int center_y_twice = only_y * 2 + only_height;
                const bool central_overlay = center_x_twice >= (800 * 2) / 3 &&
                    center_x_twice <= (800 * 4) / 3 &&
                    center_y_twice >= (600 * 2) / 3 &&
                    center_y_twice <= (600 * 4) / 3;
                if (central_overlay) {
                    RememberWorldRoot(child);
                }
            }
            ReflowExistingRootChildren(child, depth + 1);
        } else if (known_canvas) {
            ReflowExistingRootChildren(child, depth + 1);
        } else if (IsMapDynamicDisplayProxy(child, root, width, height)) {
            RememberProcessedLayoutObject(child);
        } else if (IsWorldInteractionCanvas(child, root, width, height) ||
                   IsRadialInteractionWheel(child, root, width, height) ||
                   IsMapLocationLabel(child, root, width, height) ||
                   IsWorldStatusBubble(child, root, width, height) ||
                   IsWorldProjectedObject(child)) {
            // Preserve the game's own projected coordinates.  In particular,
            // do not turn the legacy x=340 interaction canvas into x=1460.
            if (!IsProcessedLayoutObject(child)) {
                RememberWorldProjectedObject(child);
                RememberProcessedLayoutObject(child);
                Log("Unified UI world interaction passthrough self=%p parent=%p rect=%d,%d %dx%d",
                    child, root, x, y, width, height);
            }
        } else if (IsEventPublicationPanel(child, root, width, height)) {
            if (!IsProcessedLayoutObject(child)) {
                const int new_x = x +
                    (static_cast<int>(g_unified_ui.width) - 800) / 2;
                const int new_y = y +
                    (static_cast<int>(g_unified_ui.height) - 600) / 2;
                original(child, new_x, new_y);
                RememberProcessedLayoutObject(child);
            }
        } else if (!IsProcessedLayoutObject(child) &&
                   IsScheduleJobHoverTooltip(child, root, width, height)) {
            int new_x = x;
            int new_y = y;
            TransformScheduleJobHoverTooltip(new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
        } else if (IsScheduleSecondaryPanel(child, root, width, height)) {
            int new_x = x;
            int new_y = y;
            TransformCenteredScheduleOverlay(new_x, new_y);
            if (new_x != x || new_y != y) {
                original(child, new_x, new_y);
            }
            RememberProcessedLayoutObject(child);
        } else if (IsSmallDialoguePortrait(child, root, width, height)) {
            int new_x = x;
            int new_y = y;
            PlaceCenteredLegacyOverlay(95, 403, new_x, new_y);
            if (new_x != x || new_y != y) {
                original(child, new_x, new_y);
            }
            RememberProcessedLayoutObject(child);
        } else if (child == g_title_tutorial_bubble ||
                   IsTitleTutorialDialogueBubble(
                       child, root, width, height, x, y)) {
            const bool newly_discovered = child != g_title_tutorial_bubble;
            const int old_x = x;
            const int old_y = y;
            const int old_width = width;
            const int old_height = height;
            ResetTitleTutorialBubbleGeometry(child);
            ScaleTitleTutorialBubbleSubtree(child);
            if (newly_discovered) {
                Log("Unified UI title tutorial dialogue self=%p rect=%d,%d %dx%d -> %d,%d %dx%d",
                    child, old_x, old_y, old_width, old_height,
                    x, y, width, height);
            }
        } else if (IsTitleTutorialProfileDropdown(
                       child, root, width, height)) {
            TransformTitleTutorialProfileDropdown(child, x, y);
            original(child, x, y);
            RememberProcessedLayoutObject(child);
        } else if (!IsProcessedLayoutObject(child) &&
                   IsSmallWorldDialogueBubble(child, root, width, height)) {
            const int old_x = x;
            const int old_y = y;
            int new_x = x;
            int new_y = y;
            TransformWorldAnchorPosition(new_x, new_y);
            if (new_x != old_x || new_y != old_y) {
                original(child, new_x, new_y);
                Log("Unified UI world dialogue bubble self=%p rect=%d,%d %dx%d -> %d,%d",
                    child, old_x, old_y, width, height, new_x, new_y);
            }
            RememberProcessedLayoutObject(child);
        } else if (!IsProcessedLayoutObject(child) &&
                   width >= 0 && height >= 0 && width < 760 && height < 560 &&
                   x >= 0 && x <= 800 && y >= 0 && y <= 600) {
            const int old_x = x;
            const int old_y = y;
            int new_x = x;
            int new_y = y;
            if (IsWorldRoot(root)) {
                TransformWorldPosition(width, height, new_x, new_y);
            } else {
                TransformRootChildPosition(width, height, new_x, new_y);
            }
            if (new_x != old_x || new_y != old_y) {
                original(child, new_x, new_y);
                const LONG count = InterlockedIncrement(&g_unified_ui.transformed_count);
                if (count <= 160) {
                    Log("Unified UI existing #%ld self=%p parent=%p rect=%d,%d %dx%d -> %d,%d",
                        count, child, root, old_x, old_y, width, height, new_x, new_y);
                }
            }
            RememberProcessedLayoutObject(child);
        }
        child = next;
    }
}

void RefreshUnifiedUILayout() {
    if (!g_unified_ui.installed || !g_unified_ui.primary_root) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    UpdateTitleStripMotion(now);
    // Before the title root is known, scan on the first draw submissions so
    // the page is aspect-fitted before its first visible frame. Once found,
    // return to the low-frequency maintenance pass used by ordinary screens.
    const bool title_discovery_pending =
        g_unified_ui.title_screen_mode != 0 &&
        g_unified_ui.title_screen_root == nullptr;
    if (!title_discovery_pending &&
        now - g_unified_ui.last_refresh_tick < 1000) {
        return;
    }
    g_unified_ui.last_refresh_tick = now;
    ReflowExistingRootChildren(g_unified_ui.primary_root);

    const bool announcement_active = IsAnnouncementScreenVisible();
    if (announcement_active != g_unified_ui.announcement_active) {
        g_unified_ui.announcement_active = announcement_active;
        Log("Unified UI announcement render layer active=%d",
            announcement_active ? 1 : 0);
    }
}

void RefreshInGameCGOverlays() {
    if (!g_unified_ui.installed ||
        !CanReadGuiObject(g_unified_ui.primary_root) ||
        !g_unified_ui.trampoline) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now == g_unified_ui.last_in_game_cg_overlay_refresh_tick) {
        return;
    }
    g_unified_ui.last_in_game_cg_overlay_refresh_tick = now;
    DiscoverInGameCGSurfaces(g_unified_ui.primary_root);
    if (!IsInGameCGVisible()) {
        return;
    }

    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    LayoutInGameCGRoot(g_unified_ui.in_game_cg_root);
    if (CanReadGuiObject(g_unified_ui.in_game_cg_caption)) {
        auto* caption_bytes = static_cast<unsigned char*>(
            g_unified_ui.in_game_cg_caption);
        if (*(caption_bytes + 0x99) != 0) {
            int x = 0;
            int y = 0;
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, x, y);
            original(g_unified_ui.in_game_cg_caption, x, y);
        }
    }
    if (CanReadGuiObject(g_unified_ui.in_game_cg_item_notice)) {
        auto* notice_bytes = static_cast<unsigned char*>(
            g_unified_ui.in_game_cg_item_notice);
        if (*(notice_bytes + 0x99) != 0) {
            int x = 0;
            int y = 0;
            GetInGameCGNativeOverlayPosition(
                g_unified_ui.in_game_cg_item_notice_native_x,
                g_unified_ui.in_game_cg_item_notice_native_y, x, y);
            original(g_unified_ui.in_game_cg_item_notice, x, y);
        }
    }
    for (auto& overlay : g_unified_ui.in_game_cg_native_overlays) {
        if (!CanReadGuiObject(overlay.object) ||
            overlay.object == g_unified_ui.in_game_cg_caption ||
            overlay.object == g_unified_ui.in_game_cg_item_notice) {
            continue;
        }
        auto* overlay_bytes = static_cast<unsigned char*>(overlay.object);
        void* parent = *reinterpret_cast<void**>(overlay_bytes + 0xF0);
        const int width = *reinterpret_cast<int*>(overlay_bytes + 0x88);
        const int height = *reinterpret_cast<int*>(overlay_bytes + 0x8C);
        if (*(overlay_bytes + 0x99) == 0 ||
            !IsInGameCGNativeRootStrip(
                overlay.object, parent, width, height)) {
            continue;
        }
        int x = 0;
        int y = 0;
        GetInGameCGNativeOverlayPosition(
            overlay.native_x, overlay.native_y, x, y);
        original(overlay.object, x, y);
        if (!overlay.logged) {
            Log("Unified UI in-game CG native strip self=%p source=%d,%d %dx%d -> %d,%d",
                overlay.object, overlay.native_x, overlay.native_y,
                overlay.width, overlay.height, x, y);
            overlay.logged = true;
        }
    }
}

void __fastcall HookGuiMove(void* self, void*, int x, int y) {
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    if (!original || !self) {
        return;
    }

    auto* bytes = static_cast<unsigned char*>(self);
    void* immediate_call = static_cast<unsigned char*>(_ReturnAddress()) - 5;
    void* world_source_return = nullptr;
    HMODULE executable = GetModuleHandleW(nullptr);
    if (executable && immediate_call ==
            static_cast<unsigned char*>(static_cast<void*>(executable)) +
                0xD7125) {
        // Stardom3's wrapper at 004D7100 pushes x/y and calls GuiMove at
        // 004D7125. At GuiMove entry its own caller's return address is the
        // fourth stack slot: [return-to-wrapper, x, y, source-return].
        auto** return_slot = reinterpret_cast<void**>(_AddressOfReturnAddress());
        world_source_return = return_slot[3];
    }
    void* parent = *reinterpret_cast<void**>(bytes + 0xF0);
    if (self == g_unified_ui.title_screen_root) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        *reinterpret_cast<int*>(bytes + 0x88) = viewport_width;
        *reinterpret_cast<int*>(bytes + 0x8C) = viewport_height;
        original(self, viewport_x, viewport_y);
        return;
    }
    if (parent && IsTitleScreenDescendant(self)) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        TitleNativeGeometry* native = RememberTitleNativeGeometry(self);
        if (native) {
            *reinterpret_cast<int*>(bytes + 0x88) =
                MulDiv(native->width, viewport_width, 800);
            *reinterpret_cast<int*>(bytes + 0x8C) =
                MulDiv(native->height, viewport_height, 600);
        }
        if (IsTitleStripGeometry(native)) {
            // The original title controller keeps one primary native x and
            // derives the paired strip from it every update:
            //   primary += direction * 2, wrapping at +/-1000;
            //   follower = primary < 0 ? primary + 2000 : primary - 2000.
            // Reading the transformed object x back into that controller moves
            // its wrap boundary and breaks the pair. Reproduce the exact native
            // state machine here, then transform both absolute native outputs
            // once. The two authored strips therefore retain their identities,
            // speed, separation, direction changes, and hand-off order.
            const int input_x = x;
            const int current_x = *reinterpret_cast<int*>(bytes + 0x80);
            const bool lead_strip = native->x < 1000;
            RegisterTitleStrip(self, native);
            if (lead_strip) {
                const long long observed_delta =
                    static_cast<long long>(input_x) - current_x;
                if (observed_delta >= 1 && observed_delta <= 4) {
                    g_title_strip_motion.native_step = 2;
                } else if (observed_delta <= -1 && observed_delta >= -4) {
                    g_title_strip_motion.native_step = -2;
                } else if (input_x <= -990 && input_x >= -1010 &&
                           current_x > 0) {
                    g_title_strip_motion.native_step = 2;
                } else if (input_x >= 990 && input_x <= 1010 &&
                           current_x < 0) {
                    g_title_strip_motion.native_step = -2;
                }
            }
            const int native_x = lead_strip ?
                g_title_strip_motion.primary_native_x :
                g_title_strip_motion.follower_native_x;
            x = MulDiv(native_x, viewport_width, 800);
            y = MulDiv(native->y, viewport_height, 600);
            original(self, x, y);
            return;
        }
        x = MulDiv(x, viewport_width, 800);
        y = MulDiv(y, viewport_height, 600);
        original(self, x, y);
        return;
    }
    if (self == g_title_tutorial_root) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        *reinterpret_cast<int*>(bytes + 0x88) = viewport_width;
        *reinterpret_cast<int*>(bytes + 0x8C) = viewport_height;
        original(self, viewport_x, viewport_y);
        return;
    }
    if (parent && IsTitleTutorialDescendant(self)) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        TitleNativeGeometry* native = RememberGeometry(
            g_title_tutorial_geometry,
            std::size(g_title_tutorial_geometry),
            g_title_tutorial_geometry_count, self);
        if (native) {
            *reinterpret_cast<int*>(bytes + 0x88) =
                MulDiv(native->width, viewport_width, 800);
            *reinterpret_cast<int*>(bytes + 0x8C) =
                MulDiv(native->height, viewport_height, 600);
            original(self,
                MulDiv(native->x, viewport_width, 800),
                MulDiv(native->y, viewport_height, 600));
            return;
        }
    }
    if (self == g_title_tutorial_question_root) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        *reinterpret_cast<int*>(bytes + 0x88) = viewport_width;
        *reinterpret_cast<int*>(bytes + 0x8C) = viewport_height;
        original(self, viewport_x, viewport_y);
        return;
    }
    if (parent && IsTitleTutorialQuestionDescendant(self)) {
        int viewport_x = 0;
        int viewport_y = 0;
        int viewport_width = 0;
        int viewport_height = 0;
        GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
            viewport_x, viewport_y, viewport_width, viewport_height);
        TitleNativeGeometry* native = RememberGeometry(
            g_title_tutorial_question_geometry,
            std::size(g_title_tutorial_question_geometry),
            g_title_tutorial_question_geometry_count, self);
        if (native) {
            *reinterpret_cast<int*>(bytes + 0x88) =
                MulDiv(native->width, viewport_width, 800);
            *reinterpret_cast<int*>(bytes + 0x8C) =
                MulDiv(native->height, viewport_height, 600);
            original(self,
                MulDiv(native->x, viewport_width, 800),
                MulDiv(native->y, viewport_height, 600));
            return;
        }
    }
    const int current_width = *reinterpret_cast<int*>(bytes + 0x88);
    const int current_height = *reinterpret_cast<int*>(bytes + 0x8C);
    if (self == g_title_tutorial_bubble ||
        IsTitleTutorialDialogueBubble(
            self, parent, current_width, current_height, x, y)) {
        ResetTitleTutorialBubbleGeometry(self);
        ScaleTitleTutorialBubbleSubtree(self);
        return;
    }
    if (parent) {
        auto* parent_bytes = static_cast<unsigned char*>(parent);
        int& parent_width = *reinterpret_cast<int*>(parent_bytes + 0x88);
        int& parent_height = *reinterpret_cast<int*>(parent_bytes + 0x8C);
        const bool legacy_root = parent_width >= 790 && parent_width <= 810 &&
            parent_height >= 590 && parent_height <= 610;
        void* grandparent = *reinterpret_cast<void**>(parent_bytes + 0xF0);
        const bool primary_legacy_root = legacy_root && grandparent == nullptr;
        if (primary_legacy_root) {
            RememberLayoutRoot(parent);
            if (!g_unified_ui.primary_root) {
                g_unified_ui.primary_root = parent;
                Log("Unified UI primary root=%p", parent);
            }
            parent_width = static_cast<int>(g_unified_ui.width);
            parent_height = static_cast<int>(g_unified_ui.height);
            ReflowExistingRootChildren(parent);
        } else if (legacy_root && grandparent &&
                   (grandparent == g_unified_ui.primary_root ||
                    IsKnownLayoutRoot(grandparent))) {
            ReflowExistingRootChildren(grandparent);
        }

        if (IsGroupCanvas(parent)) {
            original(self, x, y);
            return;
        }

        const int self_width = *reinterpret_cast<int*>(bytes + 0x88);
        const int self_height = *reinterpret_cast<int*>(bytes + 0x8C);
        const bool possible_cg_overlay = parent == g_unified_ui.primary_root &&
            self_width >= 798 && self_width <= 802 &&
            ((self_height >= 72 && self_height <= 80) ||
             (self_height >= 122 && self_height <= 134));
        if (possible_cg_overlay && !IsInGameCGVisible()) {
            DiscoverInGameCGSurfaces(g_unified_ui.primary_root);
        }
        const bool direct_cg_caption = parent == g_unified_ui.primary_root &&
            self_width >= 798 && self_width <= 802 &&
            self_height >= 72 && self_height <= 80;
        if (IsInGameCGVisible() && direct_cg_caption) {
            if (self != g_unified_ui.in_game_cg_caption) {
                g_unified_ui.in_game_cg_caption = self;
                if (y >= 0 && y <= 600) {
                    g_unified_ui.in_game_cg_caption_native_y = y;
                }
                g_unified_ui.in_game_cg_caption_logged = false;
            }
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, x, y);
            original(self, x, y);
            return;
        }
        const bool direct_cg_item_notice =
            parent == g_unified_ui.primary_root &&
            self_width >= 798 && self_width <= 802 &&
            self_height >= 122 && self_height <= 134;
        if (IsInGameCGVisible() && direct_cg_item_notice) {
            if (self != g_unified_ui.in_game_cg_item_notice) {
                g_unified_ui.in_game_cg_item_notice = self;
                if (x >= -800 && x <= 800 && y >= -200 && y <= 600) {
                    g_unified_ui.in_game_cg_item_notice_native_x = x;
                    g_unified_ui.in_game_cg_item_notice_native_y = y;
                }
                g_unified_ui.in_game_cg_item_notice_logged = false;
            }
            GetInGameCGNativeOverlayPosition(
                g_unified_ui.in_game_cg_item_notice_native_x,
                g_unified_ui.in_game_cg_item_notice_native_y, x, y);
            original(self, x, y);
            return;
        }
        if (IsInGameCGVisible() &&
            IsInGameCGNativeRootStrip(
                self, parent, self_width, self_height)) {
            auto* overlay = FindInGameCGNativeOverlay(self);
            if (!overlay) {
                overlay = RegisterInGameCGNativeOverlay(
                    self, x, y, self_width, self_height);
            }
            if (overlay) {
                GetInGameCGNativeOverlayPosition(
                    overlay->native_x, overlay->native_y, x, y);
                original(self, x, y);
                return;
            }
        }

        if (self == g_unified_ui.in_game_cg_root && IsInGameCGVisible()) {
            LayoutInGameCGRoot(self);
            return;
        }
        if (parent == g_unified_ui.in_game_cg_root &&
            IsInGameCGVisible()) {
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            *reinterpret_cast<int*>(bytes + 0x88) = viewport_width;
            *reinterpret_cast<int*>(bytes + 0x8C) = viewport_height;
            original(self, MulDiv(x, viewport_width, 800),
                MulDiv(y, viewport_height, 600));
            return;
        }
        if (self == g_unified_ui.in_game_cg_caption &&
            IsInGameCGVisible()) {
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, x, y);
            original(self, x, y);
            return;
        }

        if (self == g_unified_ui.photo_album_root) {
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            original(self, viewport_x, viewport_y);
            return;
        }
        if (IsPhotoAlbumDescendant(parent)) {
            const int current_x = *reinterpret_cast<int*>(bytes + 0x80);
            const int current_y = *reinterpret_cast<int*>(bytes + 0x84);
            int viewport_x = 0;
            int viewport_y = 0;
            int viewport_width = 0;
            int viewport_height = 0;
            GetPhotoAlbumViewport(g_unified_ui.width, g_unified_ui.height,
                viewport_x, viewport_y, viewport_width, viewport_height);
            const int incoming_x = x;
            const int incoming_y = y;
            const bool carousel_frame = TransformPhotoAlbumCarouselFrame(
                self, immediate_call, current_x, current_y,
                viewport_width, viewport_height, x, y);
            const LONG album_move_count = InterlockedIncrement(
                &g_unified_ui.photo_album_animation_log_count);
            if (album_move_count <= 120 &&
                (incoming_x != current_x || incoming_y != current_y)) {
                Log("Photo album move #%ld self=%p current=%d,%d incoming=%d,%d -> %d,%d carousel=%d caller=%p",
                    album_move_count, self, current_x, current_y,
                    incoming_x, incoming_y, x, y,
                    carousel_frame ? 1 : 0, immediate_call);
            }
            original(self, x, y);
            return;
        }

        if (primary_legacy_root || IsKnownLayoutRoot(parent)) {
            if (parent == g_unified_ui.primary_root) {
                ReflowExistingRootChildren(parent);
            }
            const int width = *reinterpret_cast<int*>(bytes + 0x88);
            const int height = *reinterpret_cast<int*>(bytes + 0x8C);
            // Full-canvas proxy objects are layout containers, not visible HUD
            // widgets. Keep their origin stable so descendants are not shifted
            // twice. Ordinary root children receive one anchor transform.
            if (IsFullscreenLeafSurface(self, parent, width, height)) {
                *reinterpret_cast<int*>(bytes + 0x88) =
                    static_cast<int>(g_unified_ui.width);
                *reinterpret_cast<int*>(bytes + 0x8C) =
                    static_cast<int>(g_unified_ui.height);
                x = 0;
                y = 0;
                RememberProcessedLayoutObject(self);
            } else if (IsMapDynamicDisplayProxy(self, parent, width, height)) {
                RememberProcessedLayoutObject(self);
                original(self, x, y);
                return;
            } else if (IsWorldInteractionCanvas(self, parent, width, height) ||
                IsRadialInteractionWheel(self, parent, width, height) ||
                IsMapLocationLabel(self, parent, width, height) ||
                IsWorldStatusBubble(self, parent, width, height) ||
                IsWorldProjectedObject(self)) {
                // Its children are positioned relative to the selected actor;
                // leave both initial placement and later game updates intact.
                RememberWorldProjectedObject(self);
                RememberWorldMoveSource(world_source_return);
                RememberProcessedLayoutObject(self);
                if (IsMapLocationLabel(self, parent, width, height)) {
                    const LONG map_count = InterlockedIncrement(
                        &g_unified_ui.map_label_move_count);
                    if (map_count <= 240) {
                        Log("Unified UI map label move #%ld self=%p rect=%d,%d %dx%d caller=%p",
                            map_count, self, x, y, width, height,
                            static_cast<unsigned char*>(_ReturnAddress()) - 5);
                    }
                } else if (IsWorldStatusBubble(self, parent, width, height)) {
                    const LONG bubble_count = InterlockedIncrement(
                        &g_unified_ui.world_status_bubble_move_count);
                    if (bubble_count <= 120) {
                        Log("Unified UI world status bubble move #%ld self=%p rect=%d,%d %dx%d caller=%p",
                            bubble_count, self, x, y, width, height,
                            immediate_call);
                    }
                }
                original(self, x, y);
                return;
            } else if (IsEventPublicationPanel(self, parent, width, height)) {
                // ReflowExistingRootChildren has already translated the
                // panel's resting point. The animation controller then emits
                // absolute frames relative to that translated point, so pass
                // those frames through unchanged to retain the full motion
                // without applying the widescreen offset a second time.
                RememberProcessedLayoutObject(self);
            } else if (IsScheduleJobHoverTooltip(
                           self, parent, width, height)) {
                TransformScheduleJobHoverTooltip(x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsScheduleSecondaryPanel(self, parent, width, height)) {
                TransformCenteredScheduleOverlay(x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsSmallDialoguePortrait(self, parent, width, height)) {
                PlaceCenteredLegacyOverlay(95, 403, x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsTitleTutorialProfileDropdown(
                           self, parent, width, height)) {
                TransformTitleTutorialProfileDropdown(self, x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsSmallWorldDialogueBubble(self, parent, width, height)) {
                const int old_x = x;
                const int old_y = y;
                TransformWorldAnchorPosition(x, y);
                RememberProcessedLayoutObject(self);
                Log("Unified UI world dialogue move self=%p rect=%d,%d %dx%d -> %d,%d caller=%p",
                    self, old_x, old_y, width, height, x, y,
                    static_cast<unsigned char*>(_ReturnAddress()) - 5);
            } else if (IsToolbarAnimationObject(parent, width, height, x, y)) {
                const int old_x = x;
                const int old_y = y;
                void* call_site = static_cast<unsigned char*>(
                    _ReturnAddress()) - 5;
                TransformToolbarSequenceFrame(
                    self, width, height, x, y, call_site);
                RememberProcessedLayoutObject(self);
                const LONG animation_count = InterlockedIncrement(
                    &g_unified_ui.toolbar_animation_log_count);
                if (animation_count <= 120) {
                    Log("Unified UI toolbar animation #%ld self=%p rect=%d,%d %dx%d -> %d,%d caller=%p",
                        animation_count, self, old_x, old_y, width, height,
                        x, y, call_site);
                }
            } else if (IsGroupCanvas(self)) {
                x = (static_cast<int>(g_unified_ui.width) - 800) / 2;
                y = (static_cast<int>(g_unified_ui.height) - 600) / 2;
            } else if (IsWorldMoveSource(world_source_return)) {
                // Learned world-projection sources are a fallback. Known UI
                // surfaces above must keep their specialized transforms even
                // if they happen to be moved by a previously observed caller.
                RememberWorldProjectedObject(self);
                RememberProcessedLayoutObject(self);
                original(self, x, y);
                return;
            } else if (width < 760 && height < 560) {
                const int old_x = x;
                const int old_y = y;
                // The toolbar's collapse button is a separate root child just
                // left of GameMain. Lower-right children therefore share the
                // same group anchor even if a small child's center is middle.
                if (IsWorldRoot(parent)) {
                    TransformWorldPosition(width, height, x, y);
                } else {
                    TransformRootChildPosition(width, height, x, y);
                }
                RememberProcessedLayoutObject(self);
                const LONG count = InterlockedIncrement(&g_unified_ui.transformed_count);
                if (count <= 80 && (x != old_x || y != old_y)) {
                    Log("Unified UI move #%ld self=%p parent=%p rect=%d,%d %dx%d -> %d,%d",
                        count, self, parent, old_x, old_y, width, height, x, y);
                }
            }
        }
    }
    original(self, x, y);
}

bool InstallUnifiedUILayoutHook(UINT width, UINT height, int title_screen_mode) {
    if (g_unified_ui.installed || width < 800 || height < 600) {
        return g_unified_ui.installed;
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x1185A0;
    const unsigned char expected[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Unified UI hook signature mismatch at %p", target);
        LogProbeBytes(target, 16);
        return false;
    }

    constexpr size_t stolen_size = sizeof(expected);
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, stolen_size + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        Log("Unified UI hook trampoline allocation failed error=%lu", GetLastError());
        return false;
    }
    std::memcpy(trampoline, target, stolen_size);
    trampoline[stolen_size] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + stolen_size + 1) =
        static_cast<int32_t>((target + stolen_size) - (trampoline + stolen_size + 5));

    DWORD old_protection = 0;
    if (!VirtualProtect(target, stolen_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        Log("Unified UI hook VirtualProtect failed error=%lu", GetLastError());
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookGuiMove) - (target + 5));
    target[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(target, stolen_size, old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, stolen_size);

    g_unified_ui.width = width;
    g_unified_ui.height = height;
    g_unified_ui.title_screen_mode = title_screen_mode;
    g_unified_ui.trampoline = trampoline;
    g_unified_ui.installed = true;
    Log("Installed unified UI layout hook at %p for %ux%u titleMode=%d",
        target, width, height, title_screen_mode);
    return true;
}

bool PatchMapLocationProjectionBounds(UINT width, UINT height) {
    if (width < 800 || height < 600) {
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }

    struct BoundPatch {
        uintptr_t offset;
        float expected;
        float replacement;
        const char* name;
    };
    const BoundPatch patches[] = {
        {0x2EF36C, 700.0f, static_cast<float>(height + 100), "y extended"},
        {0x2EF370, 900.0f, static_cast<float>(width + 100), "x extended"},
        {0x2EF378, 580.0f, static_cast<float>(height - 20), "y safe"},
        {0x2EF37C, 780.0f, static_cast<float>(width - 20), "x safe"},
    };

    bool all_patched = true;
    for (const auto& patch : patches) {
        auto* target = reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(executable) + patch.offset);
        if (std::fabs(*target - patch.replacement) < 0.01f) {
            continue;
        }
        if (std::fabs(*target - patch.expected) >= 0.01f) {
            Log("Map label bound signature mismatch %s at %p value=%.2f",
                patch.name, target, *target);
            all_patched = false;
            continue;
        }
        DWORD old_protection = 0;
        if (!VirtualProtect(target, sizeof(float), PAGE_READWRITE,
                            &old_protection)) {
            Log("Map label bound VirtualProtect failed %s error=%lu",
                patch.name, GetLastError());
            all_patched = false;
            continue;
        }
        *target = patch.replacement;
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(float), old_protection, &ignored);
        Log("Map label bound patched %s %.0f -> %.0f at %p",
            patch.name, patch.expected, patch.replacement, target);
    }
    return all_patched;
}

bool PatchAirportLocationLabelFilter() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x9CE2D;
    const unsigned char expected[] = {0x0F, 0x84, 0xA2, 0x02, 0x00, 0x00};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Airport label filter signature mismatch at %p", target);
        LogProbeBytes(target, sizeof(expected));
        return false;
    }
    DWORD old_protection = 0;
    if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        Log("Airport label filter VirtualProtect failed error=%lu",
            GetLastError());
        return false;
    }
    std::fill(target, target + sizeof(expected), 0x90);
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
    Log("Airport label filter removed at %p", target);
    return true;
}

bool PatchMapLocationVisibilityGuards() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }

    struct GuardPatch {
        uintptr_t offset;
        unsigned char expected[6];
    };
    const GuardPatch patches[] = {
        {0x9CF05, {0x0F, 0x85, 0xCA, 0x01, 0x00, 0x00}},
        {0x9CF1A, {0x0F, 0x84, 0xB5, 0x01, 0x00, 0x00}},
        {0x9CF2F, {0x0F, 0x85, 0xA0, 0x01, 0x00, 0x00}},
        {0x9CF44, {0x0F, 0x84, 0x8B, 0x01, 0x00, 0x00}},
        {0x9CF5E, {0x0F, 0x85, 0x71, 0x01, 0x00, 0x00}},
        {0x9CF73, {0x0F, 0x84, 0x5C, 0x01, 0x00, 0x00}},
        {0x9CF88, {0x0F, 0x85, 0x47, 0x01, 0x00, 0x00}},
        {0x9CF9D, {0x0F, 0x84, 0x32, 0x01, 0x00, 0x00}},
        {0x9CFB2, {0x0F, 0x85, 0x1D, 0x01, 0x00, 0x00}},
        {0x9CFC7, {0x0F, 0x84, 0x08, 0x01, 0x00, 0x00}},
        {0x9CFDC, {0x0F, 0x85, 0xF3, 0x00, 0x00, 0x00}},
        {0x9CFF1, {0x0F, 0x84, 0xDE, 0x00, 0x00, 0x00}},
    };

    bool all_patched = true;
    for (const auto& patch : patches) {
        auto* target = reinterpret_cast<unsigned char*>(executable) +
                       patch.offset;
        const bool already_patched = std::all_of(
            target, target + sizeof(patch.expected),
            [](unsigned char value) { return value == 0x90; });
        if (already_patched) {
            continue;
        }
        if (std::memcmp(target, patch.expected, sizeof(patch.expected)) != 0) {
            Log("Map label visibility guard signature mismatch at %p", target);
            LogProbeBytes(target, sizeof(patch.expected));
            all_patched = false;
            continue;
        }
        DWORD old_protection = 0;
        if (!VirtualProtect(target, sizeof(patch.expected),
                            PAGE_EXECUTE_READWRITE, &old_protection)) {
            Log("Map label visibility guard VirtualProtect failed at %p error=%lu",
                target, GetLastError());
            all_patched = false;
            continue;
        }
        std::fill(target, target + sizeof(patch.expected), 0x90);
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(patch.expected), old_protection,
                       &ignored);
        FlushInstructionCache(GetCurrentProcess(), target,
                              sizeof(patch.expected));
    }
    if (all_patched) {
        Log("Map label visibility guards removed");
    }
    return all_patched;
}

bool __fastcall HookMapLocationUpdate(void* self, void*, void* location) {
    auto original = reinterpret_cast<MapLocationUpdateFn>(
        g_map_location_diagnostic.trampoline);
    if (!original) {
        return false;
    }
    const bool changed = location &&
        location != g_map_location_diagnostic.last_location;
    if (changed) {
        g_map_location_diagnostic.last_location = location;
        unsigned char name[32]{};
        auto* source = static_cast<unsigned char*>(location) + 0x19E;
        if (!IsBadReadPtr(source, sizeof(name))) {
            std::memcpy(name, source, sizeof(name));
            char hex[sizeof(name) * 2 + 1]{};
            for (size_t i = 0; i < sizeof(name); ++i) {
                std::snprintf(hex + i * 2, 3, "%02X", name[i]);
            }
            Log("Map location hover object=%p name_hex=%s", location, hex);
        } else {
            Log("Map location hover object=%p name unreadable", location);
        }
    }
    const bool result = original(self, location);
    if (changed) {
        Log("Map location hover result object=%p shown=%d", location,
            result ? 1 : 0);
    }
    return result;
}

bool InstallMapLocationDiagnosticHook() {
    if (g_map_location_diagnostic.installed) {
        return true;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x9CDA0;
    const unsigned char expected[] = {
        0xA1, 0x5C, 0x80, 0x75, 0x00, 0x83, 0xEC, 0x54,
    };
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Map location diagnostic signature mismatch at %p", target);
        return false;
    }
    constexpr size_t stolen_size = sizeof(expected);
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, stolen_size + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, target, stolen_size);
    trampoline[stolen_size] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + stolen_size + 1) =
        static_cast<int32_t>((target + stolen_size) -
                             (trampoline + stolen_size + 5));
    DWORD old_protection = 0;
    if (!VirtualProtect(target, stolen_size, PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookMapLocationUpdate) -
        (target + 5));
    std::fill(target + 5, target + stolen_size, 0x90);
    DWORD ignored = 0;
    VirtualProtect(target, stolen_size, old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, stolen_size);
    g_map_location_diagnostic.trampoline = trampoline;
    g_map_location_diagnostic.installed = true;
    Log("Installed map location diagnostic hook at %p", target);
    return true;
}


}  // namespace stardom
