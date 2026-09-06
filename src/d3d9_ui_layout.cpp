#include "d3d9_proxy_internal.h"
#include "gui_object.h"
#include "ui_layout_registry.h"
#include "ui_page_geometry.h"
#include "ui_dispatch.h"
#include "font_outline.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

namespace stardom {

using GuiMoveFn = void(__thiscall*)(void*, int, int);
using GuiResizeFn = void(__thiscall*)(void*, int, int);
using MapLocationUpdateFn = bool(__thiscall*)(void*, void*);

struct MapLocationDiagnosticState {
    void* trampoline = nullptr;
    void* last_location = nullptr;
    bool installed = false;
};

MapLocationDiagnosticState g_map_location_diagnostic;

void TransformRootChildPosition(int width, int height, int& x, int& y) {
    // Treat a coordinate pair as one coordinate space. Dynamic GUI objects
    // sometimes feed their already-transformed/output-space x back through
    // this hook while y still happens to fall below 600. Transforming the
    // axes independently then creates a mixed output/legacy coordinate pair.
    // It also makes repeated show/hide or animation callbacks
    // non-idempotent. Only coordinates wholly inside the legacy canvas are
    // eligible for legacy edge anchoring; staging and output-space positions
    // pass through unchanged.
    if (x < 0 || x > LegacyCanvas::width ||
        y < 0 || y > LegacyCanvas::height) {
        return;
    }
    const int vertical_center_twice = y * 2 + std::max(height, 0);
    const bool bottom_group = vertical_center_twice >
        (LegacyCanvas::height * 4) / 3;
    if (bottom_group && x >= LegacyCanvas::width / 2 &&
        x <= LegacyCanvas::width) {
        x += static_cast<int>(g_unified_ui.width) - LegacyCanvas::width;
    } else {
        x = TransformAnchoredCoordinate(x, width, LegacyCanvas::width,
            static_cast<int>(g_unified_ui.width));
    }
    y = TransformAnchoredCoordinate(y, height, LegacyCanvas::height,
        static_cast<int>(g_unified_ui.height));
}

void TransformWorldPosition(int width, int height, int& x, int& y) {
    const PointI transformed = TransformWorldCoordinate(
        x, y, width, height, static_cast<int>(g_unified_ui.width),
        static_cast<int>(g_unified_ui.height));
    x = transformed.x;
    y = transformed.y;
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

bool IsRadialInteractionWheel(void* object, void* parent,
                              int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 150 || width > 170 || height < 60 || height > 80) {
        return false;
    }

    // Four 32x32 buttons form the shallow arc displayed above a selected
    // character. Match the child structure so unrelated 160x70 widgets keep
    // their normal screen anchoring.
    void* child = GuiPointer(object, GuiObjectField::first_child);
    size_t count = 0;
    while (CanReadGuiObject(child) && count < 5) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        if (child_width < 30 || child_width > 34 ||
            child_height < 30 || child_height > 34) {
            return false;
        }
        ++count;
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
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
    const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
    const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
    void* grandchild = GuiField<void*>(child_bytes, GuiObjectField::first_child);
    void* next_child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    return !grandchild && !next_child && child_width >= 20 &&
        child_width <= 140 && child_height >= 18 && child_height <= 24;
}

bool IsTeamFriendshipTag(void* object, void* parent,
                         int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 64 || width > 68 || height < 17 || height > 21) {
        return false;
    }

    // Office/TeamFriendship.txt is a projected 66x19 tag with a 20x18
    // numeric label and a 15x15 relationship icon. Unlike ordinary compact
    // status bubbles it has two children, so match the complete resource
    // signature before bypassing legacy page anchoring. The team controller
    // already supplies the tag position in the active 3D viewport; treating
    // (320,411) as a centred legacy control incorrectly moves it to
    // (560,531) at 1280x720.
    void* first = GuiPointer(object, GuiObjectField::first_child);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    void* second = GuiPointer(first, GuiObjectField::next_sibling);
    if (!CanReadGuiObject(second) ||
        GuiPointer(second, GuiObjectField::next_sibling) != nullptr) {
        return false;
    }
    const auto is_label = [](void* child) {
        return GuiField<int>(child, GuiObjectField::x) >= 43 &&
            GuiField<int>(child, GuiObjectField::x) <= 47 &&
            GuiField<int>(child, GuiObjectField::y) >= 0 &&
            GuiField<int>(child, GuiObjectField::y) <= 2 &&
            GuiField<int>(child, GuiObjectField::width) >= 18 &&
            GuiField<int>(child, GuiObjectField::width) <= 22 &&
            GuiField<int>(child, GuiObjectField::height) >= 16 &&
            GuiField<int>(child, GuiObjectField::height) <= 20 &&
            GuiPointer(child, GuiObjectField::first_child) == nullptr;
    };
    const auto is_icon = [](void* child) {
        return GuiField<int>(child, GuiObjectField::x) >= 32 &&
            GuiField<int>(child, GuiObjectField::x) <= 36 &&
            GuiField<int>(child, GuiObjectField::y) >= 0 &&
            GuiField<int>(child, GuiObjectField::y) <= 2 &&
            GuiField<int>(child, GuiObjectField::width) >= 13 &&
            GuiField<int>(child, GuiObjectField::width) <= 17 &&
            GuiField<int>(child, GuiObjectField::height) >= 13 &&
            GuiField<int>(child, GuiObjectField::height) <= 17 &&
            GuiPointer(child, GuiObjectField::first_child) == nullptr;
    };
    return (is_label(first) && is_icon(second)) ||
        (is_icon(first) && is_label(second));
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
    const int first_width = GuiField<int>(first_bytes, GuiObjectField::width);
    const int first_height = GuiField<int>(first_bytes, GuiObjectField::height);
    void* second = GuiField<void*>(first_bytes, GuiObjectField::next_sibling);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    const int second_width = GuiField<int>(second_bytes, GuiObjectField::width);
    const int second_height = GuiField<int>(second_bytes, GuiObjectField::height);
    return first_width >= 30 && first_width <= 34 &&
        first_height >= 30 && first_height <= 34 &&
        second_width >= 80 && second_width <= 190 &&
        second_height >= 35 && second_height <= 85;
}

bool IsWorldDialogueChoicePanel(void* object, void* parent,
                                int width, int height) {
    if (!CanReadGuiObject(object) ||
        (!IsWorldRoot(parent) && !IsKnownLayoutRoot(parent)) ||
        width < 174 || width > 182 || height < 113 || height > 121) {
        return false;
    }

    // A world dialogue choice panel is a 178x117 projection-space container
    // with four 168x20 option rows. Its controller continuously positions the
    // complete panel beside the selected actor using output pixels. At narrow
    // widescreen modes those valid projected coordinates can still be below
    // the legacy 800px boundary; the generic edge anchor would then add the
    // output expansion a second time (for example 622 -> 1102 at 1280x720).
    // Match the complete row signature so authored 178x117 legacy panels keep
    // their normal layout behavior.
    int option_rows = 0;
    void* child = GuiPointer(object, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 8) {
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
        option_rows += child_x >= 4 && child_x <= 8 &&
            child_y >= 32 && child_y <= 96 &&
            child_width >= 166 && child_width <= 170 &&
            child_height >= 18 && child_height <= 22 &&
            CanReadGuiObject(GuiPointer(child, GuiObjectField::first_child));
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return child == nullptr && visited == 4 && option_rows == 4;
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
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
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
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return visited == 5 && stage_layers == 2 && central_prompt &&
        compact_control && lower_explanation;
}

extern void* g_title_tutorial_root;

bool HasTitleTutorialPage() {
    if (CanReadGuiObject(g_title_tutorial_root)) {
        auto* bytes = static_cast<unsigned char*>(g_title_tutorial_root);
        if (GuiField<void*>(bytes, GuiObjectField::parent) ==
                g_unified_ui.primary_root && *(bytes + 0x99) != 0) {
            return true;
        }
    }
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        return false;
    }
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(g_unified_ui.primary_root) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 512) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
        if (IsTitleTutorialPage(
                child, g_unified_ui.primary_root, width, height)) {
            return true;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
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
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        rows += child_x >= 3 && child_x <= 7 &&
            child_y >= 3 && child_y <= 107 &&
            child_width >= width - 33 && child_width <= width - 29 &&
            child_height >= 23 && child_height <= 27;
        arrows += child_x >= width - 24 && child_x <= width - 20 &&
            (child_y >= 1 && child_y <= 5 ||
             child_y >= 101 && child_y <= 105) &&
            child_width >= 17 && child_width <= 21 &&
            child_height >= 27 && child_height <= 31;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited == 7 && rows == 5 && arrows == 2;
}

extern void* g_studio_event_editor_root;

bool HasStudioEventEditorPage() {
    if (!CanReadGuiObject(g_studio_event_editor_root)) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_studio_event_editor_root);
    return GuiField<void*>(bytes, GuiObjectField::parent) ==
            g_unified_ui.primary_root && *(bytes + 0x99) != 0;
}

bool IsStudioEventEditorDropdown(void* object, void* parent,
                                 int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 58 || width > 172 || height < 133 || height > 137 ||
        HasTitleTutorialPage() || !HasStudioEventEditorPage()) {
        return false;
    }

    // Editor/SelectList*.txt uses the same detached seven-child popup family
    // for year, month, location, condition and participant selectors. Width
    // varies from 60 to 170, while all variants keep five rows and two arrows.
    int rows = 0;
    int arrows = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        rows += child_x >= 3 && child_x <= 7 &&
            child_y >= 3 && child_y <= 107 &&
            child_width >= width - 33 && child_width <= width - 29 &&
            child_height >= 23 && child_height <= 27;
        arrows += child_x >= width - 24 && child_x <= width - 20 &&
            (child_y >= 1 && child_y <= 5 ||
             child_y >= 101 && child_y <= 105) &&
            child_width >= 17 && child_width <= 21 &&
            child_height >= 27 && child_height <= 31;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
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
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
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
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited == 4 && answer_buttons == 3 && question_panel;
}

bool IsArtistProfilePage(void* object, void* parent,
                         int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // The artist profile is a direct-root 800x600 page. Its stable top-level
    // signature is four 110x35 tabs, the 549x281 profile card (which owns the
    // portrait, attributes, radar grid, data polygon and labels), and a hidden
    // 100x500 side strip. Match the complete group so unrelated legacy dialogs
    // continue to use the native-size centred-page path below.
    int tabs = 0;
    bool profile_card = false;
    bool side_strip = false;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 10) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        const bool tab_x =
            (child_x >= 158 && child_x <= 162) ||
            (child_x >= 273 && child_x <= 277) ||
            (child_x >= 388 && child_x <= 392) ||
            (child_x >= 503 && child_x <= 507);
        tabs += tab_x && child_y >= 127 && child_y <= 131 &&
            child_width >= 108 && child_width <= 112 &&
            child_height >= 33 && child_height <= 37;
        profile_card |= child_x >= 123 && child_x <= 127 &&
            child_y >= 157 && child_y <= 161 &&
            child_width >= 547 && child_width <= 551 &&
            child_height >= 279 && child_height <= 283;
        side_strip |= child_x >= 18 && child_x <= 22 &&
            child_y >= 48 && child_y <= 52 &&
            child_width >= 98 && child_width <= 102 &&
            child_height >= 498 && child_height <= 502;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited == 6 && tabs == 4 && profile_card && side_strip;
}

bool IsInventoryPage(void* object, void* parent, int, int) {
    return parent == g_unified_ui.primary_root &&
        IsInventoryPageTree(object, g_unified_ui.primary_root);
}

bool IsSaveLoadPage(void* object, void* parent, int, int) {
    return parent == g_unified_ui.primary_root &&
        IsSaveLoadPageTree(object, g_unified_ui.primary_root);
}

bool IsBigActivityPage(void* object, void* parent, int, int) {
    return parent == g_unified_ui.primary_root &&
        IsBigActivityPageTree(object, g_unified_ui.primary_root);
}

bool IsArtistContractPage(void* object, void* parent,
                          int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // The artist contract page is a direct-root 800x600 page containing one
    // 522x290 contract card. Match the card's portrait and complete three-
    // button action column as well, so unrelated single-card dialogs retain
    // the native-size centred-page layout.
    auto* root_bytes = static_cast<unsigned char*>(object);
    void* card = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(card)) {
        return false;
    }
    auto* card_bytes = static_cast<unsigned char*>(card);
    if (GuiField<int>(card_bytes, GuiObjectField::x) < 137 ||
        GuiField<int>(card_bytes, GuiObjectField::x) > 141 ||
        GuiField<int>(card_bytes, GuiObjectField::y) < 153 ||
        GuiField<int>(card_bytes, GuiObjectField::y) > 157 ||
        GuiField<int>(card_bytes, GuiObjectField::width) < 520 ||
        GuiField<int>(card_bytes, GuiObjectField::width) > 524 ||
        GuiField<int>(card_bytes, GuiObjectField::height) < 288 ||
        GuiField<int>(card_bytes, GuiObjectField::height) > 292 ||
        GuiField<void*>(card_bytes, GuiObjectField::next_sibling) != nullptr) {
        return false;
    }

    int action_buttons = 0;
    bool portrait = false;
    void* child = GuiField<void*>(card_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 20) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        const bool action_y =
            (child_y >= 149 && child_y <= 153) ||
            (child_y >= 189 && child_y <= 193) ||
            (child_y >= 229 && child_y <= 233);
        action_buttons += child_x >= 411 && child_x <= 415 && action_y &&
            child_width >= 90 && child_width <= 94 &&
            child_height >= 30 && child_height <= 34;
        portrait |= child_x >= 55 && child_x <= 59 &&
            child_y >= 31 && child_y <= 35 &&
            child_width >= 98 && child_width <= 102 &&
            child_height >= 118 && child_height <= 122;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited == 13 && action_buttons == 3 && portrait;
}

bool IsArtistSigningPage(void* object, void* parent,
                         int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 804 || height < 598 || height > 604) {
        return false;
    }

    // Signing and renewal share an 802x602 direct-root page with one 434x410
    // dossier panel. The full-height character cut-in is a separate sibling
    // and intentionally remains at its authored size; only the dossier and
    // its hit-test controls are aspect-fitted here.
    auto* root_bytes = static_cast<unsigned char*>(object);
    void* panel = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(panel)) {
        return false;
    }
    auto* panel_bytes = static_cast<unsigned char*>(panel);
    if (GuiField<int>(panel_bytes, GuiObjectField::x) < 181 ||
        GuiField<int>(panel_bytes, GuiObjectField::x) > 185 ||
        GuiField<int>(panel_bytes, GuiObjectField::y) < 93 ||
        GuiField<int>(panel_bytes, GuiObjectField::y) > 97 ||
        GuiField<int>(panel_bytes, GuiObjectField::width) < 432 ||
        GuiField<int>(panel_bytes, GuiObjectField::width) > 436 ||
        GuiField<int>(panel_bytes, GuiObjectField::height) < 408 ||
        GuiField<int>(panel_bytes, GuiObjectField::height) > 412 ||
        GuiField<void*>(panel_bytes, GuiObjectField::next_sibling) != nullptr) {
        return false;
    }

    int action_buttons = 0;
    bool portrait = false;
    void* child = GuiField<void*>(panel_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 40) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        const bool button_x =
            (child_x >= 183 && child_x <= 187) ||
            (child_x >= 293 && child_x <= 297);
        action_buttons += button_x && child_y >= 358 && child_y <= 362 &&
            child_width >= 96 && child_width <= 100 &&
            child_height >= 26 && child_height <= 30;
        portrait |= child_x >= 38 && child_x <= 42 &&
            child_y >= 25 && child_y <= 29 &&
            child_width >= 98 && child_width <= 102 &&
            child_height >= 118 && child_height <= 122;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited >= 20 && action_buttons == 2 && portrait;
}

bool IsCompanyRevenuePage(void* object, void* parent,
                          int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // The company revenue report is a direct-root 800x600 page containing a
    // single 444x535 ledger panel. Distinguish it from the other company
    // pages that share the same shell by matching the month/year navigation
    // row and all five report-category buttons owned by that panel.
    void* panel = GuiPointer(object, GuiObjectField::first_child);
    if (!CanReadGuiObject(panel) ||
        GuiPointer(panel, GuiObjectField::next_sibling) != nullptr ||
        GuiField<int>(panel, GuiObjectField::x) < 178 ||
        GuiField<int>(panel, GuiObjectField::x) > 182 ||
        GuiField<int>(panel, GuiObjectField::y) < 29 ||
        GuiField<int>(panel, GuiObjectField::y) > 33 ||
        GuiField<int>(panel, GuiObjectField::width) < 442 ||
        GuiField<int>(panel, GuiObjectField::width) > 446 ||
        GuiField<int>(panel, GuiObjectField::height) < 533 ||
        GuiField<int>(panel, GuiObjectField::height) > 537) {
        return false;
    }

    int navigation_buttons = 0;
    int category_buttons = 0;
    bool year_label = false;
    void* child = GuiPointer(panel, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 64) {
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
        const bool navigation_x =
            (child_x >= 39 && child_x <= 43 &&
             child_width >= 78 && child_width <= 82) ||
            (child_x >= 127 && child_x <= 131 &&
             child_width >= 38 && child_width <= 42) ||
            (child_x >= 270 && child_x <= 274 &&
             child_width >= 38 && child_width <= 42) ||
            (child_x >= 317 && child_x <= 321 &&
             child_width >= 78 && child_width <= 82);
        navigation_buttons += navigation_x &&
            child_y >= 62 && child_y <= 66 &&
            child_height >= 28 && child_height <= 32;
        category_buttons += child_x >= 44 && child_x <= 328 &&
            child_y >= 97 && child_y <= 101 &&
            child_width >= 66 && child_width <= 70 &&
            child_height >= 23 && child_height <= 27;
        year_label |= child_x >= 170 && child_x <= 174 &&
            child_y >= 65 && child_y <= 69 &&
            child_width >= 92 && child_width <= 96 &&
            child_height >= 18 && child_height <= 22;
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return child == nullptr && navigation_buttons == 4 &&
        category_buttons == 5 && year_label;
}

bool IsCompanyRevenueChartReady(void* object) {
    if (!CanReadGuiObject(object)) {
        return false;
    }
    void* panel = GuiPointer(object, GuiObjectField::first_child);
    if (!CanReadGuiObject(panel)) {
        return false;
    }

    int bars = 0;
    int populated_bars = 0;
    void* column = GuiPointer(panel, GuiObjectField::first_child);
    size_t visited_columns = 0;
    while (CanReadGuiObject(column) && visited_columns++ < 64) {
        const int column_x = GuiField<int>(column, GuiObjectField::x);
        const int column_y = GuiField<int>(column, GuiObjectField::y);
        const int column_width = GuiField<int>(column, GuiObjectField::width);
        const bool chart_column = IsCompanyChartColumn(
            column_x, column_y, column_width,
            GuiField<int>(column, GuiObjectField::height));
        if (chart_column) {
            void* child = GuiPointer(column, GuiObjectField::first_child);
            size_t visited_children = 0;
            while (CanReadGuiObject(child) && visited_children++ < 8) {
                const int child_x = GuiField<int>(child, GuiObjectField::x);
                const int child_y = GuiField<int>(child, GuiObjectField::y);
                const int child_width =
                    GuiField<int>(child, GuiObjectField::width);
                const int child_height =
                    GuiField<int>(child, GuiObjectField::height);
                const bool bar = child_width >= 18 && child_width <= 22 &&
                    ((child_x >= 12 && child_x <= 16) ||
                     (child_x >= 33 && child_x <= 37));
                if (bar) {
                    ++bars;
                    populated_bars += child_y > 8 || child_height < 150;
                }
                child = GuiPointer(child, GuiObjectField::next_sibling);
            }
        }
        column = GuiPointer(column, GuiObjectField::next_sibling);
    }
    // All ten bars are initially created as nearly full-height placeholders.
    // Let the report controller populate at least one real value before the
    // subtree changes coordinate systems.
    return bars == 10 && populated_bars > 0;
}

bool IsCompanySectionPage(void* object, void* parent,
                          int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    void* panel = GuiPointer(object, GuiObjectField::first_child);
    return CanReadGuiObject(panel) &&
        GuiPointer(panel, GuiObjectField::next_sibling) == nullptr &&
        GuiField<int>(panel, GuiObjectField::x) >= 178 &&
        GuiField<int>(panel, GuiObjectField::x) <= 182 &&
        GuiField<int>(panel, GuiObjectField::y) >= 29 &&
        GuiField<int>(panel, GuiObjectField::y) <= 33 &&
        GuiField<int>(panel, GuiObjectField::width) >= 442 &&
        GuiField<int>(panel, GuiObjectField::width) <= 446 &&
        GuiField<int>(panel, GuiObjectField::height) >= 533 &&
        GuiField<int>(panel, GuiObjectField::height) <= 537;
}

bool IsCompanyNavigationOverlay(void* object, void* parent,
                                int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // The five company-section buttons and shared exit button live in a
    // separate direct-root overlay, not in any individual company page.
    int section_buttons = 0;
    bool exit_button = false;
    void* child = GuiPointer(object, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
        // Tabs slide between slots when the selected section changes. The
        // six-control shape identifies this overlay, not one tab's rest pose.
        section_buttons += IsCompanyTabButton(
            child_x, child_y, child_width, child_height);
        exit_button |= child_x >= 496 && child_x <= 500 &&
            child_y >= 524 && child_y <= 528 &&
            child_width >= 90 && child_width <= 94 &&
            child_height >= 30 && child_height <= 34;
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return child == nullptr && visited == 6 &&
        section_buttons == 5 && exit_button;
}

bool IsAirportSelectionPage(void* object, void* parent,
                            int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // Airport/Airport.txt is a direct-root 800x600 page. Its distinctive
    // bottom section consists of a 700x100 flight-information strip and the
    // two 95x28 go/leave buttons at the far-right edge. The map also owns at
    // least five 30x30 location buttons directly under the page. Match the
    // complete group so other centred legacy pages keep their native size.
    bool flight_information = false;
    int action_buttons = 0;
    int location_buttons = 0;
    void* child = GuiPointer(object, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 64) {
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
        flight_information |= child_x >= -2 && child_x <= 2 &&
            child_y >= 498 && child_y <= 502 &&
            child_width >= 698 && child_width <= 702 &&
            child_height >= 98 && child_height <= 102;
        action_buttons += child_x >= 697 && child_x <= 701 &&
            (child_y >= 514 && child_y <= 518 ||
             child_y >= 554 && child_y <= 558) &&
            child_width >= 93 && child_width <= 97 &&
            child_height >= 26 && child_height <= 30;
        location_buttons += child_x >= 0 && child_x <= 700 &&
            child_y >= 150 && child_y <= 320 &&
            child_width >= 28 && child_width <= 32 &&
            child_height >= 28 && child_height <= 32 &&
            CanReadGuiObject(GuiPointer(child, GuiObjectField::first_child));
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return child == nullptr && flight_information && action_buttons == 2 &&
        location_buttons >= 5;
}

bool IsStudioEventListPage(void* object, void* parent,
                           int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // EventList.txt owns a 760x567 ground panel containing ten event rows,
    // one exit button and the vertical scroll track. Match that full group so
    // other legacy editor pages keep their existing centred-page behaviour.
    void* ground = nullptr;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t root_children = 0;
    while (CanReadGuiObject(child) && root_children++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        if (child_x >= 18 && child_x <= 22 &&
            child_y >= 9 && child_y <= 13 &&
            child_width >= 758 && child_width <= 762 &&
            child_height >= 565 && child_height <= 569) {
            ground = child;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    if (!CanReadGuiObject(ground)) {
        return false;
    }

    int event_rows = 0;
    bool exit_button = false;
    bool scroll_track = false;
    child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(ground) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 256) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        event_rows += child_x >= 224 && child_x <= 228 &&
            child_y >= 83 && child_y <= 447 &&
            child_width >= 478 && child_width <= 482 &&
            child_height >= 18 && child_height <= 22;
        exit_button |= child_x >= 625 && child_x <= 629 &&
            child_y >= 518 && child_y <= 522 &&
            child_width >= 90 && child_width <= 94 &&
            child_height >= 30 && child_height <= 34;
        scroll_track |= child_x >= 708 && child_x <= 712 &&
            child_y >= 162 && child_y <= 166 &&
            child_width >= 26 && child_width <= 30 &&
            child_height >= 224 && child_height <= 228;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return event_rows == 10 && exit_button && scroll_track;
}

bool IsStudioEventEditorPage(void* object, void* parent,
                             int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // EventUnit.txt owns a 760x577 editor panel with two 700x190 dialogue
    // sections and five 98x28 action buttons along its bottom edge. Match the
    // complete authored group instead of treating every legacy editor page as
    // an event editor.
    void* panel = nullptr;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t root_children = 0;
    while (CanReadGuiObject(child) && root_children++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        if (child_x >= 18 && child_x <= 22 &&
            child_y >= 9 && child_y <= 13 &&
            child_width >= 758 && child_width <= 762 &&
            child_height >= 575 && child_height <= 579) {
            panel = child;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    if (!CanReadGuiObject(panel)) {
        return false;
    }

    bool first_dialogue = false;
    bool second_dialogue = false;
    int action_buttons = 0;
    child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(panel) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 512) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        first_dialogue |= child_x >= -2 && child_x <= 2 &&
            child_y >= 148 && child_y <= 152 &&
            child_width >= 698 && child_width <= 702 &&
            child_height >= 188 && child_height <= 192;
        second_dialogue |= child_x >= -2 && child_x <= 2 &&
            child_y >= 338 && child_y <= 342 &&
            child_width >= 698 && child_width <= 702 &&
            child_height >= 188 && child_height <= 192;
        action_buttons += child_x >= 183 && child_x <= 623 &&
            child_y >= 531 && child_y <= 535 &&
            child_width >= 96 && child_width <= 100 &&
            child_height >= 26 && child_height <= 30;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return first_dialogue && second_dialogue && action_buttons == 5;
}

bool IsTrainingMinigamePanel(void* object, void* parent,
                             int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 424 || width > 428 || height < 367 || height > 371) {
        return false;
    }

    // Training activities share a direct-root 426x369 minigame frame. The
    // generic layout has usually translated its authored (187,126) position
    // onto the centred 800x600 canvas before discovery, so position is not
    // part of the signature. Match the timer, full frame and play surface to
    // exclude unrelated centred panels of a similar size.
    int timer_panels = 0;
    bool full_frame = false;
    bool play_surface = false;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 20) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        timer_panels += child_x >= 53 && child_x <= 57 &&
            child_y >= 2 && child_y <= 6 &&
            child_width >= 213 && child_width <= 217 &&
            child_height >= 30 && child_height <= 34;
        full_frame |= child_x >= -2 && child_x <= 2 &&
            child_y >= 32 && child_y <= 36 &&
            child_width >= 424 && child_width <= 428 &&
            child_height >= 331 && child_height <= 335;
        play_surface |= child_x >= 10 && child_x <= 14 &&
            child_y >= 118 && child_y <= 122 &&
            child_width >= 398 && child_width <= 402 &&
            child_height >= 228 && child_height <= 232;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visited >= 8 && timer_panels == 1 && full_frame && play_surface;
}

bool IsEventPublicationPanel(void* object, void* parent,
                             int width, int height) {
    // Publication events and award shortlists use the only 509/510x350
    // direct-root panel family. Resource variants differ by one pixel, so
    // excluding 509-wide panels sends their entrance frames through the
    // generic lower-right anchor and strands them off-screen.
    return object && parent == g_unified_ui.primary_root &&
        width >= 508 && width <= 512 && height >= 348 && height <= 352;
}

bool IsEventPublicationCover(void* object, void* parent,
                             int width, int height) {
    // Weekly magazines and newspapers use a portrait cover assembled from
    // three full-size image layers plus one shorter header layer.  The game
    // animates this direct-root surface with legacy 800x600 coordinates, so
    // it must not be mistaken for a lower-right HUD widget.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 328 || width > 332 || height < 448 || height > 452) {
        return false;
    }

    int full_layers = 0;
    int header_layers = 0;
    int child_count = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    while (CanReadGuiObject(child) && child_count < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        if (child_width >= 328 && child_width <= 332) {
            full_layers += child_height >= 448 && child_height <= 452;
            header_layers += child_height >= 288 && child_height <= 304;
        }
        ++child_count;
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return child_count == 4 && full_layers == 3 && header_layers == 1;
}

void* g_publication_cover = nullptr;
void* g_awards_ceremony_root = nullptr;
bool g_awards_ceremony_logged = false;
void* g_loading_page_root = nullptr;

struct LoadingPageSignature {
    bool top_badge = false;
    bool central_art = false;
    bool bottom_progress = false;
    size_t visited = 0;
};

void ScanLoadingPageSignature(void* parent, int origin_x, int origin_y,
                              int depth, LoadingPageSignature& signature) {
    if (!CanReadGuiObject(parent) || depth > 2 || signature.visited >= 128) {
        return;
    }
    auto* parent_bytes = static_cast<unsigned char*>(parent);
    void* child = GuiField<void*>(parent_bytes, GuiObjectField::first_child);
    while (CanReadGuiObject(child) && signature.visited++ < 128) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = origin_x + GuiField<int>(bytes, GuiObjectField::x);
        const int y = origin_y + GuiField<int>(bytes, GuiObjectField::y);
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
        signature.top_badge = signature.top_badge ||
            (x >= 240 && x <= 500 && y >= 0 && y <= 170 &&
             width >= 80 && width <= 280 && height >= 20 && height <= 150);
        signature.central_art = signature.central_art ||
            (x >= 140 && x <= 420 && y >= 100 && y <= 390 &&
             width >= 220 && width <= 460 && height >= 140 && height <= 340);
        signature.bottom_progress = signature.bottom_progress ||
            (x >= -10 && x <= 120 && y >= 490 && y <= 610 &&
             width >= 560 && width <= 820 && height >= 8 && height <= 100);
        ScanLoadingPageSignature(child, x, y, depth + 1, signature);
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
}

bool IsLoadingPageRoot(void* object, void* parent, int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 804 || height < 598 || height > 604) {
        return false;
    }
    LoadingPageSignature signature;
    ScanLoadingPageSignature(object, 0, 0, 0, signature);
    return signature.top_badge && signature.central_art &&
        signature.bottom_progress;
}

bool IsEndGameSummaryPage(void* object, void* parent,
                          int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }

    // Main/EndGame.txt is a direct-root 800x600 page with exactly four
    // children: the history memo, two overlapping 300x600 scrolling-credit
    // planes, and the 240x180 character photo. Match the complete resource
    // signature so other legacy pages retain their ordinary native-size
    // centered layout.
    int history_memos = 0;
    int credit_planes = 0;
    int character_photos = 0;
    size_t child_count = 0;
    void* child = GuiPointer(object, GuiObjectField::first_child);
    while (CanReadGuiObject(child) && child_count < 8) {
        const int child_x = GuiField<int>(child, GuiObjectField::x);
        const int child_y = GuiField<int>(child, GuiObjectField::y);
        const int child_width = GuiField<int>(child, GuiObjectField::width);
        const int child_height = GuiField<int>(child, GuiObjectField::height);
        history_memos += child_x >= 68 && child_x <= 72 &&
            child_y >= 354 && child_y <= 358 &&
            child_width >= 399 && child_width <= 403 &&
            child_height >= 209 && child_height <= 213;
        credit_planes += child_x >= 498 && child_x <= 502 &&
            child_y >= -2 && child_y <= 2 &&
            child_width >= 298 && child_width <= 302 &&
            child_height >= 598 && child_height <= 602;
        character_photos += child_x >= 78 && child_x <= 82 &&
            child_y >= 118 && child_y <= 122 &&
            child_width >= 238 && child_width <= 242 &&
            child_height >= 178 && child_height <= 182;
        ++child_count;
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return child == nullptr && child_count == 4 && history_memos == 1 &&
        credit_planes == 2 && character_photos == 1;
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
    const int x = GuiField<int>(bytes, GuiObjectField::x);
    const int y = GuiField<int>(bytes, GuiObjectField::y);
    void* first_child = GuiField<void*>(bytes, GuiObjectField::first_child);
    return x >= -2 && x <= 2 && y >= -2 && y <= 2 && !first_child;
}

bool IsAwardsCeremonyOverlayRoot(void* object, void* parent,
                                 int width, int height) {
    // The awards ceremony combines a widescreen 3D stage with an authored
    // 800x600 curtain overlay.  Its control root has a stable, unique direct
    // child stack: two full-canvas leaf layers, one 800x90 banner, the small
    // award badge, and the tall award-name panel.  Match the whole stack so
    // ordinary legacy pages never opt into the curtain-only cover transform.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);

    int full_canvas_leaves = 0;
    int banners = 0;
    int badges = 0;
    int award_panels = 0;
    size_t child_count = 0;
    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    while (CanReadGuiObject(child) && child_count < 8) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(child_bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(child_bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        void* grandchild = GuiField<void*>(child_bytes, GuiObjectField::first_child);
        full_canvas_leaves += !grandchild && std::abs(child_x) <= 2 &&
            std::abs(child_y) <= 2 && child_width >= 798 &&
            child_width <= 802 && child_height >= 598 && child_height <= 602;
        banners += !grandchild && std::abs(child_x) <= 2 &&
            child_y >= 196 && child_y <= 200 && child_width >= 798 &&
            child_width <= 802 && child_height >= 88 && child_height <= 92;
        badges += !grandchild && child_x >= 652 && child_x <= 656 &&
            child_y >= 12 && child_y <= 16 && child_width >= 102 &&
            child_width <= 106 && child_height >= 26 && child_height <= 30;
        award_panels += !grandchild && child_x >= 694 && child_x <= 698 &&
            child_y >= 46 && child_y <= 50 && child_width >= 75 &&
            child_width <= 79 && child_height >= 447 && child_height <= 451;
        ++child_count;
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return child_count == 5 && full_canvas_leaves == 2 && banners == 1 &&
        badges == 1 && award_panels == 1;
}

bool IsAwardsCeremonyCurtainLayer(void* object, void* parent) {
    if (!CanReadGuiObject(object) ||
        parent != g_awards_ceremony_root) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    const int width = GuiField<int>(bytes, GuiObjectField::width);
    const int height = GuiField<int>(bytes, GuiObjectField::height);
    void* first_child = GuiField<void*>(bytes, GuiObjectField::first_child);
    const bool native_canvas = width >= 798 && width <= 802 &&
        height >= 598 && height <= 602;
    const bool cover_canvas = width >= static_cast<int>(g_unified_ui.width) &&
        height >= static_cast<int>(g_unified_ui.height);
    return !first_child && (native_canvas || cover_canvas);
}

bool IsAwardsCeremonyVisible() {
    if (!CanReadGuiObject(g_awards_ceremony_root)) {
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(
        g_awards_ceremony_root);
    return *(bytes + 0x99) != 0;
}

void LayoutAwardsCeremonyCurtainLayer(void* object) {
    if (!CanReadGuiObject(object) || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    auto* bytes = static_cast<unsigned char*>(object);
    int cover_width = 0;
    int cover_height = 0;
    if (static_cast<uint64_t>(g_unified_ui.width) * LegacyCanvas::height >=
        static_cast<uint64_t>(g_unified_ui.height) * LegacyCanvas::width) {
        cover_width = static_cast<int>(g_unified_ui.width);
        cover_height = MulDiv(cover_width, LegacyCanvas::height,
                              LegacyCanvas::width);
    } else {
        cover_height = static_cast<int>(g_unified_ui.height);
        cover_width = MulDiv(cover_height, LegacyCanvas::width,
                             LegacyCanvas::height);
    }
    GuiField<int>(bytes, GuiObjectField::width) = cover_width;
    GuiField<int>(bytes, GuiObjectField::height) = cover_height;

    const int root_x = (static_cast<int>(g_unified_ui.width) -
                        LegacyCanvas::width) / 2;
    const int root_y = (static_cast<int>(g_unified_ui.height) -
                        LegacyCanvas::height) / 2;
    const int output_x = (static_cast<int>(g_unified_ui.width) -
                          cover_width) / 2;
    const int output_y = (static_cast<int>(g_unified_ui.height) -
                          cover_height) / 2;
    const int local_x = output_x - root_x;
    const int local_y = output_y - root_y;
    original(object, local_x, local_y);
    // These curtain leaves have the game's movement limiter enabled. It
    // clamps the negative local origin back to 0,0 even though the oversized
    // cover surface must extend beyond its centered 800x600 parent. They have
    // no descendants or hit region, so restoring the requested object-space
    // coordinates directly is both sufficient and isolated to rendering.
    GuiField<int>(bytes, GuiObjectField::x) = local_x;
    GuiField<int>(bytes, GuiObjectField::y) = local_y;
    RememberProcessedLayoutObject(object);
}

void LayoutAwardsCeremonyOverlayRoot(void* root) {
    if (!CanReadGuiObject(root) || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    original(root,
        (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2,
        (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2);

    auto* root_bytes = static_cast<unsigned char*>(root);
    void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 8) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        void* next = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
        if (IsAwardsCeremonyCurtainLayer(child, root)) {
            LayoutAwardsCeremonyCurtainLayer(child);
        }
        child = next;
    }
    RememberProcessedLayoutObject(root);
}

void LogAwardsCeremonyLayoutOnce(void* root) {
    if (g_awards_ceremony_logged || !CanReadGuiObject(root)) {
        return;
    }
    auto* awards_bytes = static_cast<unsigned char*>(root);
    void* layer = GuiField<void*>(awards_bytes, GuiObjectField::first_child);
    while (CanReadGuiObject(layer)) {
        if (IsAwardsCeremonyCurtainLayer(layer, root)) {
            auto* layer_bytes = static_cast<unsigned char*>(layer);
            Log("Unified UI awards curtain cover self=%p root=%p rect=%d,%d %dx%d output=%ux%u",
                layer, root,
                GuiField<int>(layer_bytes, GuiObjectField::x),
                GuiField<int>(layer_bytes, GuiObjectField::y),
                GuiField<int>(layer_bytes, GuiObjectField::width),
                GuiField<int>(layer_bytes, GuiObjectField::height),
                g_unified_ui.width, g_unified_ui.height);
        }
        layer = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(layer) + 0xF8);
    }
    g_awards_ceremony_logged = true;
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

    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    while (CanReadGuiObject(child) && direct_children++ < 32) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(child_bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(child_bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
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
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
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
    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 32) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int x = GuiField<int>(child_bytes, GuiObjectField::x);
        const int y = GuiField<int>(child_bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        filmstrip |= x >= -2 && x <= 2 && y >= 504 && y <= 512 &&
            child_width >= 798 && child_width <= 802 &&
            child_height >= 88 && child_height <= 96;
        book_page |= x >= 395 && x <= 405 && y >= 25 && y <= 40 &&
            child_width >= 350 && child_width <= 360 &&
            child_height >= 440 && child_height <= 450;
        exit_button |= x >= 685 && x <= 695 && y >= 550 && y <= 560 &&
            child_width >= 88 && child_width <= 96 &&
            child_height >= 28 && child_height <= 36;
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return filmstrip && book_page && exit_button;
}

bool IsInGameCGRoot(void* object, void* parent, int width, int height) {
    // Story CGs are a direct-root 800x600 page containing exactly two
    // childless 800x600 image surfaces (the current and next frame). This is
    // distinct from the interactive photo album, whose root has many controls.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 598 || height > 602) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    // Cache this structurally unique page even while hidden. Visibility is
    // checked separately every frame, so the first CG can activate without
    // waiting for another discovery cycle.
    void* first = GuiField<void*>(bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    auto* first_bytes = static_cast<unsigned char*>(first);
    void* second = GuiField<void*>(first_bytes, GuiObjectField::next_sibling);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    if (GuiField<void*>(second_bytes, GuiObjectField::next_sibling) != nullptr) {
        return false;
    }
    for (auto* child_bytes : {first_bytes, second_bytes}) {
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        if (child_width < 798 || child_width > 802 ||
            child_height < 598 || child_height > 602 ||
            GuiField<void*>(child_bytes, GuiObjectField::first_child) != nullptr) {
            return false;
        }
    }
    return true;
}

bool IsInGameCGCaption(void* object, void* parent, int width, int height) {
    // Story-CG narration and ordinary status reminders share this direct-root
    // 800x76 surface with one inset 780x60 text child. Match the structure,
    // not the active scene, and keep the complete bar at native size.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 72 || height > 80) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(child)) {
        return false;
    }
    auto* child_bytes = static_cast<unsigned char*>(child);
    const int child_x = GuiField<int>(child_bytes, GuiObjectField::x);
    const int child_y = GuiField<int>(child_bytes, GuiObjectField::y);
    const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
    const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
    return child_x >= 0 && child_x <= 20 && child_y >= 0 && child_y <= 20 &&
        child_width >= 760 && child_width <= 790 &&
        child_height >= 50 && child_height <= 70 &&
        GuiField<void*>(child_bytes, GuiObjectField::next_sibling) == nullptr;
}

bool IsInGameCGItemNotice(void* object, void* parent,
                          int width, int height) {
    // Status reminders and item acquisition notices share this 800x128 root.
    // Its two children are the 380x60 memo and the 200x140 character/item
    // illustration. Match the structure rather than the current scene: the
    // same control is also used over the ordinary 3D world.
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width < 798 || width > 802 || height < 122 || height > 134) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    if (*(bytes + 0x99) == 0) {
        return false;
    }
    void* first = GuiField<void*>(bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(first)) {
        return false;
    }
    auto* first_bytes = static_cast<unsigned char*>(first);
    void* second = GuiField<void*>(first_bytes, GuiObjectField::next_sibling);
    if (!CanReadGuiObject(second)) {
        return false;
    }
    auto* second_bytes = static_cast<unsigned char*>(second);
    const int first_width = GuiField<int>(first_bytes, GuiObjectField::width);
    const int first_height = GuiField<int>(first_bytes, GuiObjectField::height);
    const int second_width = GuiField<int>(second_bytes, GuiObjectField::width);
    const int second_height = GuiField<int>(second_bytes, GuiObjectField::height);
    return first_width >= 370 && first_width <= 390 &&
        first_height >= 55 && first_height <= 65 &&
        second_width >= 190 && second_width <= 210 &&
        second_height >= 130 && second_height <= 150 &&
        GuiField<void*>(second_bytes, GuiObjectField::next_sibling) == nullptr;
}

bool IsInGameCGVisible() {
    if (!CanReadGuiObject(g_unified_ui.in_game_cg_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_unified_ui.in_game_cg_root) +
        0x99) != 0;
}

void GetInGameCGCaptionPosition(int native_y, int& x, int& y) {
    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    // Match the photo-album CG caption treatment: preserve the complete
    // authored 800x600 overlay and translate it as one native-size canvas.
    // Do not scale y through the aspect-fit image viewport; doing so feeds an
    // already moved value back into the next refresh and pushes the bar down.
    x = viewport_x + (viewport_width - LegacyCanvas::width) / 2;
    y = native_y + (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
}

void GetInGameCGNativeOverlayPosition(int native_x, int native_y,
                                      int& x, int& y) {
    x = native_x + (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
    y = native_y + (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
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
    return CanReadGuiObject(GuiField<void*>(bytes, GuiObjectField::first_child));
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
        native_y < -200 || native_y > LegacyCanvas::height) {
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

void* DiscoverInGameCGSurfaces(void* root, void* start_child = nullptr,
                               size_t maximum_nodes = 4096) {
    GuiObjectReadBatch read_batch;
    if (!CanReadGuiObject(root)) {
        return nullptr;
    }
    void* visible_cg = IsInGameCGVisible() ?
        g_unified_ui.in_game_cg_root : nullptr;
    void* visible_caption = nullptr;
    void* visible_item_notice = nullptr;
    int caption_native_y = 342;
    int item_notice_native_x = 0;
    int item_notice_native_y = 316;
    void* child = start_child ? start_child : *reinterpret_cast<void**>(
        static_cast<unsigned char*>(root) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < maximum_nodes) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = GuiField<int>(bytes, GuiObjectField::x);
        const int y = GuiField<int>(bytes, GuiObjectField::y);
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
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
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    if (visible_cg) {
        g_unified_ui.in_game_cg_root = visible_cg;
    }
    // The 800x76 bar is also used for status reminders over the ordinary 3D
    // world, so retain it independently of the story-CG root.
    if (visible_caption) {
        const bool new_caption =
            visible_caption != g_unified_ui.in_game_cg_caption;
        g_unified_ui.in_game_cg_caption = visible_caption;
        if (new_caption) {
            g_unified_ui.in_game_cg_caption_native_y = caption_native_y;
            g_unified_ui.in_game_cg_caption_logged = false;
        }
    }
    // The notice is shared by story CGs and ordinary world scenes. Retain it
    // even when no CG root is visible so both contexts use the same native
    // 800x600 translation.
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
    // A non-null result resumes at the next unvisited sibling next frame. At
    // the end (or after an invalidated list node), restart from the root on the
    // following frame so visibility-only changes are discovered promptly.
    return visited >= maximum_nodes && CanReadGuiObject(child) ?
        child : nullptr;
}

void LayoutInGameCGRoot(void* object) {
    if (!CanReadGuiObject(object) || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    auto* bytes = static_cast<unsigned char*>(object);
    GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
    GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
    original(object, viewport_x, viewport_y);

    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 2) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        GuiField<int>(child_bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(child_bytes, GuiObjectField::height) = viewport_height;
        original(child, 0, 0);
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
}

bool IsPhotoAlbumDescendant(void* object) {
    return IsDescendantOf(object, g_unified_ui.photo_album_root);
}

bool IsTitleScreenDescendant(void* object) {
    return IsDescendantOf(object, g_unified_ui.title_screen_root);
}

bool IsTitleScreenVisible() {
    if (g_unified_ui.title_screen_mode == 0 ||
        !CanReadGuiObject(g_unified_ui.title_screen_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_unified_ui.title_screen_root) +
        0x99) != 0;
}

bool GetLoadingScreenRect(RECT& rect) {
    if (!CanReadGuiObject(g_loading_page_root)) {
        g_loading_page_root = nullptr;
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_loading_page_root);
    void* parent = GuiField<void*>(bytes, GuiObjectField::parent);
    const int x = GuiField<int>(bytes, GuiObjectField::x);
    const int y = GuiField<int>(bytes, GuiObjectField::y);
    const int width = GuiField<int>(bytes, GuiObjectField::width);
    const int height = GuiField<int>(bytes, GuiObjectField::height);
    if (parent != g_unified_ui.primary_root || *(bytes + 0x99) == 0 ||
        width < 798 || width > 804 || height < 598 || height > 604) {
        return false;
    }
    rect = RECT{x, y, x + width, y + height};
    return true;
}

bool IsTitleTutorialVisible() {
    if (g_unified_ui.title_screen_mode == 0 ||
        !CanReadGuiObject(g_title_tutorial_root)) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_title_tutorial_root) + 0x99) != 0;
}

using TitleNativeGeometry = NativeGeometry;

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
    geometry.x = GuiField<int>(bytes, GuiObjectField::x);
    geometry.y = GuiField<int>(bytes, GuiObjectField::y);
    geometry.width = GuiField<int>(bytes, GuiObjectField::width);
    geometry.height = GuiField<int>(bytes, GuiObjectField::height);
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
    // The controller's primary strip always remains inside [-1000,1000].
    // Its paired strip is exactly 2000 pixels away and can be negative when
    // discovery happens mid-cycle. Testing only x < 1000 misclassifies both
    // strips as primary during that half of the cycle and leaves no follower.
    if (native->x >= -1000 && native->x <= 1000) {
        if (g_title_strip_motion.primary_object != object) {
            g_title_strip_motion.primary_object = object;
            g_title_strip_motion.primary_native_x = native->x;
        }
    } else {
        g_title_strip_motion.follower_object = object;
    }
    if (g_title_strip_motion.primary_object &&
        g_title_strip_motion.follower_object) {
        const bool newly_valid = !g_title_strip_motion.pair_valid;
        g_title_strip_motion.pair_valid = true;
        g_title_strip_motion.follower_native_x =
            g_title_strip_motion.primary_native_x < 0 ?
                g_title_strip_motion.primary_native_x + 2000 :
                g_title_strip_motion.primary_native_x - 2000;
        if (newly_valid) {
            Log("Unified UI title strips paired primary=%p nativeX=%d follower=%p nativeX=%d",
                g_title_strip_motion.primary_object,
                g_title_strip_motion.primary_native_x,
                g_title_strip_motion.follower_object,
                g_title_strip_motion.follower_native_x);
        }
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

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    TitleNativeGeometry* primary =
        FindTitleNativeGeometry(g_title_strip_motion.primary_object);
    TitleNativeGeometry* follower =
        FindTitleNativeGeometry(g_title_strip_motion.follower_object);
    if (!primary || !follower) {
        return;
    }
    original(g_title_strip_motion.primary_object,
        MulDiv(g_title_strip_motion.primary_native_x, viewport_width, LegacyCanvas::width),
        MulDiv(primary->y, viewport_height, LegacyCanvas::height));
    original(g_title_strip_motion.follower_object,
        MulDiv(g_title_strip_motion.follower_native_x, viewport_width, LegacyCanvas::width),
        MulDiv(follower->y, viewport_height, LegacyCanvas::height));
}

void ScaleTitleScreenSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
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
                width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
                height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
                // Existing objects may currently be between animation keyframes.
                // Only translate a newly discovered object here; later movement is
                // handled by HookGuiMove without snapping the animation backward.
                if (newly_discovered) {
                    original(object,
                        MulDiv(native->x, viewport_width, LegacyCanvas::width),
                        MulDiv(native->y, viewport_height, LegacyCanvas::height));
                }
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 8, 512, enter, leave);
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
TitleNativeGeometry g_announcement_geometry[96]{};
size_t g_announcement_geometry_count = 0;
void* g_announcement_root = nullptr;
size_t g_announcement_artist_count = 0;
thread_local bool g_scaling_announcement_subtree = false;
TitleNativeGeometry g_artist_profile_geometry[512]{};
size_t g_artist_profile_geometry_count = 0;
void* g_artist_profile_root = nullptr;
TitleNativeGeometry g_artist_contract_geometry[64]{};
TitleNativeGeometry g_inventory_geometry[64]{};
size_t g_inventory_geometry_count = 0;
void* g_inventory_root = nullptr;
TitleNativeGeometry g_save_load_geometry[256]{};
size_t g_save_load_geometry_count = 0;
void* g_save_load_root = nullptr;
void* g_inventory_target_root = nullptr;
TitleNativeGeometry g_inventory_target_geometry[8]{};
size_t g_inventory_target_geometry_count = 0;
TitleNativeGeometry g_big_activity_geometry[512]{};
size_t g_big_activity_geometry_count = 0;
void* g_big_activity_root = nullptr;
TitleNativeGeometry g_big_activity_option_geometry[128]{};
size_t g_big_activity_option_geometry_count = 0;
void* g_big_activity_option_root = nullptr;
size_t g_artist_contract_geometry_count = 0;
void* g_artist_contract_root = nullptr;
TitleNativeGeometry g_artist_signing_geometry[128]{};
size_t g_artist_signing_geometry_count = 0;
void* g_artist_signing_root = nullptr;
TitleNativeGeometry g_company_revenue_geometry[512]{};
size_t g_company_revenue_geometry_count = 0;
void* g_company_revenue_root = nullptr;
bool g_company_revenue_waiting_for_bars = false;
TitleNativeGeometry g_company_navigation_geometry[32]{};
size_t g_company_navigation_geometry_count = 0;
void* g_company_navigation_root = nullptr;
struct CompanySectionLayout {
    void* root = nullptr;
    TitleNativeGeometry geometry[1024]{};
    size_t geometry_count = 0;
};
CompanySectionLayout g_company_section_layouts[8]{};
size_t g_company_section_layout_count = 0;
TitleNativeGeometry g_airport_selection_geometry[128]{};
size_t g_airport_selection_geometry_count = 0;
void* g_airport_selection_root = nullptr;
TitleNativeGeometry g_studio_event_list_geometry[1024]{};
size_t g_studio_event_list_geometry_count = 0;
void* g_studio_event_list_root = nullptr;
TitleNativeGeometry g_studio_event_editor_geometry[2048]{};
size_t g_studio_event_editor_geometry_count = 0;
void* g_studio_event_editor_root = nullptr;
struct StudioEventDropdownLayout {
    void* root = nullptr;
    TitleNativeGeometry geometry[16]{};
    size_t geometry_count = 0;
    int native_width = 0;
    int native_height = 0;
    bool tutorial_mode = false;
};

StudioEventDropdownLayout g_studio_event_dropdowns[8]{};
size_t g_studio_event_dropdown_count = 0;
TitleNativeGeometry g_training_minigame_geometry[64]{};
size_t g_training_minigame_geometry_count = 0;
void* g_training_minigame_root = nullptr;
TitleNativeGeometry g_training_activity_geometry[1024]{};
size_t g_training_activity_geometry_count = 0;
void* g_training_activity_roots[16]{};
size_t g_training_activity_root_count = 0;
TitleNativeGeometry g_end_game_summary_geometry[16]{};
size_t g_end_game_summary_geometry_count = 0;
void* g_end_game_summary_root = nullptr;

struct EndGameCreditMotionState {
    void* object = nullptr;
    int native_x = 500;
    int native_y = 0;
    int native_width = 300;
    int native_height = LegacyCanvas::height;
};

EndGameCreditMotionState g_end_game_credit_motion[2]{};

struct ArtistRadarVertexCache {
    float x[7][4]{};
    float y[7][4]{};
    bool valid = false;
};

ArtistRadarVertexCache g_artist_radar_vertices;

bool IsCachedAnnouncementRootValid();

template <size_t GeometryCapacity, typename ResetGeometry>
void ScaleAspectFitSubtree(
        void* object, int depth,
        TitleNativeGeometry (&geometry)[GeometryCapacity],
        size_t& geometry_count, ResetGeometry reset_geometry,
        int maximum_depth, size_t maximum_children) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }

        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        int& width = GuiField<int>(object, GuiObjectField::width);
        int& height = GuiField<int>(object, GuiObjectField::height);
        const RectI viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            if (!PrepareAspectFitRoot(reset_geometry, object)) {
                return false;
            }
            width = viewport.width;
            height = viewport.height;
            original(object, viewport.x, viewport.y);
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                geometry, GeometryCapacity, geometry_count, object);
            if (native) {
                width = MulDiv(native->width, viewport.width,
                               LegacyCanvas::width);
                height = MulDiv(native->height, viewport.height,
                                LegacyCanvas::height);
                original(object,
                    MulDiv(native->x, viewport.width, LegacyCanvas::width),
                    MulDiv(native->y, viewport.height, LegacyCanvas::height));
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, maximum_depth, maximum_children, enter, leave);
}

void ResetEndGameSummaryGeometry(void* root) {
    if (g_end_game_summary_root == root) {
        return;
    }
    g_end_game_summary_root = root;
    g_end_game_summary_geometry_count = 0;
    for (auto& motion : g_end_game_credit_motion) {
        motion = {};
    }
}

bool IsEndGameSummaryDescendant(void* object) {
    return IsDescendantOf(object, g_end_game_summary_root);
}

bool IsEndGameCreditGeometry(const TitleNativeGeometry* native) {
    return native && native->x >= 498 && native->x <= 502 &&
        native->y >= -2 && native->y <= 2 &&
        native->width >= 298 && native->width <= 302 &&
        native->height >= 598 && native->height <= 602;
}

EndGameCreditMotionState* RememberEndGameCreditMotion(
        void* object, const TitleNativeGeometry* native) {
    if (!object || !IsEndGameCreditGeometry(native)) {
        return nullptr;
    }
    for (auto& motion : g_end_game_credit_motion) {
        if (motion.object == object) {
            return &motion;
        }
    }
    for (auto& motion : g_end_game_credit_motion) {
        if (!motion.object) {
            motion.object = object;
            motion.native_x = native->x;
            motion.native_y = native->y;
            motion.native_width = native->width;
            motion.native_height = native->height;
            return &motion;
        }
    }
    return nullptr;
}

void RefreshEndGameCreditsBeforeDraw() {
    if (!CanReadGuiObject(g_end_game_summary_root) ||
        GuiField<unsigned char>(g_end_game_summary_root,
                                GuiObjectField::visible) == 0 ||
        !g_unified_ui.trampoline) {
        return;
    }
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width),
        static_cast<int>(g_unified_ui.height));
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    for (const auto& motion : g_end_game_credit_motion) {
        if (!CanReadGuiObject(motion.object) ||
            !IsDescendantOf(motion.object, g_end_game_summary_root)) {
            continue;
        }
        GuiField<int>(motion.object, GuiObjectField::width) = MulDiv(
            motion.native_width, viewport.width, LegacyCanvas::width);
        GuiField<int>(motion.object, GuiObjectField::height) = MulDiv(
            motion.native_height, viewport.height, LegacyCanvas::height);
        original(motion.object,
            MulDiv(motion.native_x, viewport.width, LegacyCanvas::width),
            MulDiv(motion.native_y, viewport.height, LegacyCanvas::height));
    }
}

void ScaleEndGameSummarySubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        int& width = GuiField<int>(object, GuiObjectField::width);
        int& height = GuiField<int>(object, GuiObjectField::height);
        const RectI viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            ResetEndGameSummaryGeometry(object);
            width = viewport.width;
            height = viewport.height;
            original(object, viewport.x, viewport.y);
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                g_end_game_summary_geometry,
                std::size(g_end_game_summary_geometry),
                g_end_game_summary_geometry_count, object);
            if (native) {
                RememberEndGameCreditMotion(object, native);
                width = MulDiv(native->width, viewport.width,
                               LegacyCanvas::width);
                height = MulDiv(native->height, viewport.height,
                                LegacyCanvas::height);
                original(object,
                    MulDiv(native->x, viewport.width, LegacyCanvas::width),
                    MulDiv(native->y, viewport.height, LegacyCanvas::height));
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 8, 16, enter, leave);
}

void ResetArtistProfileGeometry(void* root) {
    if (g_artist_profile_root == root) {
        return;
    }
    g_artist_profile_root = root;
    g_artist_profile_geometry_count = 0;
}

void CorrectArtistRadarVertices() {
    if (!CanReadGuiObject(g_artist_profile_root) ||
        *reinterpret_cast<unsigned char*>(
            static_cast<unsigned char*>(g_artist_profile_root) + 0x99) == 0) {
        g_artist_radar_vertices.valid = false;
        return;
    }

    void* profile_card = nullptr;
    for (size_t i = 0; i < g_artist_profile_geometry_count; ++i) {
        const TitleNativeGeometry& geometry = g_artist_profile_geometry[i];
        if (geometry.x == 125 && geometry.y == 159 &&
            geometry.width == 549 && geometry.height == 281 &&
            CanReadGuiObject(geometry.object)) {
            profile_card = geometry.object;
            break;
        }
    }
    if (!profile_card) {
        return;
    }

    auto* root_bytes = static_cast<unsigned char*>(g_artist_profile_root);
    auto* card_bytes = static_cast<unsigned char*>(profile_card);
    const float root_x = static_cast<float>(
        GuiField<int>(root_bytes, GuiObjectField::x));
    const float root_y = static_cast<float>(
        GuiField<int>(root_bytes, GuiObjectField::y));
    const float card_x = static_cast<float>(
        GuiField<int>(card_bytes, GuiObjectField::x));
    const float card_y = static_cast<float>(
        GuiField<int>(card_bytes, GuiObjectField::y));

    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return;
    }
    auto* controller = reinterpret_cast<unsigned char*>(executable) + 0x3A9C78;
    const uintptr_t expected_vtable =
        reinterpret_cast<uintptr_t>(executable) + 0x2EF20C;
    if (*reinterpret_cast<uintptr_t*>(controller) != expected_vtable) {
        return;
    }

    // StarState's custom renderer stores seven triangle pairs at +0x2C0.
    // Its authored local centre is (436,125), but the game only adds the
    // profile card's relative position.  Ordinary GUI objects subsequently
    // receive the root offset and 800x600-to-output scale; these raw vertices
    // do not, which places the polygon over the information panel in
    // widescreen modes.
    constexpr size_t kVerticesOffset = 0x2C0;
    constexpr size_t kVertexStride = 0x20;
    constexpr float kNativeCenterX = 436.0f;
    constexpr float kNativeCenterY = 125.0f;
    auto* first = reinterpret_cast<float*>(controller + kVerticesOffset);
    const float expected_raw_center_x = card_x + kNativeCenterX;
    const float expected_raw_center_y = card_y + kNativeCenterY;
    const bool game_regenerated_vertices =
        std::abs(first[0] - expected_raw_center_x) <= 1.5f &&
        std::abs(first[1] - expected_raw_center_y) <= 1.5f;
    if (game_regenerated_vertices) {
        for (size_t i = 0; i < 7; ++i) {
            auto* vertex = reinterpret_cast<float*>(
                controller + kVerticesOffset + i * kVertexStride);
            for (size_t point = 0; point < 4; ++point) {
                g_artist_radar_vertices.x[i][point] = vertex[point * 2];
                g_artist_radar_vertices.y[i][point] = vertex[point * 2 + 1];
            }
        }
        g_artist_radar_vertices.valid = true;
    }
    if (!g_artist_radar_vertices.valid) {
        return;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    const float scale_x = static_cast<float>(viewport_width) / static_cast<float>(LegacyCanvas::width);
    const float scale_y = static_cast<float>(viewport_height) / static_cast<float>(LegacyCanvas::height);
    const auto transform_x = [&](float raw_x) {
        return root_x + card_x + (raw_x - card_x) * scale_x;
    };
    const auto transform_y = [&](float raw_y) {
        return root_y + card_y + (raw_y - card_y) * scale_y;
    };
    for (size_t i = 0; i < 7; ++i) {
        auto* vertex = reinterpret_cast<float*>(
            controller + kVerticesOffset + i * kVertexStride);
        for (size_t point = 0; point < 4; ++point) {
            vertex[point * 2] = transform_x(
                g_artist_radar_vertices.x[i][point]);
            vertex[point * 2 + 1] = transform_y(
                g_artist_radar_vertices.y[i][point]);
        }
    }
}

void ScaleArtistProfileSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_artist_profile_geometry,
        g_artist_profile_geometry_count, ResetArtistProfileGeometry, 8, 512);
}

void ResetArtistContractGeometry(void* root) {
    if (g_artist_contract_root == root) {
        return;
    }
    g_artist_contract_root = root;
    g_artist_contract_geometry_count = 0;
}

void ResetInventoryGeometry(void* root) {
    if (g_inventory_root == root) return;
    g_inventory_root = root;
    g_inventory_geometry_count = 0;
}

void ResetSaveLoadGeometry(void* root) {
    if (g_save_load_root == root) return;
    g_save_load_root = root;
    g_save_load_geometry_count = 0;
}

void ScaleSaveLoadSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(object, depth, g_save_load_geometry,
        g_save_load_geometry_count, ResetSaveLoadGeometry, 6, 64);
}

void ScaleInventorySubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(object, depth, g_inventory_geometry,
        g_inventory_geometry_count, ResetInventoryGeometry, 6, 64);
}

void ResetBigActivityGeometry(void* root) {
    if (g_big_activity_root == root) return;
    g_big_activity_root = root;
    g_big_activity_geometry_count = 0;
}

void ScaleBigActivitySubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(object, depth, g_big_activity_geometry,
        g_big_activity_geometry_count, ResetBigActivityGeometry, 7, 512);
}

void ScaleBigActivityOption(void* object) {
    if (!g_unified_ui.trampoline || !CanReadGuiObject(object)) return;
    if (g_big_activity_option_root != object) {
        g_big_activity_option_root = object;
        g_big_activity_option_geometry_count = 0;
    }
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height));
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    GuiField<int>(object, GuiObjectField::width) = MulDiv(360, viewport.width, 800);
    GuiField<int>(object, GuiObjectField::height) = MulDiv(370, viewport.height, 600);
    original(object, viewport.x + (viewport.width - GuiField<int>(object, GuiObjectField::width)) / 2,
             viewport.y + (viewport.height - GuiField<int>(object, GuiObjectField::height)) / 2);
    // PlaneMove owns four siblings: three tabs and the entire content panel.
    // A subtree traversal starting at first_child does not visit its siblings.
    ForEachGuiChild(object, 128, [&](void* child) {
        ScaleAspectFitSubtree(child, 1,
            g_big_activity_option_geometry, g_big_activity_option_geometry_count,
            [](void*) {}, 7, 128);
    });
    RememberLayoutRoot(object);
    RememberProcessedLayoutObject(object);
}

void DiscoverInventory(void* candidate) {
    if (g_inventory_root || !IsInventoryPageTree(candidate, g_unified_ui.primary_root))
        return;
    ResetInventoryGeometry(candidate);
    RememberLayoutRoot(candidate);
    ScaleInventorySubtree(candidate);
}

void RefreshInventoryLayout() {
    // First-frame discovery is independent of the slow maintenance pass.
    // Once found, no GUI-tree scan is needed, even while this page is hidden.
    if (!g_inventory_root) {
        void* child = GuiPointer(g_unified_ui.primary_root, GuiObjectField::first_child);
        for (size_t i = 0; CanReadGuiObject(child) && i < 512; ++i) {
            DiscoverInventory(child);
            if (g_inventory_root) break;
            child = GuiPointer(child, GuiObjectField::next_sibling);
        }
    }
    if (!CanReadGuiObject(g_inventory_root) ||
        !GuiField<unsigned char>(g_inventory_root, GuiObjectField::visible)) return;
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height));
    // Controllers may rewrite bounds when refreshing row contents. Repair
    // only retained bounds; never replay tab positions during interpolation.
    for (size_t i = 0; i < g_inventory_geometry_count; ++i) {
        const auto& native = g_inventory_geometry[i];
        if (!CanReadGuiObject(native.object)) continue;
        GuiField<int>(native.object, GuiObjectField::width) =
            MulDiv(native.width, viewport.width, LegacyCanvas::width);
        GuiField<int>(native.object, GuiObjectField::height) =
            MulDiv(native.height, viewport.height, LegacyCanvas::height);
    }
}

void ScaleArtistContractSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_artist_contract_geometry,
        g_artist_contract_geometry_count, ResetArtistContractGeometry, 8, 64);
}

void ResetArtistSigningGeometry(void* root) {
    if (g_artist_signing_root == root) {
        return;
    }
    g_artist_signing_root = root;
    g_artist_signing_geometry_count = 0;
}

void ScaleArtistSigningSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_artist_signing_geometry,
        g_artist_signing_geometry_count, ResetArtistSigningGeometry, 8, 128);
}

void ResetCompanyRevenueGeometry(void* root) {
    if (g_company_revenue_root == root) {
        return;
    }
    g_company_revenue_root = root;
    g_company_revenue_geometry_count = 0;
}

bool IsCompanyRevenueDescendant(void* object) {
    return IsDescendantOf(object, g_company_revenue_root, 8);
}

void ScaleCompanyRevenueSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_company_revenue_geometry,
        g_company_revenue_geometry_count, ResetCompanyRevenueGeometry,
        8, 512);
}

bool IsCompanyRevenueActive() {
    return CanReadGuiObject(g_company_revenue_root) &&
        GuiField<unsigned char>(g_company_revenue_root,
                                GuiObjectField::visible) != 0;
}

bool IsCompanyRevenueScreenVisible() {
    if (CanReadGuiObject(g_company_navigation_root) &&
        GuiField<unsigned char>(g_company_navigation_root,
                                GuiObjectField::visible) != 0) {
        return true;
    }
    if (IsCompanyRevenueActive()) {
        return true;
    }
    for (size_t i = 0; i < g_company_section_layout_count; ++i) {
        void* root = g_company_section_layouts[i].root;
        if (CanReadGuiObject(root) &&
            GuiField<unsigned char>(root, GuiObjectField::visible) != 0) {
            return true;
        }
    }
    return false;
}

bool IsCompanyRevenueChartColumnGeometry(
        const TitleNativeGeometry* native) {
    if (!native) {
        return false;
    }
    // The report controller centres 1..5 artist columns dynamically.
    return IsCompanyChartColumn(native->x, native->y,
                                native->width, native->height);
}

bool IsCompanyRevenueBarGeometry(
        void* object, const TitleNativeGeometry* native) {
    if (!native || native->width < 18 || native->width > 22 ||
        !((native->x >= 12 && native->x <= 16) ||
          (native->x >= 33 && native->x <= 37))) {
        return false;
    }
    void* parent = GuiPointer(object, GuiObjectField::parent);
    return IsCompanyRevenueChartColumnGeometry(FindGeometry(
        g_company_revenue_geometry, g_company_revenue_geometry_count,
        parent));
}

bool IsCompanyRevenueChartMemberGeometry(
        void* object, const TitleNativeGeometry* native) {
    if (!native) {
        return false;
    }
    if (IsCompanyRevenueChartColumnGeometry(native)) {
        return true;
    }
    return IsCompanyRevenueChartColumnGeometry(FindGeometry(
        g_company_revenue_geometry, g_company_revenue_geometry_count,
        GuiPointer(object, GuiObjectField::parent)));
}

void RestoreCompanyRevenueChartToNative() {
    if (!g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    for (size_t i = 0; i < g_company_revenue_geometry_count; ++i) {
        TitleNativeGeometry& native = g_company_revenue_geometry[i];
        if (!CanReadGuiObject(native.object)) {
            continue;
        }
        if (!IsCompanyRevenueChartMemberGeometry(
                native.object, &native)) {
            continue;
        }
        GuiField<int>(native.object, GuiObjectField::width) = native.width;
        GuiField<int>(native.object, GuiObjectField::height) = native.height;
        original(native.object, native.x, native.y);
    }
    g_company_revenue_waiting_for_bars = true;
}

bool AreCompanyRevenueBarsReady() {
    int native_bars = 0;
    int populated_bars = 0;
    for (size_t i = 0; i < g_company_revenue_geometry_count; ++i) {
        TitleNativeGeometry& native = g_company_revenue_geometry[i];
        if (!CanReadGuiObject(native.object) ||
            !IsCompanyRevenueBarGeometry(native.object, &native)) {
            continue;
        }
        const int x = GuiField<int>(native.object, GuiObjectField::x);
        const int y = GuiField<int>(native.object, GuiObjectField::y);
        const int width = GuiField<int>(native.object, GuiObjectField::width);
        const int height = GuiField<int>(native.object, GuiObjectField::height);
        const bool native_bar = width >= 18 && width <= 22 &&
            ((x >= 12 && x <= 16) || (x >= 33 && x <= 37)) &&
            y >= 0 && y <= 163 && height >= 1 && height <= 164;
        if (native_bar) {
            ++native_bars;
            populated_bars += y > 8 || height < 150;
        }
    }
    return native_bars == 10 && populated_bars > 0;
}

void RefreshCompanyRevenueBars() {
    if (!IsCompanyRevenueActive() || !g_unified_ui.trampoline) {
        return;
    }
    if (g_company_revenue_waiting_for_bars) {
        if (!AreCompanyRevenueBarsReady()) {
            return;
        }
        for (size_t i = 0; i < g_company_revenue_geometry_count; ++i) {
            TitleNativeGeometry& native = g_company_revenue_geometry[i];
            if (!CanReadGuiObject(native.object) ||
                !IsCompanyRevenueChartMemberGeometry(native.object, &native)) {
                continue;
            }
            native.x = GuiField<int>(native.object, GuiObjectField::x);
            native.y = GuiField<int>(native.object, GuiObjectField::y);
            native.width = GuiField<int>(native.object, GuiObjectField::width);
            native.height = GuiField<int>(native.object, GuiObjectField::height);
        }
        g_company_revenue_waiting_for_bars = false;
        ScaleCompanyRevenueSubtree(g_company_revenue_root);
        return;
    }
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width),
        static_cast<int>(g_unified_ui.height));
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    for (size_t i = 0; i < g_company_revenue_geometry_count; ++i) {
        TitleNativeGeometry& native = g_company_revenue_geometry[i];
        if (!CanReadGuiObject(native.object) ||
            !IsCompanyRevenueBarGeometry(native.object, &native)) {
            continue;
        }
        const int current_x =
            GuiField<int>(native.object, GuiObjectField::x);
        const int current_y =
            GuiField<int>(native.object, GuiObjectField::y);
        const int current_width =
            GuiField<int>(native.object, GuiObjectField::width);
        const int current_height =
            GuiField<int>(native.object, GuiObjectField::height);
        const bool native_bar = current_width >= 18 && current_width <= 22 &&
            ((current_x >= 12 && current_x <= 16) ||
             (current_x >= 33 && current_x <= 37)) &&
            current_y >= 0 && current_y <= 163 &&
            current_height >= 1 && current_height <= 164;
        if (!native_bar) {
            continue;
        }
        GuiField<int>(native.object, GuiObjectField::width) =
            MulDiv(current_width, viewport.width, LegacyCanvas::width);
        GuiField<int>(native.object, GuiObjectField::height) =
            MulDiv(current_height, viewport.height, LegacyCanvas::height);
        original(native.object,
            MulDiv(current_x, viewport.width, LegacyCanvas::width),
            MulDiv(current_y, viewport.height, LegacyCanvas::height));
        RememberProcessedLayoutObject(native.object);
    }
}

void ResetCompanyNavigationGeometry(void* root) {
    if (g_company_navigation_root == root) {
        return;
    }
    g_company_navigation_root = root;
    g_company_navigation_geometry_count = 0;
}

bool IsCompanyNavigationDescendant(void* object) {
    return IsDescendantOf(object, g_company_navigation_root, 4);
}

void ScaleCompanyNavigationSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_company_navigation_geometry,
        g_company_navigation_geometry_count, ResetCompanyNavigationGeometry,
        4, 32);
}

bool DiscoverCompanyNavigation(void* candidate) {
    if (g_company_navigation_root || !CanReadGuiObject(candidate) ||
        !IsCompanyNavigationOverlay(candidate,
            GuiPointer(candidate, GuiObjectField::parent),
            GuiField<int>(candidate, GuiObjectField::width),
            GuiField<int>(candidate, GuiObjectField::height))) {
        return false;
    }
    ResetCompanyNavigationGeometry(candidate);
    RememberLayoutRoot(candidate);
    ScaleCompanyNavigationSubtree(candidate);
    return true;
}

void DiscoverVisibleCompanyNavigation() {
    if (g_company_navigation_root || !IsCompanyRevenueScreenVisible()) {
        return;
    }
    // Functional first-frame discovery, independent of the 30-second full
    // layout maintenance pass. Only direct roots can own this overlay.
    void* child = GuiPointer(g_unified_ui.primary_root,
                             GuiObjectField::first_child);
    for (size_t visited = 0; CanReadGuiObject(child) && visited < 512;
         ++visited) {
        if (DiscoverCompanyNavigation(child)) {
            return;
        }
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
}

using GuiAnimateMoveFn = void(__thiscall*)(void*, int, int, float);
GuiAnimateMoveFn g_company_animate_move = nullptr;

void __fastcall HookCompanyAnimateMove(void* self, void*, int x, int y,
                                       float duration) {
    GuiObjectReadBatch read_batch;
    if (CanReadGuiObject(self)) {
        DiscoverCompanyNavigation(GuiPointer(self, GuiObjectField::parent));
        void* parent = GuiPointer(self, GuiObjectField::parent);
        if (CanReadGuiObject(parent)) DiscoverInventory(GuiPointer(parent, GuiObjectField::parent));
        TitleNativeGeometry* native = FindGeometry(
                g_company_navigation_geometry,
                g_company_navigation_geometry_count, self);
        if (!native) native = FindGeometry(g_inventory_geometry, g_inventory_geometry_count, self);
        if (native) {
            // 005187A0 snapshots +80/+84 into its interpolation start.
            // Keep BOTH endpoints native; HookGuiMove fits each result once.
            const int fitted_x = GuiField<int>(self, GuiObjectField::x);
            const int fitted_y = GuiField<int>(self, GuiObjectField::y);
            GuiField<int>(self, GuiObjectField::x) = native->x;
            GuiField<int>(self, GuiObjectField::y) = native->y;
            g_company_animate_move(self, x, y, duration);
            GuiField<int>(self, GuiObjectField::x) = fitted_x;
            GuiField<int>(self, GuiObjectField::y) = fitted_y;
            return;
        }
    }
    g_company_animate_move(self, x, y, duration);
}

TitleNativeGeometry* RememberDynamicCompanySectionGeometry(
        CompanySectionLayout& layout, void* object, int x, int y,
        int viewport_width, int viewport_height) {
    if (TitleNativeGeometry* existing = FindGeometry(
            layout.geometry, layout.geometry_count, object)) {
        return existing;
    }
    if (!CanReadGuiObject(object) ||
        layout.geometry_count >= std::size(layout.geometry)) {
        return nullptr;
    }

    // Company list controllers clone controls from the already fitted page
    // template. Their current width/height are therefore scaled, while the
    // GuiMove coordinates are still authored in the native 800x600 space.
    // Cache a reconstructed native rectangle so the clone is not enlarged a
    // second time (most visibly, the artist-achievement month boards).
    TitleNativeGeometry& native =
        layout.geometry[layout.geometry_count++];
    native.object = object;
    native.x = x;
    native.y = y;
    const int current_width =
        GuiField<int>(object, GuiObjectField::width);
    const int current_height =
        GuiField<int>(object, GuiObjectField::height);
    native.width = current_width;
    native.height = current_height;
    for (size_t i = 0; i + 1 < layout.geometry_count; ++i) {
        const TitleNativeGeometry& template_geometry = layout.geometry[i];
        const int fitted_width = MulDiv(
            template_geometry.width, viewport_width, LegacyCanvas::width);
        const int fitted_height = MulDiv(
            template_geometry.height, viewport_height, LegacyCanvas::height);
        if (std::abs(current_width - fitted_width) <= 1 &&
            std::abs(current_height - fitted_height) <= 1) {
            native.width = template_geometry.width;
            native.height = template_geometry.height;
            break;
        }
    }
    return &native;
}

CompanySectionLayout* FindCompanySectionLayout(void* root) {
    for (size_t i = 0; i < g_company_section_layout_count; ++i) {
        if (g_company_section_layouts[i].root == root) {
            return &g_company_section_layouts[i];
        }
    }
    return nullptr;
}

CompanySectionLayout* RegisterCompanySectionLayout(void* root) {
    if (CompanySectionLayout* existing = FindCompanySectionLayout(root)) {
        return existing;
    }
    if (g_company_section_layout_count >=
        std::size(g_company_section_layouts)) {
        return nullptr;
    }
    CompanySectionLayout& layout =
        g_company_section_layouts[g_company_section_layout_count++];
    layout = {};
    layout.root = root;
    return &layout;
}

CompanySectionLayout* FindCompanySectionLayoutForObject(void* object) {
    for (size_t i = 0; i < g_company_section_layout_count; ++i) {
        CompanySectionLayout& layout = g_company_section_layouts[i];
        if (IsDescendantOf(object, layout.root, 8)) {
            return &layout;
        }
    }
    return nullptr;
}

void ScaleCompanySectionSubtree(
        CompanySectionLayout& layout, void* object, int depth = 0) {
    const auto reset = [&layout](void* root) {
        if (layout.root != root) {
            layout.root = root;
            layout.geometry_count = 0;
        }
    };
    ScaleAspectFitSubtree(
        object, depth, layout.geometry, layout.geometry_count, reset,
        8, 1024);
}

using CompanyListDrawFn = void(__thiscall*)(void*, void*, uint32_t, uint32_t);
CompanyListDrawFn g_company_list_draw = nullptr;

bool IsAspectFittedTextObject(void* object) {
    // Follow only the current object's ownership chain. No global GUI scan,
    // no stale per-object font cache, and dynamic/cloned labels work too.
    const void* roots[] = {
        g_title_native_root, g_title_tutorial_root, g_title_tutorial_bubble,
        g_title_tutorial_question_root, g_announcement_root,
        g_artist_profile_root, g_artist_contract_root, g_artist_signing_root,
        g_inventory_root, g_save_load_root,
        g_big_activity_root, g_big_activity_option_root,
        g_company_revenue_root, g_company_navigation_root,
        g_airport_selection_root, g_studio_event_list_root,
        g_studio_event_editor_root, g_training_minigame_root,
        g_end_game_summary_root, g_unified_ui.photo_album_root};
    for (int depth = 0; CanReadGuiObject(object) && depth <= 10; ++depth) {
        for (const void* root : roots) {
            if (object == root) {
                return true;
            }
        }
        for (size_t i = 0; i < g_company_section_layout_count; ++i) {
            if (object == g_company_section_layouts[i].root) {
                return true;
            }
        }
        for (size_t i = 0; i < g_studio_event_dropdown_count; ++i) {
            if (object == g_studio_event_dropdowns[i].root) {
                return true;
            }
        }
        for (size_t i = 0; i < g_training_activity_root_count; ++i) {
            if (object == g_training_activity_roots[i]) {
                return true;
            }
        }
        object = GuiPointer(object, GuiObjectField::parent);
    }
    return false;
}

CompanyListDrawFn g_fitted_text_draw = nullptr;
using TextMeasureFn = void(__thiscall*)(void*);
TextMeasureFn g_fitted_text_measure = nullptr;

void __fastcall HookFittedTextDraw(void* self, void*, void* renderer,
                                  uint32_t time, uint32_t flags) {
    GuiObjectReadBatch read_batch;
    if (!g_fitted_text_measure || !IsAspectFittedTextObject(self) ||
        !CanReadGuiObject(static_cast<unsigned char*>(self) + 0x60)) {
        g_fitted_text_draw(self, renderer, time, flags);
        return;
    }
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height));
    const int font = GuiField<int>(self, 0x130);
    if (viewport.height <= LegacyCanvas::height || font < 2 || font > 128) {
        g_fitted_text_draw(self, renderer, time, flags);
        return;
    }
    // These are render metrics, not the backing string, character limits,
    // animation state or logical geometry. Never persist a scaled font into
    // the control: game layout and cloning must keep seeing native values.
    constexpr size_t fields[] = {0x130, 0x140, 0x150, 0x154, 0x158};
    int saved[std::size(fields)];
    for (size_t i = 0; i < std::size(fields); ++i) {
        saved[i] = GuiField<int>(self, fields[i]);
    }
    const unsigned char auto_size = GuiField<unsigned char>(self, 0x134);
    const auto* save_label = FindGeometry(
        g_save_load_geometry, g_save_load_geometry_count, self);
    InventoryTextContrast contrast(save_label != nullptr, true);
    const int old_width = GuiField<int>(self, GuiObjectField::width);
    const int old_height = GuiField<int>(self, GuiObjectField::height);
    const unsigned char measured = GuiField<unsigned char>(self, 0x14C);
    if (save_label) {
        // The save controller rewrites money labels to 145x20 after reflow.
        // Repair the draw/clip bounds from authored geometry on every draw.
        GuiField<int>(self, GuiObjectField::width) =
            MulDiv(save_label->width, viewport.width, LegacyCanvas::width);
        GuiField<int>(self, GuiObjectField::height) =
            MulDiv(save_label->height, viewport.height, LegacyCanvas::height);
        // 0053072B skips measurement while the +14C reentry guard is set.
        // Restore it with the native metrics after this temporary draw.
        GuiField<unsigned char>(self, 0x14C) = 0;
    }
    const auto fitted = FitPageTextMetrics(font, saved[1], viewport.height);
    GuiField<int>(self, 0x130) = fitted.font_size;
    GuiField<int>(self, 0x140) = fitted.line_gap;
    // Use the game's own measurement with the fitted bounds/font so centred,
    // right-aligned and multiline strings share the renderer's wrap rules.
    // Disable auto-resize during this render-only measurement.
    GuiField<unsigned char>(self, 0x134) = 0;
    g_fitted_text_measure(self);
    g_fitted_text_draw(self, renderer, time, flags);
    if (save_label) {
        GuiField<int>(self, GuiObjectField::width) = old_width;
        GuiField<int>(self, GuiObjectField::height) = old_height;
        GuiField<unsigned char>(self, 0x14C) = measured;
    }
    GuiField<unsigned char>(self, 0x134) = auto_size;
    for (size_t i = 0; i < std::size(fields); ++i) {
        GuiField<int>(self, fields[i]) = saved[i];
    }
}

void __fastcall HookCompanyListDraw(void* self, void*, void* renderer,
                                    uint32_t time, uint32_t flags) {
    GuiObjectReadBatch read_batch;
    if (!IsAspectFittedTextObject(self) ||
        !CanReadGuiObject(static_cast<unsigned char*>(self) + 0x50)) {
        g_company_list_draw(self, renderer, time, flags);
        return;
    }
    // Live inventory Memo vtable 006F03F8 points to 00533700, shared with
    // company lists, rather than the separate 00530310 text renderer.
    const auto* inventory = FindGeometry(
        g_inventory_geometry, g_inventory_geometry_count, self);
    InventoryTextContrast contrast(inventory && inventory->width == 200 &&
        inventory->height == 60);
    // 00533700 draws the list's text directly, without GuiMove children.
    // Keep logical wrapping/scrolling/month indices native; fit these six
    // render metrics only for the duration of the draw and then restore them.
    constexpr size_t fields[] = {0x11C, 0x120, 0x124, 0x128, 0x13C, 0x144};
    const CompanyListMetrics native = {
        GuiField<int>(self, fields[0]), GuiField<int>(self, fields[1]),
        GuiField<int>(self, fields[2]), GuiField<int>(self, fields[3]),
        GuiField<int>(self, fields[4]), GuiField<int>(self, fields[5])};
    const RectI viewport = AspectFitLegacyCanvas(
        static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height));
    const auto fitted = FitCompanyListMetrics(native, viewport.width, viewport.height);
    const int before[] = {native.left, native.top, native.right, native.bottom,
                          native.font_size, native.line_gap};
    const int after[] = {fitted.left, fitted.top, fitted.right, fitted.bottom,
                         fitted.font_size, fitted.line_gap};
    for (size_t i = 0; i < std::size(fields); ++i) {
        GuiField<int>(self, fields[i]) = after[i];
    }
    g_company_list_draw(self, renderer, time, flags);
    for (size_t i = 0; i < std::size(fields); ++i) {
        GuiField<int>(self, fields[i]) = before[i];
    }
}

void ResetAirportSelectionGeometry(void* root) {
    if (g_airport_selection_root == root) {
        return;
    }
    g_airport_selection_root = root;
    g_airport_selection_geometry_count = 0;
}

bool IsAirportSelectionDescendant(void* object) {
    return IsDescendantOf(object, g_airport_selection_root, 8);
}

bool IsAirportAirplaneGeometry(const TitleNativeGeometry* native) {
    // Airport/Airport.txt: PlaneAirplane is the direct 54x58 visual at
    // (500,100). Keep its GUI geometry in the controller's native coordinate
    // system; its final vertices are aspect-fitted by the render hook.
    return native && native->width == 54 && native->height == 58 &&
        native->x == 500 && native->y == 100;
}

void ScaleAirportSelectionSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        int& width = GuiField<int>(object, GuiObjectField::width);
        int& height = GuiField<int>(object, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            ResetAirportSelectionGeometry(object);
            width = viewport_width;
            height = viewport_height;
            original(object, viewport_x, viewport_y);
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                g_airport_selection_geometry,
                std::size(g_airport_selection_geometry),
                g_airport_selection_geometry_count, object);
            if (native) {
                if (IsAirportAirplaneGeometry(native)) {
                    width = native->width;
                    height = native->height;
                    original(object, native->x, native->y);
                } else {
                    width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
                    height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
                    original(object,
                        MulDiv(native->x, viewport_width, LegacyCanvas::width),
                        MulDiv(native->y, viewport_height, LegacyCanvas::height));
                }
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 8, 256, enter, leave);
}

void ResetStudioEventListGeometry(void* root) {
    if (g_studio_event_list_root == root) {
        return;
    }
    g_studio_event_list_root = root;
    g_studio_event_list_geometry_count = 0;
}

void ScaleStudioEventListSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_studio_event_list_geometry,
        g_studio_event_list_geometry_count, ResetStudioEventListGeometry,
        8, 2048);
}

void ResetStudioEventEditorGeometry(void* root) {
    if (g_studio_event_editor_root == root) {
        return;
    }
    g_studio_event_editor_root = root;
    g_studio_event_editor_geometry_count = 0;
}

void ScaleStudioEventEditorSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_studio_event_editor_geometry,
        g_studio_event_editor_geometry_count, ResetStudioEventEditorGeometry,
        8, 4096);
}

StudioEventDropdownLayout* FindStudioEventDropdownLayout(void* root) {
    for (size_t i = 0; i < g_studio_event_dropdown_count; ++i) {
        if (g_studio_event_dropdowns[i].root == root) {
            return &g_studio_event_dropdowns[i];
        }
    }
    return nullptr;
}

StudioEventDropdownLayout* RegisterStudioEventDropdownLayout(
        void* root, int native_width, int native_height) {
    StudioEventDropdownLayout* layout =
        FindStudioEventDropdownLayout(root);
    if (!layout) {
        if (g_studio_event_dropdown_count >=
            std::size(g_studio_event_dropdowns)) {
            return nullptr;
        }
        layout = &g_studio_event_dropdowns[
            g_studio_event_dropdown_count++];
        *layout = {};
        layout->root = root;
    } else {
        // GUI pools can reuse the same addresses when returning to the editor.
        // A native-sized signature means the resource has been reconstructed,
        // so discard child geometry retained from its previous lifetime.
        layout->geometry_count = 0;
    }
    layout->native_width = native_width;
    layout->native_height = native_height;
    layout->tutorial_mode = false;
    return layout;
}

StudioEventDropdownLayout* FindStudioEventDropdownOwner(void* object) {
    for (size_t i = 0; i < g_studio_event_dropdown_count; ++i) {
        StudioEventDropdownLayout& layout = g_studio_event_dropdowns[i];
        if (object == layout.root ||
            IsDescendantOf(object, layout.root, 2)) {
            return &layout;
        }
    }
    return nullptr;
}

bool IsTitleTutorialDropdownAnchor(int x, int y) {
    const bool birthday =
        (std::abs(x - 268) <= 4 || std::abs(x - 359) <= 4) &&
        std::abs(y - 434) <= 4;
    const bool blood_type = std::abs(x - 550) <= 4 &&
        std::abs(y - 405) <= 4;
    return birthday || blood_type;
}

void RestoreTitleTutorialDropdown(
        StudioEventDropdownLayout& layout, int native_x, int native_y) {
    if (!CanReadGuiObject(layout.root) || !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    for (size_t i = 0; i < layout.geometry_count; ++i) {
        TitleNativeGeometry& native = layout.geometry[i];
        if (!CanReadGuiObject(native.object)) {
            continue;
        }
        auto* bytes = static_cast<unsigned char*>(native.object);
        GuiField<int>(bytes, GuiObjectField::width) = native.width;
        GuiField<int>(bytes, GuiObjectField::height) = native.height;
        original(native.object, native.x, native.y);
    }
    auto* root_bytes = static_cast<unsigned char*>(layout.root);
    GuiField<int>(root_bytes, GuiObjectField::width) = layout.native_width;
    GuiField<int>(root_bytes, GuiObjectField::height) = layout.native_height;
    original(layout.root,
        native_x + (static_cast<int>(g_unified_ui.width) -
            LegacyCanvas::width) / 2,
        native_y + (static_cast<int>(g_unified_ui.height) -
            LegacyCanvas::height) / 2);
    layout.tutorial_mode = true;
    RememberProcessedLayoutObject(layout.root);
}

void ScaleStudioEventDropdownSubtree(void* object,
                                     StudioEventDropdownLayout& layout,
                                     int native_root_x,
                                     int native_root_y,
                                     int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            width = MulDiv(layout.native_width,
                viewport_width, LegacyCanvas::width);
            height = MulDiv(layout.native_height,
                viewport_height, LegacyCanvas::height);
            original(object,
                viewport_x + MulDiv(native_root_x, viewport_width, LegacyCanvas::width),
                viewport_y + MulDiv(native_root_y, viewport_height, LegacyCanvas::height));
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                layout.geometry,
                std::size(layout.geometry),
                layout.geometry_count, object);
            if (native) {
                width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
                height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
                original(object,
                    MulDiv(native->x, viewport_width, LegacyCanvas::width),
                    MulDiv(native->y, viewport_height, LegacyCanvas::height));
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 2, 16, enter, leave);
}

void ResetTrainingMinigameGeometry(void* root) {
    if (g_training_minigame_root == root) {
        return;
    }
    g_training_minigame_root = root;
    g_training_minigame_geometry_count = 0;
    g_training_activity_geometry_count = 0;
    g_training_activity_root_count = 0;
}

bool IsTrainingMinigameDescendant(void* object) {
    return IsDescendantOf(object, g_training_minigame_root);
}

bool IsTrainingTimerGeometry(const TitleNativeGeometry* native) {
    if (!native) {
        return false;
    }
    const bool timer_panel = native->x >= 53 && native->x <= 57 &&
        native->y >= 2 && native->y <= 6 &&
        native->width >= 213 && native->width <= 217 &&
        native->height >= 30 && native->height <= 34;
    const bool progress_bar = native->x >= 5 && native->x <= 9 &&
        native->y >= 5 && native->y <= 9 &&
        native->width >= 199 && native->width <= 203 &&
        native->height >= 13 && native->height <= 17;
    return timer_panel || progress_bar;
}

bool IsTrainingActivityRoot(void* object) {
    for (size_t i = 0; i < g_training_activity_root_count; ++i) {
        if (g_training_activity_roots[i] == object) {
            return true;
        }
    }
    return false;
}

bool RememberTrainingActivityRoot(void* object) {
    if (IsTrainingActivityRoot(object)) {
        return true;
    }
    if (g_training_activity_root_count >=
            std::size(g_training_activity_roots)) {
        return false;
    }
    g_training_activity_roots[g_training_activity_root_count++] = object;
    return true;
}

bool IsTrainingActivityPage(void* object, void* parent,
                            int width, int height) {
    if (!CanReadGuiObject(g_training_minigame_root) ||
        !CanReadGuiObject(object) || parent != g_unified_ui.primary_root) {
        return false;
    }
    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    const bool native_page = width >= 798 && width <= 802 &&
        height >= 598 && height <= 602;
    const bool scaled_page = std::abs(width - viewport_width) <= 2 &&
        std::abs(height - viewport_height) <= 2;
    if (!native_page && !scaled_page) {
        return false;
    }

    // Every training activity owns a separate 800x600 direct-root page with
    // one 400x300 playfield at (200,175). The pages coexist in the root list;
    // the game selects which one to draw for the current training type.
    size_t playfields = 0;
    size_t visited = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    while (CanReadGuiObject(child) && visited++ < 8) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_x = GuiField<int>(bytes, GuiObjectField::x);
        const int child_y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        if ((native_page && child_x >= 198 && child_x <= 202 &&
             child_y >= 173 && child_y <= 177 &&
             child_width >= 398 && child_width <= 402 &&
             child_height >= 298 && child_height <= 302) ||
            (scaled_page &&
             std::abs(child_x - MulDiv(200, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_y - MulDiv(175, viewport_height, LegacyCanvas::height)) <= 2 &&
             std::abs(child_width - MulDiv(400, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_height - MulDiv(300, viewport_height, LegacyCanvas::height)) <= 2)) {
            ++playfields;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return playfields == 1;
}

void ScaleTrainingMinigameSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            // The minigame frame is authored at (187,126). Generic root-child
            // anchoring may already have added the centred legacy-canvas offset;
            // use the authored coordinates explicitly instead of scaling that
            // output-space position a second time.
            constexpr int kNativeX = 187;
            constexpr int kNativeY = 126;
            ResetTrainingMinigameGeometry(object);
            width = MulDiv(426, viewport_width, LegacyCanvas::width);
            height = MulDiv(369, viewport_height, LegacyCanvas::height);
            original(object,
                viewport_x + MulDiv(kNativeX, viewport_width, LegacyCanvas::width),
                viewport_y + MulDiv(kNativeY, viewport_height, LegacyCanvas::height));
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                g_training_minigame_geometry,
                std::size(g_training_minigame_geometry),
                g_training_minigame_geometry_count, object);
            if (native) {
                width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
                height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
                original(object,
                    MulDiv(native->x, viewport_width, LegacyCanvas::width),
                    MulDiv(native->y, viewport_height, LegacyCanvas::height));
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 8, 64, enter, leave);
}

void ScaleTrainingActivitySubtree(void* object, int depth = 0) {
    // Registration can fail when all 16 activity-root slots are occupied.
    // The common template must stop before fitting that unregistered subtree.
    ScaleAspectFitSubtree(
        object, depth, g_training_activity_geometry,
        g_training_activity_geometry_count, RememberTrainingActivityRoot, 8, 256);
}

void ResetTitleTutorialGeometry(void* root) {
    if (g_title_tutorial_root == root) {
        return;
    }
    g_title_tutorial_root = root;
    g_title_tutorial_geometry_count = 0;
}

void ScaleTitleTutorialSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_title_tutorial_geometry,
        g_title_tutorial_geometry_count, ResetTitleTutorialGeometry, 8, 128);
}

void ResetTitleTutorialBubbleGeometry(void* bubble) {
    if (g_title_tutorial_bubble == bubble) {
        return;
    }
    g_title_tutorial_bubble = bubble;
    g_title_tutorial_bubble_geometry_count = 0;
}

void ScaleTitleTutorialBubbleSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_title_tutorial_bubble_geometry,
            std::size(g_title_tutorial_bubble_geometry),
            g_title_tutorial_bubble_geometry_count, object);
        if (native) {
            width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
            height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
            const int scaled_x = MulDiv(native->x, viewport_width, LegacyCanvas::width);
            const int scaled_y = MulDiv(native->y, viewport_height, LegacyCanvas::height);
            original(object,
                depth == 0 ? viewport_x + scaled_x : scaled_x,
                depth == 0 ? viewport_y + scaled_y : scaled_y);
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 4, 16, enter, leave);
}

void ResetTitleTutorialQuestionGeometry(void* root) {
    if (g_title_tutorial_question_root == root) {
        return;
    }
    g_title_tutorial_question_root = root;
    g_title_tutorial_question_geometry_count = 0;
}

void ScaleTitleTutorialQuestionSubtree(void* object, int depth = 0) {
    ScaleAspectFitSubtree(
        object, depth, g_title_tutorial_question_geometry,
        g_title_tutorial_question_geometry_count,
        ResetTitleTutorialQuestionGeometry, 4, 16);
}

void ResetAnnouncementGeometry(void* root) {
    if (g_announcement_root == root) {
        return;
    }
    g_announcement_root = root;
    g_announcement_geometry_count = 0;
    g_announcement_artist_count = 0;
}

size_t DetectAnnouncementArtistCount(void* root) {
    if (!CanReadGuiObject(root)) {
        return 0;
    }
    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    size_t visible_result_cards = 0;
    size_t visited = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(root) + 0xF4);
    while (CanReadGuiObject(child) && visited++ < 32) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
        const bool result_card =
            (width >= 95 && width <= 105 &&
             height >= 115 && height <= 125) ||
            (width >= MulDiv(95, viewport_width, LegacyCanvas::width) &&
             width <= MulDiv(105, viewport_width, LegacyCanvas::width) &&
             height >= MulDiv(115, viewport_height, LegacyCanvas::height) &&
             height <= MulDiv(125, viewport_height, LegacyCanvas::height));
        if (result_card && *(bytes + 0x99) != 0) {
            ++visible_result_cards;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return std::min<size_t>(visible_result_cards, 4);
}

void GetFirstAnnouncementCaptionPosition(size_t artist_count,
                                         int& x, int& y) {
    // These are the game's native slot formulas. Only the first caption is
    // never given its final Move call, so reconstruct that one from the
    // number of visible result cards; all remaining captions stay dynamic.
    if (artist_count == 1) {
        x = 220;
        y = 322;
    } else if (artist_count == 2) {
        x = 70;
        y = 202;
    } else {
        x = 25;
        y = 202;
    }
}

bool IsAnnouncementDescendant(void* object) {
    if (!IsCachedAnnouncementRootValid()) {
        return false;
    }
    return IsDescendantOf(object, g_announcement_root, 8);
}

void ScaleAnnouncementSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        const bool owns_scaling_guard = depth == 0;
        if (owns_scaling_guard) {
            g_scaling_announcement_subtree = true;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            ResetAnnouncementGeometry(object);
            const size_t artist_count = DetectAnnouncementArtistCount(object);
            if (artist_count != g_announcement_artist_count) {
                g_announcement_artist_count = artist_count;
                Log("Unified UI announcement artist slots=%zu", artist_count);
            }
            width = viewport_width;
            height = viewport_height;
            original(object, viewport_x, viewport_y);
        } else {
            TitleNativeGeometry* native = RememberGeometry(
                g_announcement_geometry, std::size(g_announcement_geometry),
                g_announcement_geometry_count, object);
            if (native) {
                // The first result caption is left at its construction position;
                // unlike the later captions the game never issues its final Move.
                if (depth == 1 && native->x == 0 && native->y == 0 &&
                    native->width == 145 && native->height == 20 &&
                    *(bytes + 0x99) != 0) {
                    GetFirstAnnouncementCaptionPosition(
                        g_announcement_artist_count, native->x, native->y);
                }
                width = MulDiv(native->width, viewport_width, LegacyCanvas::width);
                height = MulDiv(native->height, viewport_height, LegacyCanvas::height);
                original(object,
                    MulDiv(native->x, viewport_width, LegacyCanvas::width),
                    MulDiv(native->y, viewport_height, LegacyCanvas::height));
            }
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {
        if (node_depth == 0) {
            g_scaling_announcement_subtree = false;
        }
    };
    VisitGuiSubtree(object, depth, 6, 128, enter, leave);
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
    void* canvas = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    if (!CanReadGuiObject(canvas)) {
        return false;
    }
    auto* canvas_bytes = static_cast<unsigned char*>(canvas);
    const int x = GuiField<int>(canvas_bytes, GuiObjectField::x);
    const int y = GuiField<int>(canvas_bytes, GuiObjectField::y);
    const int width = GuiField<int>(canvas_bytes, GuiObjectField::width);
    const int height = GuiField<int>(canvas_bytes, GuiObjectField::height);
    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
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
        x = MulDiv(x, viewport_width, LegacyCanvas::width);
        y = MulDiv(y, viewport_height, LegacyCanvas::height);

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
        x = MulDiv(x, viewport_width, LegacyCanvas::width);
        y = MulDiv(y, viewport_height, LegacyCanvas::height);
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
        const int native_source_x = MulDiv(current_x, LegacyCanvas::width, viewport_width);
        sequence->target_native_x = native_source_x +
            g_unified_ui.photo_album_animation_direction * 60;
        sequence->target_native_y = MulDiv(
            current_y, LegacyCanvas::height, viewport_height);
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
        sequence->target_native_x, viewport_width, LegacyCanvas::width);
    const int target_y = MulDiv(
        sequence->target_native_y, viewport_height, LegacyCanvas::height);
    x = static_cast<int>(std::lround(
        sequence->source_x +
        (target_x - sequence->source_x) * progress));
    y = static_cast<int>(std::lround(
        sequence->source_y +
        (target_y - sequence->source_y) * progress));
    return true;
}

void ScalePhotoAlbumSubtree(void* object, int depth = 0) {
    auto enter = [&](void* object, int depth) -> bool {
        if (!g_unified_ui.trampoline) {
            return false;
        }
        auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
        auto* bytes = static_cast<unsigned char*>(object);
        int& x = GuiField<int>(bytes, GuiObjectField::x);
        int& y = GuiField<int>(bytes, GuiObjectField::y);
        int& width = GuiField<int>(bytes, GuiObjectField::width);
        int& height = GuiField<int>(bytes, GuiObjectField::height);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        if (depth == 0) {
            width = viewport_width;
            height = viewport_height;
            original(object, viewport_x, viewport_y);
        } else {
            const int scaled_x = MulDiv(x, viewport_width, LegacyCanvas::width);
            const int scaled_y = MulDiv(y, viewport_height, LegacyCanvas::height);
            width = MulDiv(width, viewport_width, LegacyCanvas::width);
            height = MulDiv(height, viewport_height, LegacyCanvas::height);
            original(object, scaled_x, scaled_y);
        }
        RememberProcessedLayoutObject(object);
        return true;
    };
    auto leave = [&](void*, int node_depth) {

    };
    VisitGuiSubtree(object, depth, 8, 512, enter, leave);
}

bool IsNearAuthoredCoordinate(int value, int expected) {
    return std::abs(value - expected) <= 4;
}

bool IsScheduleTooltipAuthoredX(int x) {
    return IsNearAuthoredCoordinate(x, 125) ||
        IsNearAuthoredCoordinate(x, 275) ||
        IsNearAuthoredCoordinate(x, 445) ||
        IsNearAuthoredCoordinate(x, 595);
}

bool IsRepeatedObjectPosition(void* object, int x, int y) {
    return IsProcessedLayoutObject(object) && CanReadGuiObject(object) &&
        GuiField<int>(object, GuiObjectField::x) == x &&
        GuiField<int>(object, GuiObjectField::y) == y;
}

void TransformScheduleJobHoverTooltip(void* object, int& x, int& y) {
    if (IsRepeatedObjectPosition(object, x, y)) {
        return;
    }
    const int center_x =
        (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
    const int center_y =
        (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
    // The schedule owns four authored artist-column x coordinates. Check
    // those resource positions directly rather than comparing x with a
    // resolution-dependent threshold: at 1280x720, valid legacy columns can
    // be numerically greater than the old threshold while output coordinates
    // can be smaller than it.
    if (IsScheduleTooltipAuthoredX(x)) {
        x += center_x;
    }
    y += center_y;
}

bool TransformCenteredPageOverlay(void* object, void* parent,
                                  int width, int height, int& x, int& y) {
    if (!CanReadGuiObject(object) ||
        parent != g_unified_ui.primary_root || IsGroupCanvas(object) ||
        width < 80 || width >= 760 || height < 25 || height >= 560 ||
        !GuiPointer(object, GuiObjectField::first_child)) {
        return false;
    }

    const RectI current{x, y, width, height};
    for (size_t i = 0; i < g_unified_ui.group_canvas_count; ++i) {
        void* page = g_unified_ui.group_canvases[i];
        if (!CanReadGuiObject(page) ||
            GuiPointer(page, GuiObjectField::parent) !=
                g_unified_ui.primary_root ||
            *(static_cast<unsigned char*>(page) + 0x99) == 0) {
            continue;
        }

        void* content = GuiPointer(page, GuiObjectField::first_child);
        size_t visited = 0;
        while (CanReadGuiObject(content) && visited++ < 256) {
            const RectI page_content{
                GuiField<int>(content, GuiObjectField::x),
                GuiField<int>(content, GuiObjectField::y),
                GuiField<int>(content, GuiObjectField::width),
                GuiField<int>(content, GuiObjectField::height),
            };
            // Large, bounded child containers describe a page's actual
            // content. Some pages expose a smaller inner panel, while others
            // (including the schedule) expose their populated 800x600 canvas
            // directly. Candidate overlay dimensions and overlap provide the
            // decoration/HUD rejection; excluding a full canvas here drops
            // legitimate detached controls from otherwise identical pages.
            if (page_content.x >= 0 && page_content.y >= 0 &&
                page_content.x + page_content.width <= LegacyCanvas::width &&
                page_content.y + page_content.height <= LegacyCanvas::height &&
                page_content.width >= 240 &&
                page_content.height >= 120 &&
                GuiPointer(content, GuiObjectField::first_child)) {
                PointI resolved{};
                if (ResolveCenteredPageOverlayPosition(
                        current, page_content,
                        static_cast<int>(g_unified_ui.width),
                        static_cast<int>(g_unified_ui.height), resolved)) {
                    x = resolved.x;
                    y = resolved.y;
                    return true;
                }
            }
            content = GuiPointer(content, GuiObjectField::next_sibling);
        }
    }
    return false;
}

void TransformCenteredScheduleOverlay(void* object, void* parent,
                                      int width, int height,
                                      int& x, int& y) {
    // Resolve the picker through the same page-ownership rule as every other
    // detached page popup. This accepts its resource position, settled +18px
    // frame, mixed native/centered coordinates, and previously misanchored
    // output position without enumerating any of those coordinates. Always
    // re-evaluate it: a page can become visible after this object was marked
    // processed, and the transform is idempotent once ownership is known.
    TransformCenteredPageOverlay(object, parent, width, height, x, y);
    // Its vertical animation/staging coordinates are not useful after the
    // schedule page is centered; keep the settled legacy baseline instead.
    y = 167 +
        (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
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
    x = legacy_x + (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
    y = legacy_y + (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
}

void TransformTitleTutorialProfileDropdown(void* object, int& x, int& y) {
    if (IsRepeatedObjectPosition(object, x, y)) {
        return;
    }
    // The seven-child dropdown signature already proves this is an authored
    // tutorial control. Convert every new game-supplied position from the
    // legacy page instead of guessing its coordinate space from the numeric
    // value. Repeated output-space callbacks are handled above.
    x += (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
    y += (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
}

// The visible announcement control layer is a centered 800x600 root backed by
// four caption slots and four 100x120 result-card slots. The game shows one to
// four of those slots according to the current artist count. Its textured
// background and preview panes are submitted separately as raw XYZRHW draws,
// so this signature is used to enable the targeted render-layer correction.
bool IsAnnouncementControlRoot(void* object, void* parent,
                               int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root) {
        return false;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    const bool native_canvas = width >= 790 && width <= 810 &&
        height >= 590 && height <= 610;
    const bool scaled_canvas = std::abs(width - viewport_width) <= 2 &&
        std::abs(height - viewport_height) <= 2;
    if (!native_canvas && !scaled_canvas) {
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
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        // A pooled announcement root can be restored to 800x600 before the
        // game restores all of its children. Accept either coordinate space
        // independently so the retained native-geometry cache can repair that
        // mixed state instead of treating scaled children as new native data.
        const bool caption =
            (child_width >= 140 && child_width <= 150 &&
             child_height >= 18 && child_height <= 22) ||
            (std::abs(child_width - MulDiv(145, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_height - MulDiv(20, viewport_height, LegacyCanvas::height)) <= 2);
        if (caption) {
            ++captions;
        }
        // Result-card positions are recomputed for every artist count. Their
        // authored size is the stable part of the RunSchedule signature.
        const bool result_card =
            (child_width >= 95 && child_width <= 105 &&
             child_height >= 115 && child_height <= 125) ||
            (std::abs(child_width - MulDiv(100, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_height - MulDiv(120, viewport_height, LegacyCanvas::height)) <= 2);
        if (result_card) {
            ++result_cards;
        }
        ++count;
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return child == nullptr && captions == 4 && result_cards == 4;
}

bool IsCachedAnnouncementRootValid() {
    if (!CanReadGuiObject(g_announcement_root)) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_announcement_root);
    if (GuiField<void*>(bytes, GuiObjectField::parent) !=
            g_unified_ui.primary_root) {
        return false;
    }

    const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
        AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                              static_cast<int>(g_unified_ui.height));
    const int width = GuiField<int>(bytes, GuiObjectField::width);
    const int height = GuiField<int>(bytes, GuiObjectField::height);
    const bool native_canvas = width >= 790 && width <= 810 &&
        height >= 590 && height <= 610;
    const bool scaled_canvas = std::abs(width - viewport_width) <= 2 &&
        std::abs(height - viewport_height) <= 2;
    if (!native_canvas && !scaled_canvas) {
        return false;
    }

    size_t captions = 0;
    size_t result_cards = 0;
    size_t count = 0;
    void* child = GuiField<void*>(bytes, GuiObjectField::first_child);
    while (CanReadGuiObject(child) && count < 20) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int child_width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(child_bytes, GuiObjectField::height);
        if ((child_width >= 140 && child_width <= 150 &&
             child_height >= 18 && child_height <= 22) ||
            (std::abs(child_width - MulDiv(145, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_height - MulDiv(20, viewport_height, LegacyCanvas::height)) <= 2)) {
            ++captions;
        }
        // Do not validate the result-card position here. The game moves all
        // four cards after construction and the coordinates differ between
        // the one-, two-, three-, and four-artist arrangements.
        if ((child_width >= 95 && child_width <= 105 &&
             child_height >= 115 && child_height <= 125) ||
            (std::abs(child_width - MulDiv(100, viewport_width, LegacyCanvas::width)) <= 2 &&
             std::abs(child_height - MulDiv(120, viewport_height, LegacyCanvas::height)) <= 2)) {
            ++result_cards;
        }
        ++count;
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return child == nullptr && captions == 4 && result_cards == 4;
}

void* FindAnnouncementRoot(void* start_child, size_t maximum_nodes,
                           bool& found) {
    found = false;
    if (g_announcement_root &&
        (!CanReadGuiObject(g_announcement_root) ||
         *reinterpret_cast<void**>(
             static_cast<unsigned char*>(g_announcement_root) + 0xF0) !=
             g_unified_ui.primary_root)) {
        g_announcement_root = nullptr;
        g_announcement_geometry_count = 0;
        g_announcement_artist_count = 0;
    }
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        return nullptr;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_unified_ui.primary_root);
    void* child = start_child ? start_child :
        GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < maximum_nodes) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        const int width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int height = GuiField<int>(child_bytes, GuiObjectField::height);
        if (IsAnnouncementControlRoot(child, g_unified_ui.primary_root,
                                      width, height)) {
            ResetAnnouncementGeometry(child);
            found = true;
            return nullptr;
        }
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return visited >= maximum_nodes && CanReadGuiObject(child) ?
        child : nullptr;
}

bool DiscoverAnnouncementScreenIncremental(size_t maximum_nodes) {
    if (IsCachedAnnouncementRootValid() &&
        *(static_cast<unsigned char*>(g_announcement_root) + 0x99) != 0) {
        return true;
    }
    if (!CanReadGuiObject(g_unified_ui.primary_root)) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const bool layout_activity = g_unified_ui.processed_count !=
        g_unified_ui.announcement_discovery_processed_count;
    if (g_unified_ui.announcement_discovery_root !=
            g_unified_ui.primary_root) {
        g_unified_ui.announcement_discovery_root =
            g_unified_ui.primary_root;
        g_unified_ui.announcement_discovery_cursor = nullptr;
        g_unified_ui.announcement_discovery_active = false;
    }
    constexpr ULONGLONG kAnnouncementDiscoveryCycleIntervalMs = 250;
    if (!g_unified_ui.announcement_discovery_active &&
        (layout_activity ||
         g_unified_ui.last_announcement_discovery_cycle_tick == 0 ||
         now - g_unified_ui.last_announcement_discovery_cycle_tick >=
             kAnnouncementDiscoveryCycleIntervalMs)) {
        g_unified_ui.announcement_discovery_active = true;
        g_unified_ui.announcement_discovery_cursor = nullptr;
        g_unified_ui.announcement_discovery_processed_count =
            g_unified_ui.processed_count;
    }
    if (!g_unified_ui.announcement_discovery_active) {
        return false;
    }

    bool found = false;
    g_unified_ui.announcement_discovery_cursor = FindAnnouncementRoot(
        g_unified_ui.announcement_discovery_cursor, maximum_nodes, found);
    // FindAnnouncementRoot identifies the page while it still has its native
    // 800x600 geometry. IsCachedAnnouncementRootValid intentionally expects
    // the already aspect-fitted geometry, so using it here rejects a genuine
    // first discovery and leaves the page broken until the rare full-layout
    // integrity pass. Let the caller perform that initial fit first.
    if (found) {
        g_unified_ui.announcement_discovery_active = false;
        return *(static_cast<unsigned char*>(g_announcement_root) +
            0x99) != 0;
    }
    if (!g_unified_ui.announcement_discovery_cursor) {
        g_unified_ui.announcement_discovery_active = false;
        g_unified_ui.last_announcement_discovery_cycle_tick = now;
    }
    return false;
}

bool IsTrainingScreenPillarboxNeeded() {
    if (!g_unified_ui.announcement_active ||
        !IsCachedAnnouncementRootValid() ||
        !CanReadGuiObject(g_training_minigame_root)) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_training_minigame_root);
    return GuiField<void*>(bytes, GuiObjectField::parent) ==
        g_unified_ui.primary_root;
}

bool IsStudioEventListScreenVisible() {
    if (!CanReadGuiObject(g_studio_event_list_root)) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_studio_event_list_root);
    return GuiField<void*>(bytes, GuiObjectField::parent) ==
            g_unified_ui.primary_root && *(bytes + 0x99) != 0;
}

bool IsAirportSelectionScreenVisible() {
    if (!CanReadGuiObject(g_airport_selection_root)) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(g_airport_selection_root);
    return GuiField<void*>(bytes, GuiObjectField::parent) ==
            g_unified_ui.primary_root && *(bytes + 0x99) != 0;
}

void* g_phone_overlay_root = nullptr;
void* g_phone_overlay_button = nullptr;
ULONGLONG g_phone_overlay_last_discovery_tick = 0;
void* g_toolbar_root = nullptr;
ULONGLONG g_toolbar_last_slot_refresh_tick = 0;
GuiResizeFn g_gui_resize = nullptr;

void ReflowExistingRootChildren(void* root, int depth);

void* FindPhoneOverlayButton(void* object, void* parent,
                             int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        width != 0 || height != 0) {
        return nullptr;
    }

    // MiniGame/BababaCallOut.txt owns a zero-sized root with five direct
    // children: the talk, countdown, confirmation and question panels plus
    // the trailing 40x40 phone button. Match the complete resource structure
    // instead of the button's current position. The event controller parks
    // the button at (-40,-40) while idle, so the old y=558 signature stopped
    // recognizing the overlay after its first lifecycle transition.
    bool talk = false;
    bool countdown = false;
    bool confirmation = false;
    bool question = false;
    void* phone_button = nullptr;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 6) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        void* next = GuiField<void*>(bytes, GuiObjectField::next_sibling);
        talk |= child_width == 605 && child_height == 178;
        confirmation |= child_width == 338 && child_height == 143;
        question |= child_width == 480 && child_height == 380;
        if (child_width == 40 && child_height == 40) {
            if (next == nullptr) {
                phone_button = child;
            } else {
                countdown = true;
            }
        }
        child = next;
    }
    return child == nullptr && visited == 5 && talk && countdown &&
        confirmation && question ? phone_button : nullptr;
}

void RememberPhoneOverlay(void* root, void* button) {
    if (g_phone_overlay_root == root && g_phone_overlay_button == button) {
        return;
    }
    g_phone_overlay_root = root;
    g_phone_overlay_button = button;
}

bool IsCachedPhoneOverlayValid() {
    if (!CanReadGuiObject(g_phone_overlay_root) ||
        !CanReadGuiObject(g_phone_overlay_button)) {
        return false;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
    auto* button_bytes = static_cast<unsigned char*>(g_phone_overlay_button);
    return GuiField<void*>(root_bytes, GuiObjectField::parent) ==
            g_unified_ui.primary_root &&
        GuiField<void*>(button_bytes, GuiObjectField::parent) ==
            g_phone_overlay_root &&
        GuiField<int>(button_bytes, GuiObjectField::width) == 40 &&
        GuiField<int>(button_bytes, GuiObjectField::height) == 40;
}

void DiscoverPhoneOverlay(ULONGLONG now) {
    if (IsCachedPhoneOverlayValid() ||
        !CanReadGuiObject(g_unified_ui.primary_root)) {
        return;
    }

    constexpr ULONGLONG kPhoneOverlayDiscoveryIntervalMs = 250;
    if (g_phone_overlay_last_discovery_tick != 0 &&
        now - g_phone_overlay_last_discovery_tick <
            kPhoneOverlayDiscoveryIntervalMs) {
        return;
    }
    g_phone_overlay_last_discovery_tick = now;
    g_phone_overlay_root = nullptr;
    g_phone_overlay_button = nullptr;

    auto* primary_bytes =
        static_cast<unsigned char*>(g_unified_ui.primary_root);
    void* child = GuiField<void*>(primary_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 4096) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        void* next = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
        const int width = GuiField<int>(child_bytes, GuiObjectField::width);
        const int height = GuiField<int>(child_bytes, GuiObjectField::height);
        if (void* phone_button = FindPhoneOverlayButton(
                child, g_unified_ui.primary_root, width, height)) {
            RememberPhoneOverlay(child, phone_button);
            RememberLayoutRoot(child);
            ReflowExistingRootChildren(child, 1);
            return;
        }
        child = next;
    }
}

bool IsPhoneOverlayRinging() {
    if (!IsCachedPhoneOverlayValid()) {
        return false;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
    auto* button_bytes = static_cast<unsigned char*>(g_phone_overlay_button);
    if (GuiField<void*>(root_bytes, GuiObjectField::parent) !=
            g_unified_ui.primary_root ||
        GuiField<void*>(button_bytes, GuiObjectField::parent) !=
            g_phone_overlay_root ||
        *(button_bytes + 0x99) == 0) {
        return false;
    }

    // BababaCallOut's PlaneBase is a zero-sized structural container. The
    // game keeps its own visibility byte cleared even while a visible child
    // (including BtnPhone) is rendered, so it cannot be used as the event's
    // visibility gate. BtnPhone and the four sibling panels carry the actual
    // phase state.

    // The ringing phase exposes only BtnPhone. Once the player answers, one
    // of the dialogue/question panels becomes visible and the controller is
    // intentionally allowed to park the button off-screen again.
    void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 6) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        if (child != g_phone_overlay_button && *(child_bytes + 0x99) != 0) {
            return false;
        }
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return child == nullptr;
}

bool IsPhoneOverlayButtonVisible() {
    if (!IsCachedPhoneOverlayValid()) {
        return false;
    }
    return *(static_cast<unsigned char*>(g_phone_overlay_button) + 0x99) != 0;
}

bool IsPhoneOverlayDialogueVisible() {
    if (!IsCachedPhoneOverlayValid()) {
        return false;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
    if (*(root_bytes + 0x99) == 0) {
        return false;
    }
    void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 6) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        if (child != g_phone_overlay_button && *(child_bytes + 0x99) != 0) {
            return true;
        }
        child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
    }
    return false;
}

void LogPhoneOverlayStateIfChanged() {
    if (!IsDebugModeEnabled()) {
        return;
    }

    struct PhoneOverlayDiagnosticState {
        void* root = nullptr;
        void* button = nullptr;
        int x = 0;
        int y = 0;
        unsigned char root_visible = 0;
        unsigned char button_visible = 0;
        unsigned int sibling_visible_mask = 0;
        bool announcement_active = false;
        bool valid = false;
    };
    static PhoneOverlayDiagnosticState previous{};
    static bool initialized = false;

    PhoneOverlayDiagnosticState current{};
    current.root = g_phone_overlay_root;
    current.button = g_phone_overlay_button;
    current.announcement_active = g_unified_ui.announcement_active;
    current.valid = IsCachedPhoneOverlayValid();
    if (current.valid) {
        auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
        auto* button_bytes = static_cast<unsigned char*>(g_phone_overlay_button);
        current.x = GuiField<int>(button_bytes, GuiObjectField::x);
        current.y = GuiField<int>(button_bytes, GuiObjectField::y);
        current.root_visible = *(root_bytes + 0x99);
        current.button_visible = *(button_bytes + 0x99);
        void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
        size_t index = 0;
        while (CanReadGuiObject(child) && index < 5) {
            auto* child_bytes = static_cast<unsigned char*>(child);
            if (child != g_phone_overlay_button &&
                *(child_bytes + 0x99) != 0) {
                current.sibling_visible_mask |= 1u << index;
            }
            child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
            ++index;
        }
    }

    const bool changed = !initialized || current.root != previous.root ||
        current.button != previous.button || current.x != previous.x ||
        current.y != previous.y ||
        current.root_visible != previous.root_visible ||
        current.button_visible != previous.button_visible ||
        current.sibling_visible_mask != previous.sibling_visible_mask ||
        current.announcement_active != previous.announcement_active ||
        current.valid != previous.valid;
    if (changed) {
        Log("Phone overlay state valid=%d root=%p visible=%u button=%p visible=%u "
            "pos=%d,%d siblings=0x%02X announcement=%d ringing=%d",
            current.valid ? 1 : 0, current.root,
            static_cast<unsigned int>(current.root_visible), current.button,
            static_cast<unsigned int>(current.button_visible),
            current.x, current.y, current.sibling_visible_mask,
            current.announcement_active ? 1 : 0,
            IsPhoneOverlayRinging() ? 1 : 0);
        if (current.valid &&
            (!initialized || current.sibling_visible_mask !=
                previous.sibling_visible_mask)) {
            auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
            void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
            size_t index = 0;
            while (CanReadGuiObject(child) && index < 5) {
                auto* child_bytes = static_cast<unsigned char*>(child);
                Log("Phone overlay child #%zu self=%p button=%d visible=%u "
                    "rect=%d,%d %dx%d",
                    index, child, child == g_phone_overlay_button ? 1 : 0,
                    static_cast<unsigned int>(*(child_bytes + 0x99)),
                    GuiField<int>(child_bytes, GuiObjectField::x),
                    GuiField<int>(child_bytes, GuiObjectField::y),
                    GuiField<int>(child_bytes, GuiObjectField::width),
                    GuiField<int>(child_bytes, GuiObjectField::height));
                child = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
                ++index;
            }
        }
        previous = current;
        initialized = true;
    }
}

void GetPhoneOverlayButtonPosition(int& x, int& y) {
    x = 3;
    y = 558;
    TransformRootChildPosition(40, 40, x, y);
}

bool GetPhoneOverlayDialoguePanelPosition(
        int width, int height, int& x, int& y) {
    if (width == 605 && height == 178) {
        x = 84;
        y = 390;
    } else if (width == 40 && height == 40) {
        x = 590;
        y = 116;
    } else if (width == 338 && height == 143) {
        x = 232;
        y = 346;
    } else if (width == 480 && height == 380) {
        x = 160;
        y = 110;
    } else {
        return false;
    }
    TransformRootChildPosition(width, height, x, y);
    return true;
}

void RefreshPhoneOverlayDialoguePanels(const char* phase) {
    if (!g_unified_ui.trampoline || !IsCachedPhoneOverlayValid()) {
        return;
    }
    auto* root_bytes = static_cast<unsigned char*>(g_phone_overlay_root);
    if (*(root_bytes + 0x99) == 0) {
        return;
    }

    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 6) {
        auto* bytes = static_cast<unsigned char*>(child);
        void* next = GuiField<void*>(bytes, GuiObjectField::next_sibling);
        if (child != g_phone_overlay_button && *(bytes + 0x99) != 0) {
            const int width = GuiField<int>(bytes, GuiObjectField::width);
            const int height = GuiField<int>(bytes, GuiObjectField::height);
            const int old_x = GuiField<int>(bytes, GuiObjectField::x);
            const int old_y = GuiField<int>(bytes, GuiObjectField::y);
            const bool parked = old_x + width <= 0 || old_y + height <= 0;
            int target_x = 0;
            int target_y = 0;
            if (parked && GetPhoneOverlayDialoguePanelPosition(
                    width, height, target_x, target_y)) {
                original(child, target_x, target_y);
                const int moved_x = GuiField<int>(bytes, GuiObjectField::x);
                const int moved_y = GuiField<int>(bytes, GuiObjectField::y);
                const bool rejected = moved_x != target_x || moved_y != target_y;
                if (rejected) {
                    GuiField<int>(bytes, GuiObjectField::x) = target_x;
                    GuiField<int>(bytes, GuiObjectField::y) = target_y;
                }
                if (IsDebugModeEnabled()) {
                    Log("Phone overlay dialogue refresh phase=%s self=%p "
                        "before=%d,%d size=%dx%d target=%d,%d rejected=%d "
                        "final=%d,%d",
                        phase, child, old_x, old_y, width, height,
                        target_x, target_y, rejected ? 1 : 0,
                        GuiField<int>(bytes, GuiObjectField::x),
                        GuiField<int>(bytes, GuiObjectField::y));
                }
                RememberProcessedLayoutObject(child);
            }
        }
        child = next;
    }
}

void RefreshPhoneOverlayButton(const char* phase) {
    const bool debug = IsDebugModeEnabled();
    const bool valid = IsCachedPhoneOverlayValid();
    const bool ringing = valid && IsPhoneOverlayRinging();
    if (!g_unified_ui.trampoline || !ringing) {
        if (debug && valid && IsPhoneOverlayButtonVisible()) {
            static bool skip_logged = false;
            if (!skip_logged) {
                auto* button_bytes =
                    static_cast<unsigned char*>(g_phone_overlay_button);
                Log("Phone overlay refresh phase=%s skipped trampoline=%p "
                    "ringing=%d pos=%d,%d",
                    phase, g_unified_ui.trampoline, ringing ? 1 : 0,
                    GuiField<int>(button_bytes, GuiObjectField::x),
                    GuiField<int>(button_bytes, GuiObjectField::y));
                skip_logged = true;
            }
        }
        return;
    }

    auto* button_bytes = static_cast<unsigned char*>(g_phone_overlay_button);
    int target_x = 0;
    int target_y = 0;
    GetPhoneOverlayButtonPosition(target_x, target_y);
    const int x = GuiField<int>(button_bytes, GuiObjectField::x);
    const int y = GuiField<int>(button_bytes, GuiObjectField::y);
    if (x == target_x && y == target_y) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    original(g_phone_overlay_button, target_x, target_y);
    int after_x = GuiField<int>(button_bytes, GuiObjectField::x);
    int after_y = GuiField<int>(button_bytes, GuiObjectField::y);
    const bool move_rejected = after_x != target_x || after_y != target_y;
    if (move_rejected) {
        // BtnPhone is retained under BababaCallOut's zero-sized structural
        // root. The base GuiMove routine returns without updating this child,
        // even though ordinary root children accept the same output-space
        // coordinates. Its renderer consumes these canonical position fields,
        // so preserve the base call and only fall back when it demonstrably
        // rejected the requested move.
        GuiField<int>(button_bytes, GuiObjectField::x) = target_x;
        GuiField<int>(button_bytes, GuiObjectField::y) = target_y;
        after_x = target_x;
        after_y = target_y;
    }
    if (debug) {
        static unsigned int attempt_count = 0;
        if (attempt_count < 32) {
            ++attempt_count;
            Log("Phone overlay refresh phase=%s attempt=%u before=%d,%d "
                "target=%d,%d rejected=%d final=%d,%d",
                phase, attempt_count, x, y, target_x, target_y,
                move_rejected ? 1 : 0,
                after_x, after_y);
        }
    }
    RememberProcessedLayoutObject(g_phone_overlay_button);
}

void RefreshPhoneOverlayButtonBeforeDraw() {
    RefreshPhoneOverlayButton("first-draw");
    RefreshPhoneOverlayDialoguePanels("first-draw");
    if (IsDebugModeEnabled()) {
        LogPhoneOverlayStateIfChanged();
    }
}

int CountVisibleToolbarSlots(void* object, void* parent,
                             int width, int height) {
    if (!CanReadGuiObject(object) || parent != g_unified_ui.primary_root ||
        !((width >= 310 && width <= 330) ||
          (width >= 270 && width <= 280) ||
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
        const int x = GuiField<int>(bytes, GuiObjectField::x);
        const int y = GuiField<int>(bytes, GuiObjectField::y);
        const int child_width = GuiField<int>(bytes, GuiObjectField::width);
        const int child_height = GuiField<int>(bytes, GuiObjectField::height);
        const bool visible = GuiField<unsigned char>(bytes, GuiObjectField::visible) != 0;
        if (visible && x >= 0 && x <= 320 && y >= 0 && y <= 10 &&
            child_width >= 35 && child_width <= 42 &&
            child_height >= 35 && child_height <= 42) {
            ++visible_count;
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    return visible_count;
}

void NormalizeVisibleToolbarSlots(void* object, int expected_slots) {
    if (!CanReadGuiObject(object) || !g_unified_ui.trampoline ||
        expected_slots < 5 || expected_slots > 7) {
        return;
    }

    struct VisibleToolbarSlot {
        void* object = nullptr;
        int x = 0;
        int y = 0;
    } slots[7]{};
    size_t slot_count = 0;
    void* child = *reinterpret_cast<void**>(
        static_cast<unsigned char*>(object) + 0xF4);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 16) {
        auto* bytes = static_cast<unsigned char*>(child);
        const int x = GuiField<int>(bytes, GuiObjectField::x);
        const int y = GuiField<int>(bytes, GuiObjectField::y);
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
        const bool visible = *(bytes + 0x99) != 0;
        if (visible && x >= 0 && x <= 320 && y >= 0 && y <= 10 &&
            width >= 35 && width <= 42 &&
            height >= 35 && height <= 42 && slot_count < std::size(slots)) {
            slots[slot_count++] = {child, x, y};
        }
        child = GuiField<void*>(bytes, GuiObjectField::next_sibling);
    }
    if (slot_count != static_cast<size_t>(expected_slots)) {
        return;
    }

    std::sort(std::begin(slots), std::begin(slots) + slot_count,
        [](const VisibleToolbarSlot& left,
           const VisibleToolbarSlot& right) {
            if (left.x != right.x) {
                return left.x < right.x;
            }
            return reinterpret_cast<uintptr_t>(left.object) <
                reinterpret_cast<uintptr_t>(right.object);
        });
    constexpr int kToolbarSlotX[7] = {5, 51, 96, 141, 186, 231, 276};
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    for (size_t i = 0; i < slot_count; ++i) {
        if (slots[i].x != kToolbarSlotX[i]) {
            original(slots[i].object, kToolbarSlotX[i], slots[i].y);
        }
    }
}

void RememberToolbarRoot(void* object) {
    if (g_toolbar_root != object) {
        g_toolbar_root = object;
        g_toolbar_last_slot_refresh_tick = 0;
    }
}

void ResizeToolbarBackground(void* object, int width, int height) {
    if (!CanReadGuiObject(object)) {
        return;
    }
    auto* bytes = static_cast<unsigned char*>(object);
    const int current_width = GuiField<int>(bytes, GuiObjectField::width);
    const int current_height = GuiField<int>(bytes, GuiObjectField::height);
    if (current_width == width && current_height == height) {
        return;
    }
    if (g_gui_resize) {
        // Stardom3's native SetSize updates the fields and invokes the Plane
        // geometry rebuild virtual. Directly writing width changes hit tests
        // but leaves the seven-slot background mesh unchanged.
        g_gui_resize(object, width, height);
    } else {
        GuiField<int>(bytes, GuiObjectField::width) = width;
        GuiField<int>(bytes, GuiObjectField::height) = height;
    }
}

void RefreshToolbarSlots(ULONGLONG now) {
    // Keep this retained seven-child check rate-limited in case another
    // immediate layout refresh happens during the same frame.
    constexpr ULONGLONG kToolbarSlotRefreshIntervalMs = 8;
    if (now - g_toolbar_last_slot_refresh_tick <
            kToolbarSlotRefreshIntervalMs ||
        !CanReadGuiObject(g_toolbar_root)) {
        return;
    }
    g_toolbar_last_slot_refresh_tick = now;

    auto* bytes = static_cast<unsigned char*>(g_toolbar_root);
    void* parent = GuiField<void*>(bytes, GuiObjectField::parent);
    const int width = GuiField<int>(bytes, GuiObjectField::width);
    const int height = GuiField<int>(bytes, GuiObjectField::height);
    if (parent != g_unified_ui.primary_root || height < 40 || height > 50) {
        g_toolbar_root = nullptr;
        return;
    }
    const int visible_slots = CountVisibleToolbarSlots(
        g_toolbar_root, parent, width, height);
    if (visible_slots >= 5 && visible_slots <= 7) {
        constexpr int kToolbarWidths[3] = {230, 275, 320};
        const int toolbar_width = kToolbarWidths[visible_slots - 5];
        g_unified_ui.toolbar_width = toolbar_width;
        ResizeToolbarBackground(g_toolbar_root, toolbar_width, height);
        NormalizeVisibleToolbarSlots(g_toolbar_root, visible_slots);
    }
}

bool GetToolbarBackgroundRenderRect(RECT& rect,
                                    float& texture_width_ratio) {
    // Seven-slot mode uses the complete authored background and never needs
    // UV cropping. Test that retained scalar state before validating the GUI
    // object because this function is reached from every candidate draw.
    if (g_unified_ui.toolbar_width < 225 ||
        g_unified_ui.toolbar_width >= 310) {
        return false;
    }
    if (!CanReadGuiObject(g_toolbar_root)) {
        return false;
    }

    auto* bytes = static_cast<unsigned char*>(g_toolbar_root);
    const int x = GuiField<int>(bytes, GuiObjectField::x);
    const int y = GuiField<int>(bytes, GuiObjectField::y);
    const int width = GuiField<int>(bytes, GuiObjectField::width);
    const int height = GuiField<int>(bytes, GuiObjectField::height);
    if (GuiField<void*>(bytes, GuiObjectField::parent) !=
            g_unified_ui.primary_root ||
        *(bytes + 0x99) == 0 || width != g_unified_ui.toolbar_width ||
        height < 40 || height > 50 ||
        x != static_cast<int>(g_unified_ui.width) - width ||
        y != static_cast<int>(g_unified_ui.height) - height) {
        return false;
    }

    rect = RECT{x, y, x + width, y + height};
    texture_width_ratio = static_cast<float>(width) / 320.0f;
    return true;
}

bool IsFiveSlotToolbar(void* object, void* parent,
                       int width, int height) {
    return CountVisibleToolbarSlots(object, parent, width, height) == 5;
}

bool IsSevenSlotToolbar(void* object, void* parent,
                        int width, int height) {
    return CountVisibleToolbarSlots(object, parent, width, height) == 7;
}

bool IsSixSlotToolbar(void* object, void* parent,
                      int width, int height) {
    return CountVisibleToolbarSlots(object, parent, width, height) == 6;
}

bool IsToolbarAnimationObject(void* parent, int width, int height,
                              int x, int y) {
    (void)x;
    (void)y;
    if (parent != g_unified_ui.primary_root) {
        return false;
    }
    const bool toolbar = ((width >= 310 && width <= 330) ||
                          (width >= 270 && width <= 280) ||
                          (width >= 225 && width <= 235)) &&
        height >= 40 && height <= 50;
    const bool toggle = width >= 25 && width <= 32 &&
        height >= 28 && height <= 35;
    return toolbar || toggle;
}

void TransformToolbarAnimationPosition(int width, int height, int& x, int& y,
                                       bool authored_endpoint) {
    (void)width;
    (void)height;
    const int expansion_x =
        static_cast<int>(g_unified_ui.width) - LegacyCanvas::width;
    const int expansion_y =
        static_cast<int>(g_unified_ui.height) - LegacyCanvas::height;

    // Endpoint calls are emitted by the game's authored 800x600 controller;
    // intermediate calls feed the already transformed position back through
    // GuiMove and are reconstructed by TransformToolbarSequenceFrame. The
    // call path, not an arbitrary x threshold, identifies the coordinate
    // space at every supported output resolution.
    if (authored_endpoint) {
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
    const bool wrapper_endpoint = reinterpret_cast<uintptr_t>(call_site) ==
        executable + 0xD7125;
    const bool authored_endpoint = open_endpoint || wrapper_endpoint;

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
            width, height, x, y, authored_endpoint);
        return false;
    }
    if (!sequence->object) {
        sequence->object = object;
    }

    if (!intermediate_frame) {
        sequence->active = false;
        sequence->last_open = open_endpoint;
        TransformToolbarAnimationPosition(
            width, height, x, y, authored_endpoint);
        return false;
    }
    if (!sequence->active) {
        sequence->active = true;
        sequence->target_width = g_unified_ui.toolbar_width;
        sequence->closing = sequence->last_open;
        const bool toolbar = (width >= 225 && width <= 330) &&
            height >= 40 && height <= 50;
        sequence->source_start_x = x;
    }

    const bool toolbar = (width >= 225 && width <= 330) &&
        height >= 40 && height <= 50;
    const int active_width = sequence->target_width;
    const int source_end_x = toolbar ?
        LegacyCanvas::width - active_width :
        LegacyCanvas::width - active_width - 28;
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

bool ReflowAspectFitPageRules(const StaticPageReflowRule* rules, size_t rule_count,
                             void* child, void* parent, int width, int height) {
    bool discovered = false;
    const auto* rule = ReflowStaticPage(rules, rule_count, child, parent,
        width, height, RememberLayoutRoot, RememberProcessedLayoutObject,
        discovered);
    if (rule && discovered) {
        const RectI viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height));
        Log("Unified UI %s aspect-fit self=%p 800x600 -> %d,%d %dx%d",
            rule->name, child, viewport.x, viewport.y,
            viewport.width, viewport.height);
    }
    return rule != nullptr;
}

bool ReflowStaticAspectFitPage(void* child, void* parent, int width, int height) {
    if (child == g_big_activity_option_root || IsBigActivityOptionPanel(
            child, parent, static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height))) {
        ScaleBigActivityOption(child);
        return true;
    }
    // Detached item recipient popup is positioned by its controller, not by
    // generic legacy edge anchoring or the 800x600 page-root transformer.
    if (child == g_inventory_target_root) {
        RememberProcessedLayoutObject(child);
        return true;
    }
    const StaticPageReflowRule rules[] = {
        {g_title_tutorial_root, IsTitleTutorialPage, ResetTitleTutorialGeometry,
         ScaleTitleTutorialSubtree, "title tutorial"},
        {g_title_tutorial_question_root, IsTitleTutorialQuestionPage,
         ResetTitleTutorialQuestionGeometry, ScaleTitleTutorialQuestionSubtree,
         "title tutorial question"},
        {g_artist_profile_root, IsArtistProfilePage, ResetArtistProfileGeometry,
         ScaleArtistProfileSubtree, "artist profile"},
        {g_artist_contract_root, IsArtistContractPage, ResetArtistContractGeometry,
         ScaleArtistContractSubtree, "artist contract"},
        {g_artist_signing_root, IsArtistSigningPage, ResetArtistSigningGeometry,
         ScaleArtistSigningSubtree, "artist signing"},
        {g_inventory_root, IsInventoryPage, ResetInventoryGeometry,
         ScaleInventorySubtree, "inventory", PageRefreshPolicy::PreserveControllerState},
        {g_save_load_root, IsSaveLoadPage, ResetSaveLoadGeometry,
         ScaleSaveLoadSubtree, "save/load", PageRefreshPolicy::PreserveControllerState},
        {g_big_activity_root, IsBigActivityPage, ResetBigActivityGeometry,
         ScaleBigActivitySubtree, "large activity", PageRefreshPolicy::PreserveControllerState},
    };
    return ReflowAspectFitPageRules(rules, std::size(rules),
                                    child, parent, width, height);
}

bool ReflowEndGameSummaryPage(void* child, void* parent, int width, int height) {
    // Credit planes own their scrolling state after the initial subtree fit.
    const StaticPageReflowRule rules[] = {
        {g_end_game_summary_root, IsEndGameSummaryPage, ResetEndGameSummaryGeometry,
         ScaleEndGameSummarySubtree, "end-game summary",
         PageRefreshPolicy::PreserveControllerState},
    };
    return ReflowAspectFitPageRules(rules, std::size(rules),
                                    child, parent, width, height);
}

bool ReflowControllerOwnedPages(void* child, void* parent, int width, int height) {
    // Retain this group's position after company pages and before training.
    // Replaying cached geometry would reset the airplane, EventList's dynamic
    // "new" unit slot, and EventUnit's controller-managed edit selections.
    const StaticPageReflowRule rules[] = {
        {g_airport_selection_root, IsAirportSelectionPage, ResetAirportSelectionGeometry,
         ScaleAirportSelectionSubtree, "airport selection",
         PageRefreshPolicy::PreserveControllerState},
        {g_studio_event_list_root, IsStudioEventListPage, ResetStudioEventListGeometry,
         ScaleStudioEventListSubtree, "studio event list",
         PageRefreshPolicy::PreserveControllerState},
        {g_studio_event_editor_root, IsStudioEventEditorPage, ResetStudioEventEditorGeometry,
         ScaleStudioEventEditorSubtree, "studio event editor",
         PageRefreshPolicy::PreserveControllerState},
    };
    return ReflowAspectFitPageRules(rules, std::size(rules),
                                    child, parent, width, height);
}

struct PageReflowContext {
    void* child;
    void* root;
    int& width;
    int& height;
};

bool ReflowEndGameDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (ReflowEndGameSummaryPage(child, root, width, height)) {
        // Discovery fits once; refresh preserves live credit scrolling.
        return true;
    }
    return false;
}

bool ReflowTitleDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_unified_ui.title_screen_root &&
               IsTitleScreenVisible()) {
        // The attract animation can restore the root rectangle or replace
        // individual visual children after the first layout pass. Reapply
        // the stored native geometry idempotently once per refresh.
        ScaleTitleScreenSubtree(child);
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsTitleScreenRoot(child, root, width, height)) {
        if (g_unified_ui.title_screen_root != child) {
            ResetTitleNativeGeometry(child);
        }
        g_unified_ui.title_screen_root = child;
        RememberLayoutRoot(child);
        ScaleTitleScreenSubtree(child);
        RememberProcessedLayoutObject(child);
        if (!g_unified_ui.title_screen_logged) {
            const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
                AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                      static_cast<int>(g_unified_ui.height));
            Log("Unified UI title screen aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                child, viewport_x, viewport_y,
                viewport_width, viewport_height);
            g_unified_ui.title_screen_logged = true;
        }
        return true;
    }
    return false;
}

bool ReflowStaticDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (ReflowStaticAspectFitPage(child, root, width, height)) {
        // The shared dispatcher has scaled and marked this static page.
        return true;
    }
    return false;
}

bool ReflowCompanyNavigationDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_company_navigation_root) {
        // The controller moves the selected section icon into the title
        // slot. Do not replay the discovery snapshot during maintenance.
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsCompanyNavigationOverlay(child, root, width, height)) {
        ResetCompanyNavigationGeometry(child);
        RememberLayoutRoot(child);
        ScaleCompanyNavigationSubtree(child);
        RememberProcessedLayoutObject(child);
        return true;
    }
    return false;
}

bool ReflowCompanyRevenueDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_company_revenue_root) {
        // Revenue bars are controller-sized after the report is loaded.
        // Do not replay the resource template during maintenance or their
        // live heights would be replaced with the cached startup values.
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsCompanyRevenuePage(child, root, width, height)) {
        const bool chart_ready = IsCompanyRevenueChartReady(child);
        ResetCompanyRevenueGeometry(child);
        RememberLayoutRoot(child);
        ScaleCompanyRevenueSubtree(child);
        // Scale the page immediately. The controller has not populated
        // these ten placeholder bars yet, so leave just those leaves in
        // native coordinates until their first real values arrive.
        if (!chart_ready) {
            RestoreCompanyRevenueChartToNative();
        }
        RememberProcessedLayoutObject(child);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        Log("Unified UI company revenue aspect-fit self=%p 800x600 -> %d,%d %dx%d",
            child, viewport_x, viewport_y,
            viewport_width, viewport_height);
        return true;
    }
    return false;
}

bool ReflowCompanySectionsDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (CompanySectionLayout* company_section =
                   FindCompanySectionLayout(child)) {
        // Controllers own live content on these cached pages. They are
        // already fitted; HookGuiMove transforms later mutations.
        RememberProcessedLayoutObject(company_section->root);
        return true;
    } else if (IsCompanySectionPage(child, root, width, height)) {
        if (CompanySectionLayout* company_section =
                RegisterCompanySectionLayout(child)) {
            RememberLayoutRoot(child);
            ScaleCompanySectionSubtree(*company_section, child);
            RememberProcessedLayoutObject(child);
        }
        return true;
    }
    return false;
}

bool ReflowControllerOwnedDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (ReflowControllerOwnedPages(child, root, width, height)) {
        // No subtree replay during controller-owned page maintenance.
        return true;
    }
    return false;
}

bool ReflowTrainingFrameDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_training_minigame_root) {
        // The progress control interpolates its marker internally. Do not
        // replay Move across the complete subtree during the one-second
        // maintenance pass: doing so interrupts that interpolation and
        // can make the marker jump to the midpoint for one frame. Initial
        // discovery performs the full fit; HookGuiMove maintains later
        // game-driven updates.
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsTrainingMinigamePanel(child, root, width, height)) {
        ResetTrainingMinigameGeometry(child);
        ScaleTrainingMinigameSubtree(child);
        RememberProcessedLayoutObject(child);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        Log("Unified UI training minigame aspect-fit self=%p 426x369 -> %d,%d %dx%d",
            child,
            viewport_x + MulDiv(187, viewport_width, LegacyCanvas::width),
            viewport_y + MulDiv(126, viewport_height, LegacyCanvas::height),
            MulDiv(426, viewport_width, LegacyCanvas::width),
            MulDiv(369, viewport_height, LegacyCanvas::height));
        return true;
    }
    return false;
}

bool ReflowTrainingActivitiesDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (IsTrainingActivityRoot(child)) {
        // Activity pages are fully transformed when first discovered.
        // Rewalking them here would also restart animations owned by
        // their custom controls.
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsTrainingActivityPage(child, root, width, height)) {
        ScaleTrainingActivitySubtree(child);
        RememberProcessedLayoutObject(child);
        Log("Unified UI training activity page aspect-fit self=%p",
            child);
        return true;
    }
    return false;
}

bool ReflowAnnouncementDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_announcement_root &&
               IsCachedAnnouncementRootValid()) {
        ScaleAnnouncementSubtree(child);
        RememberProcessedLayoutObject(child);
        return true;
    } else if (IsAnnouncementControlRoot(child, root, width, height)) {
        ResetAnnouncementGeometry(child);
        RememberLayoutRoot(child);
        ScaleAnnouncementSubtree(child);
        RememberProcessedLayoutObject(child);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        Log("Unified UI announcement aspect-fit self=%p 800x600 -> %d,%d %dx%d",
            child, viewport_x, viewport_y,
            viewport_width, viewport_height);
        return true;
    }
    return false;
}

bool ReflowPhotoAlbumDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (IsPhotoAlbumRoot(child, root, width, height)) {
        g_unified_ui.photo_album_root = child;
        RememberLayoutRoot(child);
        ScalePhotoAlbumSubtree(child);
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        Log("Unified UI photo album aspect-fit self=%p 800x600 -> %d,%d %dx%d",
            child, viewport_x, viewport_y,
            viewport_width, viewport_height);
        return true;
    }
    return false;
}

bool ReflowAwardsDispatch(PageReflowContext& context) {
    auto& [child, root, width, height] = context;
    if (child == g_awards_ceremony_root) {
        LayoutAwardsCeremonyOverlayRoot(child);
        return true;
    } else if (IsAwardsCeremonyOverlayRoot(
                   child, root, width, height)) {
        g_awards_ceremony_root = child;
        RememberLayoutRoot(child);
        LayoutAwardsCeremonyOverlayRoot(child);
        LogAwardsCeremonyLayoutOnce(child);
        return true;
    }
    return false;
}

bool DispatchAspectFitPageReflow(void* child, void* root, int& width, int& height) {
    using Handler = bool (*)(PageReflowContext&);
    static constexpr Handler handlers[] = {
        ReflowEndGameDispatch,
        ReflowTitleDispatch,
        ReflowStaticDispatch,
        ReflowCompanyNavigationDispatch,
        ReflowCompanyRevenueDispatch,
        ReflowCompanySectionsDispatch,
        ReflowControllerOwnedDispatch,
        ReflowTrainingFrameDispatch,
        ReflowTrainingActivitiesDispatch,
        ReflowAnnouncementDispatch,
        ReflowPhotoAlbumDispatch,
        ReflowAwardsDispatch,
    };
    PageReflowContext context{child, root, width, height};
    return DispatchFirstHandled(handlers, context);
}

void ReflowExistingRootChildren(void* root, int depth = 0) {
    GuiObjectReadBatch read_batch;
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
    void* child = GuiField<void*>(root_bytes, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < 4096) {
        auto* child_bytes = static_cast<unsigned char*>(child);
        void* next = GuiField<void*>(child_bytes, GuiObjectField::next_sibling);
        int& x = GuiField<int>(child_bytes, GuiObjectField::x);
        int& y = GuiField<int>(child_bytes, GuiObjectField::y);
        int& width = GuiField<int>(child_bytes, GuiObjectField::width);
        int& height = GuiField<int>(child_bytes, GuiObjectField::height);
        if (IsLoadingPageRoot(child, root, width, height) &&
            g_loading_page_root != child) {
            g_loading_page_root = child;
            Log("Unified UI loading page discovered self=%p rect=%d,%d %dx%d",
                child, x, y, width, height);
        }
        const bool legacy_canvas = width >= 790 && width <= 810 &&
            height >= 590 && height <= 610;
        const bool known_canvas = IsKnownLayoutRoot(child);
        if (child == g_unified_ui.in_game_cg_root && IsInGameCGVisible()) {
            LayoutInGameCGRoot(child);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_logged) {
                const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
                    AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                          static_cast<int>(g_unified_ui.height));
                Log("Unified UI in-game CG aspect-fit self=%p 800x600 -> %d,%d %dx%d",
                    child, viewport_x, viewport_y,
                    viewport_width, viewport_height);
                g_unified_ui.in_game_cg_logged = true;
            }
        } else if (IsInGameCGCaption(child, root, width, height)) {
            const bool new_caption =
                child != g_unified_ui.in_game_cg_caption;
            g_unified_ui.in_game_cg_caption = child;
            if (new_caption) {
                g_unified_ui.in_game_cg_caption_native_y = y;
                g_unified_ui.in_game_cg_caption_logged = false;
            }
            int new_x = 0;
            int new_y = 0;
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_caption_logged) {
                Log("Unified UI native caption/status bar self=%p y=%d -> %d,%d",
                    child, g_unified_ui.in_game_cg_caption_native_y,
                    new_x, new_y);
                g_unified_ui.in_game_cg_caption_logged = true;
            }
        } else if (IsInGameCGItemNotice(
                       child, root, width, height)) {
            const bool new_notice =
                child != g_unified_ui.in_game_cg_item_notice;
            g_unified_ui.in_game_cg_item_notice = child;
            if (new_notice) {
                g_unified_ui.in_game_cg_item_notice_native_x = x;
                g_unified_ui.in_game_cg_item_notice_native_y = y;
                g_unified_ui.in_game_cg_item_notice_logged = false;
            }
            int new_x = 0;
            int new_y = 0;
            GetInGameCGNativeOverlayPosition(
                g_unified_ui.in_game_cg_item_notice_native_x,
                g_unified_ui.in_game_cg_item_notice_native_y,
                new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
            if (!g_unified_ui.in_game_cg_item_notice_logged) {
                Log("Unified UI native status notice self=%p source=%d,%d -> %d,%d",
                    child,
                    g_unified_ui.in_game_cg_item_notice_native_x,
                    g_unified_ui.in_game_cg_item_notice_native_y,
                    new_x, new_y);
                g_unified_ui.in_game_cg_item_notice_logged = true;
            }
        } else if (DispatchAspectFitPageReflow(child, root, width, height)) {
            // The first matching page handler owns discovery/maintenance.
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
        } else if (void* phone_button = FindPhoneOverlayButton(
                       child, root, width, height)) {
            RememberPhoneOverlay(child, phone_button);
            RememberLayoutRoot(child);
            ReflowExistingRootChildren(child, depth + 1);
        } else if (IsSevenSlotToolbar(child, root, width, height)) {
            if (g_unified_ui.toolbar_width != 320) {
                Log("Unified UI toolbar mode=7 width=320");
            }
            RememberToolbarRoot(child);
            g_unified_ui.toolbar_width = 320;
            NormalizeVisibleToolbarSlots(child, 7);
            ResizeToolbarBackground(child, 320, height);
            original(child,
                static_cast<int>(g_unified_ui.width) - 320,
                static_cast<int>(g_unified_ui.height) - height);
            RememberProcessedLayoutObject(child);
        } else if (IsSixSlotToolbar(child, root, width, height)) {
            if (g_unified_ui.toolbar_width != 275) {
                Log("Unified UI toolbar mode=6 width=275");
            }
            RememberToolbarRoot(child);
            g_unified_ui.toolbar_width = 275;
            NormalizeVisibleToolbarSlots(child, 6);
            ResizeToolbarBackground(child, 275, height);
            original(child,
                static_cast<int>(g_unified_ui.width) - 275,
                static_cast<int>(g_unified_ui.height) - height);
            RememberProcessedLayoutObject(child);
        } else if (IsFiveSlotToolbar(child, root, width, height)) {
            if (g_unified_ui.toolbar_width != 230) {
                Log("Unified UI toolbar mode=5 width=230");
            }
            RememberToolbarRoot(child);
            g_unified_ui.toolbar_width = 230;
            NormalizeVisibleToolbarSlots(child, 5);
            ResizeToolbarBackground(child, 230, height);
            original(child,
                static_cast<int>(g_unified_ui.width) - 230,
                static_cast<int>(g_unified_ui.height) - height);
            RememberProcessedLayoutObject(child);
        } else if (IsToolbarAnimationObject(root, width, height, x, y)) {
            if (!IsProcessedLayoutObject(child)) {
                int new_x = x;
                int new_y = y;
                TransformToolbarAnimationPosition(
                    width, height, new_x, new_y, true);
                original(child, new_x, new_y);
                RememberProcessedLayoutObject(child);
            }
        } else if (legacy_canvas) {
            size_t child_count = 0;
            void* only_child = GuiField<void*>(child_bytes, GuiObjectField::first_child);
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
            const int only_x = GuiField<int>(only_bytes, GuiObjectField::x);
            const int only_y = GuiField<int>(only_bytes, GuiObjectField::y);
            const int only_width = GuiField<int>(only_bytes, GuiObjectField::width);
            const int only_height = GuiField<int>(only_bytes, GuiObjectField::height);
            const bool small_single_overlay = child_count == 1 &&
                only_width >= 0 && only_width < 400 &&
                only_height >= 0 && only_height < 400;

            if (!small_single_overlay) {
                RememberGroupCanvas(child);
                RememberProcessedLayoutObject(child);
                const int page_x = (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
                const int page_y = (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
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
                const bool central_overlay = center_x_twice >= (LegacyCanvas::width * 2) / 3 &&
                    center_x_twice <= (LegacyCanvas::width * 4) / 3 &&
                    center_y_twice >= (LegacyCanvas::height * 2) / 3 &&
                    center_y_twice <= (LegacyCanvas::height * 4) / 3;
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
                   IsTeamFriendshipTag(child, root, width, height) ||
                   IsWorldDialogueChoicePanel(child, root, width, height) ||
                   IsWorldProjectedObject(child)) {
            // Preserve the game's own projected coordinates.  In particular,
            // do not turn the legacy x=340 interaction canvas into x=1460.
            if (!IsProcessedLayoutObject(child)) {
                RememberWorldProjectedObject(child);
                RememberProcessedLayoutObject(child);
                Log("Unified UI world interaction passthrough self=%p parent=%p rect=%d,%d %dx%d",
                    child, root, x, y, width, height);
            }
        } else if (IsEventPublicationCover(child, root, width, height)) {
            g_publication_cover = child;
            if (!IsProcessedLayoutObject(child)) {
                // Match the established 510x350 newspaper-panel path: shift
                // the controller's current origin once, then let its later
                // absolute frames run in that translated coordinate space.
                const int new_x = x +
                    (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
                const int new_y = y +
                    (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
                original(child, new_x, new_y);
                RememberProcessedLayoutObject(child);
                Log("Unified UI publication cover origin shifted self=%p rect=%d,%d %dx%d -> %d,%d",
                    child, x, y, width, height, new_x, new_y);
            }
        } else if (IsEventPublicationPanel(child, root, width, height)) {
            if (!IsProcessedLayoutObject(child)) {
                const int new_x = x +
                    (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
                const int new_y = y +
                    (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
                original(child, new_x, new_y);
                RememberProcessedLayoutObject(child);
            }
        } else if (!IsProcessedLayoutObject(child) &&
                   IsScheduleJobHoverTooltip(child, root, width, height)) {
            int new_x = x;
            int new_y = y;
            TransformScheduleJobHoverTooltip(child, new_x, new_y);
            original(child, new_x, new_y);
            RememberProcessedLayoutObject(child);
        } else if (IsScheduleSecondaryPanel(child, root, width, height)) {
            int new_x = x;
            int new_y = y;
            TransformCenteredScheduleOverlay(
                child, root, width, height, new_x, new_y);
            if (new_x != x || new_y != y) {
                original(child, new_x, new_y);
            }
            RememberProcessedLayoutObject(child);
        } else if (TransformCenteredPageOverlay(
                       child, root, width, height, x, y)) {
            original(child, x, y);
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
        } else if (StudioEventDropdownLayout* layout =
                       FindStudioEventDropdownLayout(child);
                   layout && layout->tutorial_mode) {
            // The GUI pool retains editor dropdown addresses. Once an object
            // is reused by character setup, keep its restored native geometry
            // out of editor discovery and maintenance scaling.
            RememberProcessedLayoutObject(child);
        } else if (IsStudioEventEditorDropdown(
                       child, root, width, height)) {
            int native_x = x;
            int native_y = y;
            const int center_x =
                (static_cast<int>(g_unified_ui.width) -
                 LegacyCanvas::width) / 2;
            const int center_y =
                (static_cast<int>(g_unified_ui.height) -
                 LegacyCanvas::height) / 2;
            if (IsProcessedLayoutObject(child) &&
                x >= center_x && y >= center_y) {
                // The generic legacy path may have centered the transient
                // popup before all seven children existed. Recover the game-
                // supplied 800x600 coordinate before applying aspect fit.
                native_x -= center_x;
                native_y -= center_y;
            }
            StudioEventDropdownLayout* layout =
                RegisterStudioEventDropdownLayout(child, width, height);
            if (layout) {
                ScaleStudioEventDropdownSubtree(
                    child, *layout, native_x, native_y);
            }
            Log("Unified UI studio event dropdown aspect-fit self=%p rect=%d,%d %dx%d",
                child, native_x, native_y, width, height);
        } else if (!HasTitleTutorialPage() &&
                   FindStudioEventDropdownLayout(child)) {
            // The popup is transient but remains controller-positioned while
            // open. HookGuiMove handles new coordinates without replaying its
            // initial selection state during maintenance.
            RememberProcessedLayoutObject(child);
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
                   x >= 0 && x <= LegacyCanvas::width && y >= 0 && y <= LegacyCanvas::height) {
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
    GuiObjectReadBatch read_batch;
    const ULONGLONG now = GetTickCount64();
    RefreshToolbarSlots(now);
    DiscoverPhoneOverlay(now);
    if (IsDebugModeEnabled()) {
        LogPhoneOverlayStateIfChanged();
    }
    RefreshPhoneOverlayButton("begin-scene");
    RefreshPhoneOverlayDialoguePanels("begin-scene");
    UpdateTitleStripMotion(now);
    CorrectArtistRadarVertices();
    DiscoverVisibleCompanyNavigation();
    RefreshInventoryLayout();
    RefreshCompanyRevenueBars();
    // Closing an already discovered announcement is a direct visibility-byte
    // change and may not produce GuiMove activity. Retire the active render
    // path immediately using the retained root instead of waiting for the
    // rare full-layout integrity pass.
    if (g_unified_ui.announcement_active &&
        (!CanReadGuiObject(g_announcement_root) ||
         *(static_cast<unsigned char*>(g_announcement_root) + 0x99) == 0)) {
        g_unified_ui.announcement_active = false;
        Log("Unified UI announcement render layer active=0");
    }
    // Before the title root is known, scan on the first draw submissions so
    // the page is aspect-fitted before its first visible frame. Once found,
    // return to the low-frequency maintenance pass used by ordinary screens.
    const bool title_discovery_pending =
        g_unified_ui.title_screen_mode != 0 &&
        g_unified_ui.title_screen_root == nullptr;
    const bool layout_activity = g_unified_ui.processed_count !=
        g_unified_ui.last_refresh_processed_count;
    // GuiMove registers new objects as they are constructed, so topology
    // changes still trigger maintenance on the next frame. Once the layout is
    // stable, retain only a rare integrity pass. Special controls that can
    // change visibility without GuiMove use their own incremental discovery
    // cycles and no longer depend on this full-tree fallback.
    constexpr ULONGLONG kStableLayoutRefreshIntervalMs = 30000;
    if (!title_discovery_pending && !layout_activity &&
        now - g_unified_ui.last_refresh_tick <
            kStableLayoutRefreshIntervalMs) {
        return;
    }
    g_unified_ui.last_refresh_tick = now;
    ReflowExistingRootChildren(g_unified_ui.primary_root);
    g_unified_ui.last_refresh_processed_count =
        g_unified_ui.processed_count;

    const bool announcement_active = IsCachedAnnouncementRootValid() &&
        *(static_cast<unsigned char*>(g_announcement_root) + 0x99) != 0;
    if (announcement_active != g_unified_ui.announcement_active) {
        g_unified_ui.announcement_active = announcement_active;
        Log("Unified UI announcement render layer active=%d",
            announcement_active ? 1 : 0);
    }
}

void RefreshUnifiedUILayoutNow() {
    if (!g_unified_ui.installed || !g_unified_ui.primary_root) {
        return;
    }
    g_unified_ui.last_refresh_tick = 0;
    RefreshUnifiedUILayout();
}

void RefreshInGameCGOverlays(bool discover_surfaces) {
    if (!g_unified_ui.installed ||
        !CanReadGuiObject(g_unified_ui.primary_root) ||
        !g_unified_ui.trampoline) {
        return;
    }
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    const auto refresh_caption = [&]() {
        if (!CanReadGuiObject(g_unified_ui.in_game_cg_caption)) {
            return;
        }
        auto* caption_bytes = static_cast<unsigned char*>(
            g_unified_ui.in_game_cg_caption);
        void* parent = GuiField<void*>(caption_bytes, GuiObjectField::parent);
        const int width = GuiField<int>(caption_bytes, GuiObjectField::width);
        const int height = GuiField<int>(caption_bytes, GuiObjectField::height);
        if (*(caption_bytes + 0x99) == 0 ||
            !IsInGameCGCaption(g_unified_ui.in_game_cg_caption,
                parent, width, height)) {
            return;
        }
        int x = 0;
        int y = 0;
        GetInGameCGCaptionPosition(
            g_unified_ui.in_game_cg_caption_native_y, x, y);
        original(g_unified_ui.in_game_cg_caption, x, y);
    };
    const auto refresh_status_notice = [&]() {
        if (!CanReadGuiObject(g_unified_ui.in_game_cg_item_notice)) {
            return;
        }
        auto* notice_bytes = static_cast<unsigned char*>(
            g_unified_ui.in_game_cg_item_notice);
        void* parent = GuiField<void*>(notice_bytes, GuiObjectField::parent);
        const int width = GuiField<int>(notice_bytes, GuiObjectField::width);
        const int height = GuiField<int>(notice_bytes, GuiObjectField::height);
        if (*(notice_bytes + 0x99) == 0 ||
            !IsInGameCGItemNotice(g_unified_ui.in_game_cg_item_notice,
                parent, width, height)) {
            return;
        }
        int x = 0;
        int y = 0;
        GetInGameCGNativeOverlayPosition(
            g_unified_ui.in_game_cg_item_notice_native_x,
            g_unified_ui.in_game_cg_item_notice_native_y, x, y);
        original(g_unified_ui.in_game_cg_item_notice, x, y);
    };

    // Once retained, correct the notice immediately at every UI submission.
    // The game writes its authored x=0 directly after BeginScene, bypassing
    // HookGuiMove. This cheap one-object pass therefore runs both at
    // BeginScene and before the first draw. Only BeginScene advances the
    // budgeted discovery cursor below.
    refresh_caption();
    refresh_status_notice();
    if (!discover_surfaces) {
        return;
    }
    if (g_unified_ui.in_game_cg_discovery_root !=
            g_unified_ui.primary_root) {
        g_unified_ui.in_game_cg_discovery_root =
            g_unified_ui.primary_root;
        g_unified_ui.in_game_cg_discovery_cursor = nullptr;
        g_unified_ui.in_game_cg_discovery_active = false;
    }
    const ULONGLONG now = GetTickCount64();
    const bool layout_activity = g_unified_ui.processed_count !=
        g_unified_ui.in_game_cg_discovery_processed_count;
    // Visibility can change without a GuiMove callback. Start immediately on
    // construction activity and otherwise leave a short pause between full
    // incremental cycles so the fallback remains responsive but bounded.
    constexpr ULONGLONG kInGameCGDiscoveryCycleIntervalMs = 100;
    if (!g_unified_ui.in_game_cg_discovery_active &&
        (layout_activity ||
         g_unified_ui.last_in_game_cg_discovery_cycle_tick == 0 ||
         now - g_unified_ui.last_in_game_cg_discovery_cycle_tick >=
             kInGameCGDiscoveryCycleIntervalMs)) {
        g_unified_ui.in_game_cg_discovery_active = true;
        g_unified_ui.in_game_cg_discovery_cursor = nullptr;
        g_unified_ui.in_game_cg_discovery_processed_count =
            g_unified_ui.processed_count;
    }
    if (g_unified_ui.in_game_cg_discovery_active) {
        // 512 nodes keeps the worst-case 4096-node root list within eight
        // frames while spreading the former one-frame spike.
        constexpr size_t kInGameCGDiscoveryNodesPerFrame = 512;
        g_unified_ui.in_game_cg_discovery_cursor = DiscoverInGameCGSurfaces(
            g_unified_ui.primary_root,
            g_unified_ui.in_game_cg_discovery_cursor,
            kInGameCGDiscoveryNodesPerFrame);
        if (!g_unified_ui.in_game_cg_discovery_cursor) {
            g_unified_ui.in_game_cg_discovery_active = false;
            g_unified_ui.last_in_game_cg_discovery_cycle_tick = now;
        }
    }
    // Discovery may have retained the notice for the first time in this pass.
    refresh_caption();
    refresh_status_notice();
    if (!IsInGameCGVisible()) {
        return;
    }

    LayoutInGameCGRoot(g_unified_ui.in_game_cg_root);
    for (auto& overlay : g_unified_ui.in_game_cg_native_overlays) {
        if (!CanReadGuiObject(overlay.object) ||
            overlay.object == g_unified_ui.in_game_cg_caption ||
            overlay.object == g_unified_ui.in_game_cg_item_notice) {
            continue;
        }
        auto* overlay_bytes = static_cast<unsigned char*>(overlay.object);
        void* parent = GuiField<void*>(overlay_bytes, GuiObjectField::parent);
        const int width = GuiField<int>(overlay_bytes, GuiObjectField::width);
        const int height = GuiField<int>(overlay_bytes, GuiObjectField::height);
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

struct PageMoveContext {
    void* self;
    void* parent;
    int& x;
    int& y;
    GuiMoveFn original;
    HMODULE executable;
    void* immediate_call;
};

bool MoveEndGamePage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_end_game_summary_root) {
        const RectI viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height));
        GuiField<int>(self, GuiObjectField::width) = viewport.width;
        GuiField<int>(self, GuiObjectField::height) = viewport.height;
        original(self, viewport.x, viewport.y);
        return true;
    }
    if (parent && IsEndGameSummaryDescendant(self)) {
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const RectI viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_end_game_summary_geometry,
            std::size(g_end_game_summary_geometry),
            g_end_game_summary_geometry_count, self);
        if (native) {
            GuiField<int>(self, GuiObjectField::width) = MulDiv(
                native->width, viewport.width, LegacyCanvas::width);
            GuiField<int>(self, GuiObjectField::height) = MulDiv(
                native->height, viewport.height, LegacyCanvas::height);
            if (EndGameCreditMotionState* motion =
                    RememberEndGameCreditMotion(self, native)) {
                const int current_x =
                    GuiField<int>(self, GuiObjectField::x);
                const int current_y =
                    GuiField<int>(self, GuiObjectField::y);
                motion->native_x = ResolveNativeAnimationCoordinate(
                    x, current_x, motion->native_x);
                motion->native_y = ResolveNativeAnimationCoordinate(
                    y, current_y, motion->native_y);
                original(self,
                    MulDiv(motion->native_x, viewport.width,
                           LegacyCanvas::width),
                    MulDiv(motion->native_y, viewport.height,
                           LegacyCanvas::height));
                return true;
            }
            original(self,
                MulDiv(x, viewport.width, LegacyCanvas::width),
                MulDiv(y, viewport.height, LegacyCanvas::height));
            return true;
        }
    }
    return false;
}

bool MoveTitlePage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_unified_ui.title_screen_root) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        original(self, viewport_x, viewport_y);
        return true;
    }
    if (parent && IsTitleScreenDescendant(self)) {
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberTitleNativeGeometry(self);
        if (native) {
            GuiField<int>(bytes, GuiObjectField::width) =
                MulDiv(native->width, viewport_width, LegacyCanvas::width);
            GuiField<int>(bytes, GuiObjectField::height) =
                MulDiv(native->height, viewport_height, LegacyCanvas::height);
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
            const int current_x = GuiField<int>(bytes, GuiObjectField::x);
            const bool lead_strip =
                native->x >= -1000 && native->x <= 1000;
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
            x = MulDiv(native_x, viewport_width, LegacyCanvas::width);
            y = MulDiv(native->y, viewport_height, LegacyCanvas::height);
            original(self, x, y);
            return true;
        }
        x = MulDiv(x, viewport_width, LegacyCanvas::width);
        y = MulDiv(y, viewport_height, LegacyCanvas::height);
        original(self, x, y);
        return true;
    }
    return false;
}

bool MoveStaticPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    // Keep these static pages in their original dispatch order. Animated
    // pages (including company navigation) stay in the specialised paths below.
    const StaticPageMoveRule static_pages[] = {
        {g_title_tutorial_root, g_title_tutorial_geometry,
         std::size(g_title_tutorial_geometry), &g_title_tutorial_geometry_count},
        {g_title_tutorial_question_root, g_title_tutorial_question_geometry,
         std::size(g_title_tutorial_question_geometry),
         &g_title_tutorial_question_geometry_count, 6},
        {g_artist_profile_root, g_artist_profile_geometry,
         std::size(g_artist_profile_geometry), &g_artist_profile_geometry_count},
        {g_artist_contract_root, g_artist_contract_geometry,
         std::size(g_artist_contract_geometry), &g_artist_contract_geometry_count},
        {g_artist_signing_root, g_artist_signing_geometry,
         std::size(g_artist_signing_geometry), &g_artist_signing_geometry_count},
    };
    if (ApplyStaticPageMove(static_pages, std::size(static_pages), self, parent,
                            static_cast<int>(g_unified_ui.width),
                            static_cast<int>(g_unified_ui.height), x, y,
                            IsRepeatedObjectPosition)) {
        original(self, x, y);
        return true;
    }
    return false;
}

bool MoveInventoryPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    DiscoverInventory(self);
    if (g_inventory_root && (self == g_inventory_target_root ||
            IsInventoryTargetTree(self, g_unified_ui.primary_root))) {
        if (g_inventory_target_root != self) {
            g_inventory_target_root = self;
            g_inventory_target_geometry_count = 0;
        }
        const auto viewport = AspectFitLegacyCanvas(
            static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height));
        void* card = GuiPointer(g_inventory_root, GuiObjectField::first_child);
        if (!CanReadGuiObject(card)) return false;
        // 0048933D reads PlaneBack's live coordinates. 00489A10 then adds
        // native (11, 81 + row*85), without the inventory root's origin.
        // Scale just those offsets, and add the fitted root origin once.
        if (!IsRepeatedObjectPosition(self, x, y)) {
            FitInventoryTargetPosition(GuiField<int>(card, GuiObjectField::x),
                GuiField<int>(card, GuiObjectField::y), viewport.x, viewport.y,
                viewport.width, viewport.height, x, y);
        }
        GuiField<int>(self, GuiObjectField::width) = MulDiv(370, viewport.width, 800);
        GuiField<int>(self, GuiObjectField::height) = MulDiv(90, viewport.height, 600);
        ScaleAspectFitSubtree(GuiPointer(self, GuiObjectField::first_child), 1,
            g_inventory_target_geometry, g_inventory_target_geometry_count,
            [](void*) {}, 4, 8);
        RememberProcessedLayoutObject(self);
        original(self, x, y);
        return true;
    }
    if (g_inventory_target_root && IsDescendantOf(self, g_inventory_target_root, 4)) {
        const StaticPageMoveRule target_rule{g_inventory_target_root,
            g_inventory_target_geometry, std::size(g_inventory_target_geometry),
            &g_inventory_target_geometry_count, 4};
        if (ApplyStaticPageMove(&target_rule, 1, self, parent,
                static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height),
                x, y, IsRepeatedObjectPosition)) {
            original(self, x, y);
            return true;
        }
    }
    const StaticPageMoveRule rule = {g_inventory_root, g_inventory_geometry,
        std::size(g_inventory_geometry), &g_inventory_geometry_count, 6,
        PageMoveCoordinates::SubmittedNative};
    const int native_x = x, native_y = y;
    const bool repeated = IsRepeatedObjectPosition(self, x, y);
    if (!ApplyStaticPageMove(&rule, 1, self, parent,
            static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height),
            x, y, IsRepeatedObjectPosition)) return false;
    if (!repeated) {
        if (auto* native = FindGeometry(g_inventory_geometry, g_inventory_geometry_count, self)) {
            native->x = native_x;
            native->y = native_y;
        }
    }
    original(self, x, y);
    return true;
}

bool MoveSaveLoadPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    if (!g_save_load_root && IsSaveLoadPageTree(self, g_unified_ui.primary_root)) {
        ResetSaveLoadGeometry(self);
        RememberLayoutRoot(self);
        ScaleSaveLoadSubtree(self);
        return true;
    }
    const StaticPageMoveRule rule{g_save_load_root, g_save_load_geometry,
        std::size(g_save_load_geometry), &g_save_load_geometry_count, 6,
        PageMoveCoordinates::SubmittedNative};
    if (!ApplyStaticPageMove(&rule, 1, self, parent,
            static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height),
            x, y, IsRepeatedObjectPosition)) return false;
    original(self, x, y);
    return true;
}

bool MoveBigActivityPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    if (!g_big_activity_root &&
        IsBigActivityPageTree(self, g_unified_ui.primary_root)) {
        ResetBigActivityGeometry(self);
        RememberLayoutRoot(self);
        ScaleBigActivitySubtree(self);
        return true;
    }
    if (self == g_big_activity_option_root || IsBigActivityOptionPanel(
            self, parent, static_cast<int>(g_unified_ui.width),
            static_cast<int>(g_unified_ui.height))) {
        ScaleBigActivityOption(self);
        return true;
    }
    const StaticPageMoveRule rules[] = {
        {g_big_activity_root, g_big_activity_geometry,
         std::size(g_big_activity_geometry), &g_big_activity_geometry_count, 7,
         PageMoveCoordinates::SubmittedNative},
        {g_big_activity_option_root, g_big_activity_option_geometry,
         std::size(g_big_activity_option_geometry), &g_big_activity_option_geometry_count, 7,
         PageMoveCoordinates::SubmittedNative},
    };
    if (!ApplyStaticPageMove(rules, std::size(rules), self, parent,
            static_cast<int>(g_unified_ui.width), static_cast<int>(g_unified_ui.height),
            x, y, IsRepeatedObjectPosition)) return false;
    original(self, x, y);
    return true;
}

bool MoveCompanyNavigationPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_company_navigation_root) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        original(self, viewport_x, viewport_y);
        return true;
    }
    if (parent && IsCompanyNavigationDescendant(self)) {
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_company_navigation_geometry,
            std::size(g_company_navigation_geometry),
            g_company_navigation_geometry_count, self);
        if (native) {
            // Retain the live native interpolation result, not the resource's
            // initial slot. The next animation must start at this position.
            native->x = x;
            native->y = y;
            GuiField<int>(bytes, GuiObjectField::width) =
                MulDiv(native->width, viewport_width, LegacyCanvas::width);
            GuiField<int>(bytes, GuiObjectField::height) =
                MulDiv(native->height, viewport_height, LegacyCanvas::height);
            original(self,
                MulDiv(x, viewport_width, LegacyCanvas::width),
                MulDiv(y, viewport_height, LegacyCanvas::height));
            return true;
        }
    }
    return false;
}

bool MoveCompanySectionPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (CompanySectionLayout* company_section =
            FindCompanySectionLayout(self)) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        original(self, viewport_x, viewport_y);
        return true;
    }
    if (parent) {
        if (CompanySectionLayout* company_section =
                FindCompanySectionLayoutForObject(self)) {
            if (IsRepeatedObjectPosition(self, x, y)) {
                original(self, x, y);
                return true;
            }
            const auto [viewport_x, viewport_y,
                        viewport_width, viewport_height] =
                AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                      static_cast<int>(g_unified_ui.height));
            TitleNativeGeometry* native =
                RememberDynamicCompanySectionGeometry(
                    *company_section, self, x, y,
                    viewport_width, viewport_height);
            if (native) {
                GuiField<int>(bytes, GuiObjectField::width) =
                    MulDiv(native->width, viewport_width,
                           LegacyCanvas::width);
                GuiField<int>(bytes, GuiObjectField::height) =
                    MulDiv(native->height, viewport_height,
                           LegacyCanvas::height);
                original(self,
                    MulDiv(x, viewport_width, LegacyCanvas::width),
                    MulDiv(y, viewport_height, LegacyCanvas::height));
                return true;
            }
        }
    }
    return false;
}

bool MoveCompanyRevenuePage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_company_revenue_root) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        original(self, viewport_x, viewport_y);
        return true;
    }
    if (parent && IsCompanyRevenueDescendant(self)) {
        const auto* executable_bytes = reinterpret_cast<unsigned char*>(executable);
        const bool bar_update = executable &&
            (immediate_call == executable_bytes + 0x84D3C ||
             immediate_call == executable_bytes + 0x84E0D);
        TitleNativeGeometry* cached = FindGeometry(
            g_company_revenue_geometry,
            g_company_revenue_geometry_count, self);
        if (!g_company_revenue_waiting_for_bars && cached && executable &&
            immediate_call == executable_bytes + 0x84AB7 &&
            IsCompanyRevenueChartColumnGeometry(cached)) {
            // 00484A9F reads the first column's fitted width, although its
            // spacing and origin remain native. Undo only that half-width.
            const RectI viewport = AspectFitLegacyCanvas(
                static_cast<int>(g_unified_ui.width),
                static_cast<int>(g_unified_ui.height));
            x = CorrectCompanyColumnCenter(x,
                MulDiv(cached->width, viewport.width, LegacyCanvas::width),
                cached->width);
        }
        if (g_company_revenue_waiting_for_bars && cached &&
            IsCompanyRevenueChartMemberGeometry(self, cached)) {
            // The report controller must see and update the chart in its
            // authored coordinate system before the fitted geometry is
            // committed. Passing these moves through lets it replace the
            // ten full-height resource placeholders with real values.
            original(self, x, y);
            return true;
        }
        if (!bar_update && IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = cached ? cached : RememberGeometry(
            g_company_revenue_geometry,
            std::size(g_company_revenue_geometry),
            g_company_revenue_geometry_count, self);
        if (native) {
            const bool live_bar = bar_update &&
                IsCompanyRevenueBarGeometry(self, native) &&
                ((x >= 12 && x <= 16) || (x >= 33 && x <= 37)) &&
                y >= 0 && y <= 163;
            if (live_bar) {
                // 00484D15 resizes the bar before 00484D3C moves it.
                // Retain that live height, including the one-pixel zero bar.
                const int native_height =
                    GuiField<int>(self, GuiObjectField::height);
                native->x = x;
                native->y = y;
                native->height = native_height;
                GuiField<int>(bytes, GuiObjectField::width) =
                    MulDiv(native->width, viewport_width,
                           LegacyCanvas::width);
                GuiField<int>(bytes, GuiObjectField::height) =
                    MulDiv(native_height, viewport_height,
                           LegacyCanvas::height);
                original(self,
                    MulDiv(x, viewport_width, LegacyCanvas::width),
                    MulDiv(y, viewport_height, LegacyCanvas::height));
                return true;
            }
            GuiField<int>(bytes, GuiObjectField::width) =
                MulDiv(native->width, viewport_width, LegacyCanvas::width);
            GuiField<int>(bytes, GuiObjectField::height) =
                MulDiv(native->height, viewport_height, LegacyCanvas::height);
            original(self,
                MulDiv(x, viewport_width, LegacyCanvas::width),
                MulDiv(y, viewport_height, LegacyCanvas::height));
            return true;
        }
    }
    return false;
}

bool MoveAirportPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_airport_selection_root) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        original(self, viewport_x, viewport_y);
        return true;
    }
    if (parent && IsAirportSelectionDescendant(self)) {
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_airport_selection_geometry,
            std::size(g_airport_selection_geometry),
            g_airport_selection_geometry_count, self);
        if (native) {
            if (IsAirportAirplaneGeometry(native)) {
                // The route controller reads this same rectangle back while
                // interpolating and deciding when the airplane is visible.
                // Preserve native logical geometry; only rendering is scaled.
                GuiField<int>(bytes, GuiObjectField::width) = native->width;
                GuiField<int>(bytes, GuiObjectField::height) = native->height;
                original(self, x, y);
            } else {
                GuiField<int>(bytes, GuiObjectField::width) =
                    MulDiv(native->width, viewport_width, LegacyCanvas::width);
                GuiField<int>(bytes, GuiObjectField::height) =
                    MulDiv(native->height, viewport_height, LegacyCanvas::height);
                original(self,
                    MulDiv(native->x, viewport_width, LegacyCanvas::width),
                    MulDiv(native->y, viewport_height, LegacyCanvas::height));
            }
            return true;
        }
    }
    return false;
}

bool MoveStudioPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    const StaticPageMoveRule studio_pages[] = {
        {g_studio_event_list_root, g_studio_event_list_geometry,
         std::size(g_studio_event_list_geometry), &g_studio_event_list_geometry_count,
         8, PageMoveCoordinates::SubmittedNative},
        {g_studio_event_editor_root, g_studio_event_editor_geometry,
         std::size(g_studio_event_editor_geometry), &g_studio_event_editor_geometry_count,
         8, PageMoveCoordinates::SubmittedNative},
    };
    if (ApplyStaticPageMove(studio_pages, std::size(studio_pages), self, parent,
                            static_cast<int>(g_unified_ui.width),
                            static_cast<int>(g_unified_ui.height), x, y,
                            IsRepeatedObjectPosition)) {
        original(self, x, y);
        return true;
    }
    return false;
}

bool MoveTrainingFramePage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_training_minigame_root) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) =
            MulDiv(426, viewport_width, LegacyCanvas::width);
        GuiField<int>(bytes, GuiObjectField::height) =
            MulDiv(369, viewport_height, LegacyCanvas::height);
        original(self,
            viewport_x + MulDiv(187, viewport_width, LegacyCanvas::width),
            viewport_y + MulDiv(126, viewport_height, LegacyCanvas::height));
        return true;
    }
    if (parent && IsTrainingMinigameDescendant(self)) {
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_training_minigame_geometry,
            std::size(g_training_minigame_geometry),
            g_training_minigame_geometry_count, self);
        if (native) {
            GuiField<int>(bytes, GuiObjectField::width) =
                MulDiv(native->width, viewport_width, LegacyCanvas::width);
            GuiField<int>(bytes, GuiObjectField::height) =
                MulDiv(native->height, viewport_height, LegacyCanvas::height);
            // Training activities move prompts and judgement markers while
            // they run. Scale the coordinates supplied by this update rather
            // than replaying the first cached position, while retaining the
            // cached authored size to avoid cumulative scaling.
            // The timer's depletion is rendered inside a stationary control.
            // Some updates echo its already-scaled position; scaling that
            // value again causes a one-frame jump. Keep only the timer group
            // on its cached authored anchor while other activity objects use
            // their live coordinates.
            const bool fixed_timer = IsTrainingTimerGeometry(native);
            const int native_x = fixed_timer ? native->x : x;
            const int native_y = fixed_timer ? native->y : y;
            original(self,
                MulDiv(native_x, viewport_width, LegacyCanvas::width),
                MulDiv(native_y, viewport_height, LegacyCanvas::height));
            return true;
        }
    }
    return false;
}

bool MoveTrainingActivitiesPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    // Activity roots coexist and share a cache; preserve roots-first group
    // dispatch rather than treating each root as an independent page rule.
    const StaticPageMoveRule training_activities{
        nullptr, g_training_activity_geometry,
        std::size(g_training_activity_geometry), &g_training_activity_geometry_count,
        10, PageMoveCoordinates::SubmittedNative};
    if (ApplyPageGroupMove(training_activities, g_training_activity_roots,
                            g_training_activity_root_count, self, parent,
                            static_cast<int>(g_unified_ui.width),
                            static_cast<int>(g_unified_ui.height), x, y,
                            IsRepeatedObjectPosition)) {
        original(self, x, y);
        return true;
    }
    return false;
}

bool MoveAnnouncementPage(PageMoveContext& context) {
    auto& [self, parent, x, y, original, executable, immediate_call] = context;
    auto* bytes = static_cast<unsigned char*>(self);
    if (self == g_announcement_root && IsCachedAnnouncementRootValid()) {
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
        GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
        g_scaling_announcement_subtree = true;
        original(self, viewport_x, viewport_y);
        g_scaling_announcement_subtree = false;
        return true;
    }
    if (parent && IsAnnouncementDescendant(self)) {
        if (g_scaling_announcement_subtree) {
            original(self, x, y);
            return true;
        }
        if (IsRepeatedObjectPosition(self, x, y)) {
            original(self, x, y);
            return true;
        }
        const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
            AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                  static_cast<int>(g_unified_ui.height));
        TitleNativeGeometry* native = RememberGeometry(
            g_announcement_geometry, std::size(g_announcement_geometry),
            g_announcement_geometry_count, self);
        if (native) {
            // Announcement captions and stat deltas are moved after the page
            // is constructed.  Keep their latest authored 800x600 position
            // instead of freezing the first (often 0,0) construction value.
            native->x = x;
            native->y = y;
            GuiField<int>(bytes, GuiObjectField::width) =
                MulDiv(native->width, viewport_width, LegacyCanvas::width);
            GuiField<int>(bytes, GuiObjectField::height) =
                MulDiv(native->height, viewport_height, LegacyCanvas::height);
            original(self,
                MulDiv(native->x, viewport_width, LegacyCanvas::width),
                MulDiv(native->y, viewport_height, LegacyCanvas::height));
            return true;
        }
    }
    return false;
}

bool DispatchAspectFitPageMove(PageMoveContext& context) {
    using Handler = bool (*)(PageMoveContext&);
    // Keep inter-page priority identical to the original HookGuiMove chain.
    static constexpr Handler handlers[] = {
        MoveEndGamePage,
        MoveTitlePage,
        MoveStaticPage,
        MoveInventoryPage,
        MoveSaveLoadPage,
        MoveBigActivityPage,
        MoveCompanyNavigationPage,
        MoveCompanySectionPage,
        MoveCompanyRevenuePage,
        MoveAirportPage,
        MoveStudioPage,
        MoveTrainingFramePage,
        MoveTrainingActivitiesPage,
        MoveAnnouncementPage,
    };
    return DispatchFirstHandled(handlers, context);
}

void __fastcall HookGuiMove(void* self, void*, int x, int y) {
    auto original = reinterpret_cast<GuiMoveFn>(g_unified_ui.trampoline);
    if (!original || !self) {
        return;
    }
    // A single move callback performs many ancestry and signature checks on
    // the same GUI heap. Share VirtualQuery results only within this callback;
    // the cache is discarded before returning to the game.
    GuiObjectReadBatch read_batch;

    auto* bytes = static_cast<unsigned char*>(self);
    void* immediate_call = static_cast<unsigned char*>(_ReturnAddress()) - 5;
    void* world_source_return = nullptr;
    HMODULE executable = g_unified_ui.executable_base;
    if (executable && immediate_call ==
            static_cast<unsigned char*>(static_cast<void*>(executable)) +
                0xD7125) {
        // Stardom3's wrapper at 004D7100 pushes x/y and calls GuiMove at
        // 004D7125. At GuiMove entry its own caller's return address is the
        // fourth stack slot: [return-to-wrapper, x, y, source-return].
        auto** return_slot = reinterpret_cast<void**>(_AddressOfReturnAddress());
        world_source_return = return_slot[3];
    }
    void* parent = GuiField<void*>(bytes, GuiObjectField::parent);
    if (!g_company_navigation_root) {
        // Construction/show callbacks can arrive before the next BeginScene.
        DiscoverCompanyNavigation(parent);
    }
    if (self == g_phone_overlay_button &&
        parent == g_phone_overlay_root && IsPhoneOverlayRinging()) {
        const int width = GuiField<int>(bytes, GuiObjectField::width);
        const int height = GuiField<int>(bytes, GuiObjectField::height);
        const bool off_screen = x + width <= 0 || y + height <= 0 ||
            x >= static_cast<int>(g_unified_ui.width) ||
            y >= static_cast<int>(g_unified_ui.height);
        const bool authored_position = x >= 0 && x <= 8 &&
            y >= 550 && y <= 565;
        if (off_screen || authored_position) {
            GetPhoneOverlayButtonPosition(x, y);
        }
        original(self, x, y);
        if (GuiField<int>(bytes, GuiObjectField::x) != x ||
            GuiField<int>(bytes, GuiObjectField::y) != y) {
            GuiField<int>(bytes, GuiObjectField::x) = x;
            GuiField<int>(bytes, GuiObjectField::y) = y;
        }
        return;
    }
    PageMoveContext page_move{self, parent, x, y, original, executable, immediate_call};
    if (DispatchAspectFitPageMove(page_move)) {
        return;
    }
    const int current_width = GuiField<int>(bytes, GuiObjectField::width);
    const int current_height = GuiField<int>(bytes, GuiObjectField::height);
    if (self == g_title_tutorial_bubble ||
        IsTitleTutorialDialogueBubble(
            self, parent, current_width, current_height, x, y)) {
        ResetTitleTutorialBubbleGeometry(self);
        ScaleTitleTutorialBubbleSubtree(self);
        return;
    }
    if (parent) {
        auto* parent_bytes = static_cast<unsigned char*>(parent);
        int& parent_width = GuiField<int>(parent_bytes, GuiObjectField::width);
        int& parent_height = GuiField<int>(parent_bytes, GuiObjectField::height);
        const bool legacy_root = parent_width >= 790 && parent_width <= 810 &&
            parent_height >= 590 && parent_height <= 610;
        void* grandparent = GuiField<void*>(parent_bytes, GuiObjectField::parent);
        if (IsLoadingPageRoot(parent, grandparent,
                              parent_width, parent_height) &&
            g_loading_page_root != parent) {
            g_loading_page_root = parent;
            Log("Unified UI loading page discovered during construction self=%p",
                parent);
        }
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
                   !IsProcessedLayoutObject(parent) &&
                   (grandparent == g_unified_ui.primary_root ||
                    IsKnownLayoutRoot(grandparent))) {
            // A completed centered 800x600 page keeps its authored size.
            // Dialogue controllers move several children when advancing to
            // the next line; rescanning the full primary tree for every one
            // of those moves stalls the render thread. Only use this path to
            // discover a legacy container during its first construction.
            ReflowExistingRootChildren(grandparent);
        }

        if (IsGroupCanvas(parent)) {
            original(self, x, y);
            return;
        }

        if (parent == g_awards_ceremony_root &&
            IsAwardsCeremonyCurtainLayer(self, parent)) {
            LayoutAwardsCeremonyCurtainLayer(self);
            return;
        }

        const int self_width = GuiField<int>(bytes, GuiObjectField::width);
        const int self_height = GuiField<int>(bytes, GuiObjectField::height);
        const bool possible_cg_overlay = parent == g_unified_ui.primary_root &&
            self_width >= 798 && self_width <= 802 &&
            ((self_height >= 72 && self_height <= 80) ||
             (self_height >= 122 && self_height <= 134));
        if (possible_cg_overlay && !IsInGameCGVisible()) {
            DiscoverInGameCGSurfaces(g_unified_ui.primary_root);
        }
        const bool direct_cg_caption = IsInGameCGCaption(
            self, parent, self_width, self_height);
        if (direct_cg_caption) {
            if (self != g_unified_ui.in_game_cg_caption) {
                g_unified_ui.in_game_cg_caption = self;
                if (y >= 0 && y <= LegacyCanvas::height) {
                    g_unified_ui.in_game_cg_caption_native_y = y;
                }
                g_unified_ui.in_game_cg_caption_logged = false;
            }
            GetInGameCGCaptionPosition(
                g_unified_ui.in_game_cg_caption_native_y, x, y);
            original(self, x, y);
            return;
        }
        const bool direct_status_notice = IsInGameCGItemNotice(
            self, parent, self_width, self_height);
        if (direct_status_notice) {
            if (self != g_unified_ui.in_game_cg_item_notice) {
                g_unified_ui.in_game_cg_item_notice = self;
                if (x >= -LegacyCanvas::width && x <= LegacyCanvas::width && y >= -200 && y <= LegacyCanvas::height) {
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
            const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
                AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                      static_cast<int>(g_unified_ui.height));
            GuiField<int>(bytes, GuiObjectField::width) = viewport_width;
            GuiField<int>(bytes, GuiObjectField::height) = viewport_height;
            original(self, MulDiv(x, viewport_width, LegacyCanvas::width),
                MulDiv(y, viewport_height, LegacyCanvas::height));
            return;
        }
        if (self == g_unified_ui.photo_album_root) {
            const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
                AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                      static_cast<int>(g_unified_ui.height));
            original(self, viewport_x, viewport_y);
            return;
        }
        if (IsPhotoAlbumDescendant(parent)) {
            if (IsRepeatedObjectPosition(self, x, y)) {
                original(self, x, y);
                return;
            }
            const int current_x = GuiField<int>(bytes, GuiObjectField::x);
            const int current_y = GuiField<int>(bytes, GuiObjectField::y);
            const auto [viewport_x, viewport_y, viewport_width, viewport_height] =
                AspectFitLegacyCanvas(static_cast<int>(g_unified_ui.width),
                                      static_cast<int>(g_unified_ui.height));
            TransformPhotoAlbumCarouselFrame(
                self, immediate_call, current_x, current_y,
                viewport_width, viewport_height, x, y);
            original(self, x, y);
            return;
        }

        StudioEventDropdownLayout* dropdown_owner =
            FindStudioEventDropdownOwner(self);
        if (!HasTitleTutorialPage() && dropdown_owner &&
            !dropdown_owner->tutorial_mode &&
            self != dropdown_owner->root) {
            if (IsRepeatedObjectPosition(self, x, y)) {
                original(self, x, y);
                return;
            }
            const auto [viewport_x, viewport_y, viewport_width,
                        viewport_height] = AspectFitLegacyCanvas(
                static_cast<int>(g_unified_ui.width),
                static_cast<int>(g_unified_ui.height));
            TitleNativeGeometry* native = RememberGeometry(
                dropdown_owner->geometry,
                std::size(dropdown_owner->geometry),
                dropdown_owner->geometry_count, self);
            if (native) {
                GuiField<int>(bytes, GuiObjectField::width) = MulDiv(
                    native->width, viewport_width, LegacyCanvas::width);
                GuiField<int>(bytes, GuiObjectField::height) = MulDiv(
                    native->height, viewport_height, LegacyCanvas::height);
                original(self,
                    MulDiv(x, viewport_width, LegacyCanvas::width),
                    MulDiv(y, viewport_height, LegacyCanvas::height));
                return;
            }
        }

        if (primary_legacy_root || IsKnownLayoutRoot(parent)) {
            // Do not rescan the complete root from this move callback. World
            // overlays (notably the character hover/status tag) are moved on
            // every mouse update, and rescanning thousands of siblings here
            // stalls the render thread. The object being moved is adapted by
            // the dispatch below; newly attached siblings are picked up by
            // their own Move call or by the low-frequency maintenance pass.
            const int width = GuiField<int>(bytes, GuiObjectField::width);
            const int height = GuiField<int>(bytes, GuiObjectField::height);
            // Full-canvas proxy objects are layout containers, not visible HUD
            // widgets. Keep their origin stable so descendants are not shifted
            // twice. Ordinary root children receive one anchor transform.
            if (IsFullscreenLeafSurface(self, parent, width, height)) {
                GuiField<int>(bytes, GuiObjectField::width) =
                    static_cast<int>(g_unified_ui.width);
                GuiField<int>(bytes, GuiObjectField::height) =
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
                IsTeamFriendshipTag(self, parent, width, height) ||
                IsWorldDialogueChoicePanel(self, parent, width, height) ||
                IsWorldProjectedObject(self)) {
                // Its children are positioned relative to the selected actor;
                // leave both initial placement and later game updates intact.
                RememberWorldProjectedObject(self);
                RememberWorldMoveSource(world_source_return);
                RememberProcessedLayoutObject(self);
                original(self, x, y);
                return;
            } else if (self == g_publication_cover ||
                       IsEventPublicationCover(
                           self, parent, width, height)) {
                // Like the older 510x350 newspaper panel, the publication
                // controller derives later frames from the origin translated
                // during reflow. Do not transform those frames again. Object
                // identity is retained because the first Move can occur while
                // its four child layers are still being constructed.
                g_publication_cover = self;
                RememberProcessedLayoutObject(self);
            } else if (IsEventPublicationPanel(self, parent, width, height)) {
                // ReflowExistingRootChildren has already translated the
                // panel's resting point. The animation controller then emits
                // absolute frames relative to that translated point, so pass
                // those frames through unchanged to retain the full motion
                // without applying the widescreen offset a second time.
                RememberProcessedLayoutObject(self);
            } else if (IsScheduleJobHoverTooltip(
                           self, parent, width, height)) {
                TransformScheduleJobHoverTooltip(self, x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsScheduleSecondaryPanel(self, parent, width, height)) {
                TransformCenteredScheduleOverlay(
                    self, parent, width, height, x, y);
                RememberProcessedLayoutObject(self);
            } else if (TransformCenteredPageOverlay(
                           self, parent, width, height, x, y)) {
                RememberProcessedLayoutObject(self);
            } else if (IsSmallDialoguePortrait(self, parent, width, height)) {
                PlaceCenteredLegacyOverlay(95, 403, x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsStudioEventEditorDropdown(
                           self, parent, width, height)) {
                StudioEventDropdownLayout* layout =
                    RegisterStudioEventDropdownLayout(
                        self, width, height);
                if (layout) {
                    ScaleStudioEventDropdownSubtree(
                        self, *layout, x, y);
                    return;
                }
            } else if (StudioEventDropdownLayout* layout =
                           FindStudioEventDropdownLayout(self);
                       layout && IsTitleTutorialDropdownAnchor(x, y)) {
                RestoreTitleTutorialDropdown(*layout, x, y);
                return;
            } else if (!HasTitleTutorialPage() &&
                       FindStudioEventDropdownLayout(self)) {
                StudioEventDropdownLayout* layout =
                    FindStudioEventDropdownLayout(self);
                layout->tutorial_mode = false;
                if (IsRepeatedObjectPosition(self, x, y)) {
                    original(self, x, y);
                    return;
                }
                const auto [viewport_x, viewport_y, viewport_width,
                            viewport_height] = AspectFitLegacyCanvas(
                    static_cast<int>(g_unified_ui.width),
                    static_cast<int>(g_unified_ui.height));
                GuiField<int>(bytes, GuiObjectField::width) = MulDiv(
                    layout->native_width,
                    viewport_width, LegacyCanvas::width);
                GuiField<int>(bytes, GuiObjectField::height) = MulDiv(
                    layout->native_height,
                    viewport_height, LegacyCanvas::height);
                x = viewport_x + MulDiv(x, viewport_width, LegacyCanvas::width);
                y = viewport_y + MulDiv(y, viewport_height, LegacyCanvas::height);
                RememberProcessedLayoutObject(self);
            } else if (IsTitleTutorialProfileDropdown(
                           self, parent, width, height)) {
                // Tutorial selectors share the same pooled GUI objects as the
                // editor. Their centered native layout takes precedence over
                // any event-dropdown geometry retained at the same address.
                TransformTitleTutorialProfileDropdown(self, x, y);
                RememberProcessedLayoutObject(self);
            } else if (self == g_awards_ceremony_root) {
                x = (static_cast<int>(g_unified_ui.width) -
                     LegacyCanvas::width) / 2;
                y = (static_cast<int>(g_unified_ui.height) -
                     LegacyCanvas::height) / 2;
                RememberProcessedLayoutObject(self);
            } else if (IsSmallWorldDialogueBubble(self, parent, width, height)) {
                TransformWorldAnchorPosition(x, y);
                RememberProcessedLayoutObject(self);
            } else if (IsToolbarAnimationObject(parent, width, height, x, y)) {
                void* call_site = static_cast<unsigned char*>(
                    _ReturnAddress()) - 5;
                TransformToolbarSequenceFrame(
                    self, width, height, x, y, call_site);
                RememberProcessedLayoutObject(self);
            } else if (IsGroupCanvas(self)) {
                x = (static_cast<int>(g_unified_ui.width) - LegacyCanvas::width) / 2;
                y = (static_cast<int>(g_unified_ui.height) - LegacyCanvas::height) / 2;
            } else if (IsWorldMoveSource(world_source_return)) {
                // Learned world-projection sources are a fallback. Known UI
                // surfaces above must keep their specialized transforms even
                // if they happen to be moved by a previously observed caller.
                RememberWorldProjectedObject(self);
                RememberProcessedLayoutObject(self);
                original(self, x, y);
                return;
            } else if (width < 760 && height < 560) {
                if (IsRepeatedObjectPosition(self, x, y)) {
                    original(self, x, y);
                    return;
                }
                // The toolbar's collapse button is a separate root child just
                // left of GameMain. Lower-right children therefore share the
                // same group anchor even if a small child's center is middle.
                if (IsWorldRoot(parent)) {
                    TransformWorldPosition(width, height, x, y);
                } else {
                    TransformRootChildPosition(width, height, x, y);
                }
                RememberProcessedLayoutObject(self);
            }
        }
    }
    original(self, x, y);
}

bool InstallCompanyAnimationHook(HMODULE executable) {
    if (g_company_animate_move) {
        return true;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x1187A0;
    // Complete instructions only; this prologue contains no relative operands.
    constexpr unsigned char expected[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Company animation hook signature mismatch at %p", target);
        return false;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, sizeof(expected) + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, expected, sizeof(expected));
    trampoline[sizeof(expected)] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + sizeof(expected) + 1) =
        static_cast<int32_t>(target - trampoline - 5);
    DWORD protection = 0;
    if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE,
                        &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_company_animate_move = reinterpret_cast<GuiAnimateMoveFn>(trampoline);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookCompanyAnimateMove) - target - 5);
    target[5] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(expected) + 5);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
    Log("Installed company native-coordinate animation hook at %p", target);
    return true;
}

bool InstallCompanyListDrawHook(HMODULE executable) {
    if (g_company_list_draw) {
        return true;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x133700;
    constexpr unsigned char expected[] = {
        0x8B, 0x44, 0x24, 0x0C, 0x81, 0xEC, 0x3C, 0x01, 0x00, 0x00};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Company list draw signature mismatch at %p", target);
        return false;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, sizeof(expected) + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, expected, sizeof(expected));
    trampoline[sizeof(expected)] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + sizeof(expected) + 1) =
        static_cast<int32_t>(target - trampoline - 5);
    DWORD protection = 0;
    if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE,
                        &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_company_list_draw = reinterpret_cast<CompanyListDrawFn>(trampoline);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookCompanyListDraw) - target - 5);
    std::memset(target + 5, 0x90, sizeof(expected) - 5);
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(expected) + 5);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
    Log("Installed company list render-metrics hook at %p", target);
    return true;
}

bool InstallFittedTextDrawHook(HMODULE executable) {
    if (g_fitted_text_draw) {
        return true;
    }
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x130310;
    auto* measure = reinterpret_cast<unsigned char*>(executable) + 0x130710;
    constexpr unsigned char expected[] = {
        0x8B, 0x44, 0x24, 0x0C, 0x83, 0xEC, 0x34};
    constexpr unsigned char measure_expected[] = {
        0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x56, 0x8B, 0xF1};
    if (std::memcmp(target, expected, sizeof(expected)) != 0 ||
        std::memcmp(measure, measure_expected, sizeof(measure_expected)) != 0) {
        Log("Aspect-fit text draw/measure signature mismatch");
        return false;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, sizeof(expected) + 5, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, expected, sizeof(expected));
    trampoline[sizeof(expected)] = 0xE9;
    *reinterpret_cast<int32_t*>(trampoline + sizeof(expected) + 1) =
        static_cast<int32_t>(target - trampoline - 5);
    DWORD protection = 0;
    if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE,
                        &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    g_fitted_text_measure = reinterpret_cast<TextMeasureFn>(measure);
    g_fitted_text_draw = reinterpret_cast<CompanyListDrawFn>(trampoline);
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(
        reinterpret_cast<unsigned char*>(&HookFittedTextDraw) - target - 5);
    std::memset(target + 5, 0x90, sizeof(expected) - 5);
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, sizeof(expected) + 5);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
    Log("Installed aspect-fit text rendering hook at %p", target);
    return true;
}

bool InstallUnifiedUILayoutHook(UINT width, UINT height, int title_screen_mode) {
    if (g_unified_ui.installed || width < LegacyCanvas::width ||
        height < LegacyCanvas::height) {
        return g_unified_ui.installed;
    }

    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }
    g_unified_ui.executable_base = executable;
    auto* target = reinterpret_cast<unsigned char*>(executable) + 0x1185A0;
    const unsigned char expected[] = {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56};
    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Unified UI hook signature mismatch at %p", target);
        LogProbeBytes(target, 16);
        return false;
    }

    // EndGame compares each scrolling plane's live y coordinate against a
    // native -600 cutoff before hiding or refilling it. The plane itself is
    // aspect-fitted, so patch that immediate to the fitted viewport height.
    // This keeps every controller update in one coordinate space even when
    // the game advances the animation more than once between presentations.
    auto* credit_cutoff = reinterpret_cast<std::int32_t*>(
        reinterpret_cast<unsigned char*>(executable) + 0xD02EB);
    constexpr std::int32_t kNativeCreditCutoff = -600;
    const RectI end_game_viewport = AspectFitLegacyCanvas(
        static_cast<int>(width), static_cast<int>(height));
    const std::int32_t fitted_credit_cutoff = -end_game_viewport.height;
    if (*credit_cutoff != kNativeCreditCutoff &&
        *credit_cutoff != fitted_credit_cutoff) {
        Log("End-game credit cutoff signature mismatch at %p value=%d",
            credit_cutoff, *credit_cutoff);
        return false;
    }
    if (*credit_cutoff != fitted_credit_cutoff) {
        DWORD cutoff_protection = 0;
        if (!VirtualProtect(credit_cutoff, sizeof(*credit_cutoff),
                            PAGE_EXECUTE_READWRITE, &cutoff_protection)) {
            Log("End-game credit cutoff VirtualProtect failed error=%lu",
                GetLastError());
            return false;
        }
        *credit_cutoff = fitted_credit_cutoff;
        DWORD ignored = 0;
        VirtualProtect(credit_cutoff, sizeof(*credit_cutoff),
                       cutoff_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), credit_cutoff,
                              sizeof(*credit_cutoff));
    }
    Log("End-game credit cutoff patched %d -> %d at %p",
        kNativeCreditCutoff, fitted_credit_cutoff, credit_cutoff);

    auto* resize_target = reinterpret_cast<unsigned char*>(executable) +
        0x118690;
    const unsigned char resize_expected[] = {
        0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x57,
    };
    if (std::memcmp(resize_target, resize_expected,
                    sizeof(resize_expected)) == 0) {
        g_gui_resize = reinterpret_cast<GuiResizeFn>(resize_target);
    } else {
        Log("Unified UI resize signature mismatch at %p", resize_target);
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
    if (!InstallCompanyAnimationHook(executable)) {
        Log("Company navigation animation correction unavailable");
    }
    if (!InstallCompanyListDrawHook(executable)) {
        Log("Company achievement list render correction unavailable");
    }
    if (!InstallFittedTextDrawHook(executable)) {
        Log("Aspect-fit page font correction unavailable");
    }
    Log("Installed unified UI layout hook at %p for %ux%u titleMode=%d",
        target, width, height, title_screen_mode);
    return true;
}

bool PatchActiveTagBounds(UINT width, UINT height) {
    if (width < LegacyCanvas::width || height < LegacyCanvas::height) {
        return false;
    }
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable) {
        return false;
    }

    struct BoundPatch {
        uintptr_t offset;
        uint32_t expected;
        uint32_t replacement;
        const char* name;
    };
    // ActiveTag::Move hides the shared toolbar/character explanation label
    // whenever the cursor is outside the original 800x600 canvas. Preserve
    // its two-pixel edge margin while extending the check to the output.
    const BoundPatch patches[] = {
        {0x83CC0, 798u, width - 2u, "x"},
        {0x83CCB, 598u, height - 2u, "y"},
    };

    bool all_patched = true;
    for (const auto& patch : patches) {
        auto* target = reinterpret_cast<uint32_t*>(
            reinterpret_cast<unsigned char*>(executable) + patch.offset);
        if (*target == patch.replacement) {
            continue;
        }
        if (*target != patch.expected) {
            Log("ActiveTag bound signature mismatch %s at %p value=%u",
                patch.name, target, *target);
            all_patched = false;
            continue;
        }
        DWORD old_protection = 0;
        if (!VirtualProtect(target, sizeof(*target), PAGE_EXECUTE_READWRITE,
                            &old_protection)) {
            Log("ActiveTag bound VirtualProtect failed %s error=%lu",
                patch.name, GetLastError());
            all_patched = false;
            continue;
        }
        *target = patch.replacement;
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(*target), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(*target));
        Log("ActiveTag bound patched %s %u -> %u at %p",
            patch.name, patch.expected, patch.replacement, target);
    }
    return all_patched;
}

bool PatchMapLocationProjectionBounds(UINT width, UINT height) {
    if (width < LegacyCanvas::width || height < LegacyCanvas::height) {
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
