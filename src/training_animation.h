#pragma once

#include "gui_object.h"
#include "ui_page_geometry.h"

#include <windows.h>
#include <initializer_list>

namespace stardom {

// SpeakSkill replaces each answered DBCS character with two ASCII spaces.
// Render both colour layers on the same character grid, independent of the
// replacement font's proportional space advance. Validate before any drawing.
template <typename Draw>
bool DrawTrainingAnswerCells(const char* text, Draw draw) {
    if (!text) return false;
    size_t length = 0;
    for (; length < 24 && text[length]; length += 2) {
        const auto lead = static_cast<unsigned char>(text[length]);
        const auto trail = static_cast<unsigned char>(text[length + 1]);
        if (!((lead == ' ' && trail == ' ') ||
              (lead >= 0x81 && lead <= 0xFE && trail >= 0x40 &&
               trail <= 0xFE && trail != 0x7F))) return false;
    }
    if (length == 24) return false;
    for (size_t offset = 0; offset < length; offset += 2) {
        if (text[offset] == ' ') continue;
        char glyph[] = {text[offset], text[offset + 1], 0};
        draw(static_cast<int>(offset / 2), glyph);
    }
    return true;
}

template <typename Start>
void StartTrainingAnimation(void* object, size_t first_field,
        int viewport_width, int viewport_height, Start start) {
    const int first = GuiField<int>(object, first_field);
    const int second = GuiField<int>(object, first_field + sizeof(int));
    GuiField<int>(object, first_field) = MulDiv(first, 800, viewport_width);
    GuiField<int>(object, first_field + sizeof(int)) = MulDiv(second, 600, viewport_height);
    start();
    GuiField<int>(object, first_field) = first;
    GuiField<int>(object, first_field + sizeof(int)) = second;
}

inline void FitTrainingCopiedPosition(int& x, int& y, int offset_x,
        int offset_y, int viewport_width, int viewport_height) {
    x += MulDiv(offset_x, viewport_width, 800) - offset_x;
    y += MulDiv(offset_y, viewport_height, 600) - offset_y;
}

// MatrixStage also reads live y against authored judgement bands. Keep its
// descendants native for the complete controller call, including moves and
// animation starts, then fit the updated geometry once when it returns.
template <typename Update>
void UpdateTrainingNativeObjects(NativeGeometry* const* objects, size_t count,
        int viewport_width, int viewport_height, Update update) {
    for (size_t i = 0; i < count; ++i) {
        void* object = objects[i]->object;
        for (size_t field : {GuiObjectField::x, GuiObjectField::width}) {
            GuiField<int>(object, field) = MulDiv(GuiField<int>(object, field), 800, viewport_width);
            GuiField<int>(object, field + 4) = MulDiv(GuiField<int>(object, field + 4), 600, viewport_height);
        }
    }
    update();
    for (size_t i = 0; i < count; ++i) {
        void* object = objects[i]->object;
        objects[i]->width = GuiField<int>(object, GuiObjectField::width);
        objects[i]->height = GuiField<int>(object, GuiObjectField::height);
        for (size_t field : {GuiObjectField::x, GuiObjectField::width}) {
            GuiField<int>(object, field) = MulDiv(GuiField<int>(object, field), viewport_width, 800);
            GuiField<int>(object, field + 4) = MulDiv(GuiField<int>(object, field + 4), viewport_height, 600);
        }
    }
}

// SpeakSkill owns 23 records: authored x/y, active/text, bubble, label.
// Its controller compares GUI positions with those anchors, then GuiAnimateMove
// snapshots the GUI position as the interpolation start. Both must be native.
template <typename Lookup, typename Update>
void UpdateTrainingBubbles(void* controller, int viewport_width,
                           int viewport_height, Lookup lookup, Update update) {
    struct SavedPosition { void* object; int x; int y; } saved[23]{};
    for (int i = 0; i < 23; ++i) {
        auto* record = static_cast<unsigned char*>(controller) + 0x290 + i * 0x14;
        void* bubble = GuiPointer(record, 0x0C);
        const NativeGeometry* native = lookup(bubble);
        if (!native) continue;
        saved[i] = {bubble, GuiField<int>(bubble, GuiObjectField::x),
                            GuiField<int>(bubble, GuiObjectField::y)};
        // Initialization can run either before or after layout discovery.
        // The geometry cache always retains the authored resting anchor.
        GuiField<int>(record, 0) = native->x;
        GuiField<int>(record, 4) = native->y;
        GuiField<int>(bubble, GuiObjectField::x) =
            MulDiv(saved[i].x, 800, viewport_width);
        GuiField<int>(bubble, GuiObjectField::y) =
            MulDiv(saved[i].y, 600, viewport_height);
    }
    update(controller);
    for (const auto& position : saved) {
        if (!position.object) continue;
        GuiField<int>(position.object, GuiObjectField::x) = position.x;
        GuiField<int>(position.object, GuiObjectField::y) = position.y;
    }
}

}  // namespace stardom
