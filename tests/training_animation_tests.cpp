#include "training_animation.h"

#include <array>
#include <cstdio>

namespace {
struct Object { alignas(void*) unsigned char bytes[0x100]{}; };
bool Expect(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "%s\n", message);
    return value;
}

bool TestSharedTrainingAnimations(int width, int height) {
    using namespace stardom;
    bool ok = true;
    Object object;
    // BeautyBalance/MatrixStage start movement from the current fitted point.
    // BodyActive/MatrixStage also animate width and height at the same time.
    for (size_t field : {GuiObjectField::x, GuiObjectField::width}) {
        for (int value : {-20, 0, 9, 90, 110, 180, 290}) {
            const int fitted = MulDiv(value, width, 800);
            GuiField<int>(object.bytes, field) = fitted;
            GuiField<int>(object.bytes, field + 4) = MulDiv(50, height, 600);
            StartTrainingAnimation(object.bytes, field, width, height, [&] {
                ok &= Expect(GuiField<int>(object.bytes, field) == value &&
                    GuiField<int>(object.bytes, field + 4) == 50,
                    "shared animation captured a fitted endpoint");
            });
            ok &= Expect(GuiField<int>(object.bytes, field) == fitted,
                "shared animation failed to restore display geometry");
        }
    }
    for (int offset : {0, -5, -15, -32, -42, -45}) {
        int x = MulDiv(150, width, 800) + offset;
        int y = MulDiv(200, height, 600) + offset;
        FitTrainingCopiedPosition(x, y, offset, offset, width, height);
        ok &= Expect(x == MulDiv(150, width, 800) + MulDiv(offset, width, 800) &&
            y == MulDiv(200, height, 600) + MulDiv(offset, height, 600),
            "copied effect position was scaled twice");
    }
    int cursor_x = 900 - 50, cursor_y = 600 - 36;
    FitTrainingCopiedPosition(cursor_x, cursor_y, -50, -36, width, height);
    ok &= Expect(cursor_x + MulDiv(50, width, 800) == 900 &&
        cursor_y + MulDiv(36, height, 600) == 600, "camera hotspot drifted from mouse");
    int memory_x = 500 + 5, memory_y = 300 + 25;
    FitTrainingCopiedPosition(memory_x, memory_y, 5, 25, width, height);
    ok &= Expect(memory_x == 500 + MulDiv(5, width, 800) &&
        memory_y == 300 + MulDiv(25, height, 600), "memory effect offsets changed origin");

    NativeGeometry native{object.bytes, 20, -20, 9, 10};
    NativeGeometry* nodes[] = {&native};
    GuiField<int>(object.bytes, GuiObjectField::x) = MulDiv(20, width, 800);
    GuiField<int>(object.bytes, GuiObjectField::y) = MulDiv(175, height, 600);
    GuiField<int>(object.bytes, GuiObjectField::width) = MulDiv(40, width, 800);
    GuiField<int>(object.bytes, GuiObjectField::height) = MulDiv(50, height, 600);
    UpdateTrainingNativeObjects(nodes, 1, width, height, [&] {
        const int y = GuiField<int>(object.bytes, GuiObjectField::y);
        ok &= Expect(y >= 170 && y <= 180, "stage judgement band used screen pixels");
        GuiField<int>(object.bytes, GuiObjectField::y) = 178;
        GuiField<int>(object.bytes, GuiObjectField::width) = 35;
        GuiField<int>(object.bytes, GuiObjectField::height) = 45;
    });
    ok &= Expect(GuiField<int>(object.bytes, GuiObjectField::y) == MulDiv(178, height, 600),
        "stage controller movement was discarded");
    ok &= Expect(native.width == 35 && native.height == 45 &&
        GuiField<int>(object.bytes, GuiObjectField::width) == MulDiv(35, width, 800),
        "stage size animation was overwritten by the resource size");
    return ok;
}
}

int main() {
    using namespace stardom;
    bool ok = true;
    // Answered prefix and blue remainder must use identical CJK cell indices.
    int cells = 0;
    ok &= Expect(DrawTrainingAnswerCells("    \xA4\xDF\xB8\xCC", [&](int cell, const char*) {
        ok &= Expect(cell == 2 + cells, "answer spaces collapsed the CJK grid");
        ++cells;
    }) && cells == 2, "answer remainder did not draw two glyphs");
    cells = 0;
    ok &= Expect(DrawTrainingAnswerCells("\xB7\x52\xA6\x62", [&](int cell, const char*) {
        ok &= Expect(cell == cells++, "answered prefix shifted cells");
    }) && cells == 2, "answered prefix was lost");
    ok &= Expect(!DrawTrainingAnswerCells("A", [&](int, const char*) {
        ok = false;
    }), "unsupported text should fall back without drawing");
    for (int height : {600, 720, 768, 1080, 1200, 1440, 2160}) {
        const int width = height * 4 / 3;
        ok &= TestSharedTrainingAnimations(width, height);
        alignas(void*) unsigned char controller[0x470]{};
        std::array<Object, 23> bubbles;
        std::array<NativeGeometry, 23> native;
        for (int i = 0; i < 23; ++i) {
            auto* record = controller + 0x290 + i * 0x14;
            auto* bubble = bubbles[i].bytes;
            native[i] = {bubble, 3 + i * 15, 7 + i * 8, 36, 36};
            GuiField<void*>(record, 0x0C) = bubble;
            GuiField<int>(bubble, GuiObjectField::x) = MulDiv(native[i].x, width, 800);
            GuiField<int>(bubble, GuiObjectField::y) = MulDiv(native[i].y, height, 600);
            // Model a controller initialized after fitting its GUI tree.
            GuiField<int>(record, 0) = GuiField<int>(bubble, GuiObjectField::x);
            GuiField<int>(record, 4) = GuiField<int>(bubble, GuiObjectField::y);
        }
        auto lookup = [&](void* object) -> const NativeGeometry* {
            for (const auto& entry : native) if (entry.object == object) return &entry;
            return nullptr;
        };
        // Repeated outward/return animations must begin and end at the same
        // authored anchors, including when an in-flight movement is restarted.
        for (int cycle = 0; cycle < 40; ++cycle) {
            std::array<int, 23> start_x{}, start_y{}, end_x{}, end_y{};
            UpdateTrainingBubbles(controller, width, height, lookup, [&](void* self) {
                ok &= Expect(self == controller, "controller identity changed");
                for (int i = 0; i < 23; ++i) {
                    auto* record = controller + 0x290 + i * 0x14;
                    auto* bubble = bubbles[i].bytes;
                    start_x[i] = GuiField<int>(bubble, GuiObjectField::x);
                    start_y[i] = GuiField<int>(bubble, GuiObjectField::y);
                    const bool resting = start_x[i] == GuiField<int>(record, 0) &&
                        start_y[i] == GuiField<int>(record, 4);
                    ok &= Expect(resting == (cycle % 2 == 0),
                        "resting comparison mixes native and fitted coordinates");
                    end_x[i] = native[i].x + (resting ? (i % 12) - 6 : 0);
                    end_y[i] = native[i].y - (resting ? (i % 6) + 2 : 0);
                }
            });
            for (int frame = 0; frame <= 10; ++frame) {
                for (int i = 0; i < 23; ++i) {
                    auto* bubble = bubbles[i].bytes;
                    if (frame == 0) {
                        ok &= Expect(GuiField<int>(bubble, GuiObjectField::x) ==
                            MulDiv(start_x[i], width, 800), "fitted x was not restored");
                        ok &= Expect(GuiField<int>(bubble, GuiObjectField::y) ==
                            MulDiv(start_y[i], height, 600), "fitted y was not restored");
                    }
                    const int x = start_x[i] + (end_x[i] - start_x[i]) * frame / 10;
                    const int y = start_y[i] + (end_y[i] - start_y[i]) * frame / 10;
                    GuiField<int>(bubble, GuiObjectField::x) = MulDiv(x, width, 800);
                    GuiField<int>(bubble, GuiObjectField::y) = MulDiv(y, height, 600);
                }
            }
        }
        // A restart midway through a wobble must snapshot the live position,
        // rather than the original resource anchor or an already fitted point.
        GuiField<int>(bubbles[0].bytes, GuiObjectField::x) = MulDiv(native[0].x - 3, width, 800);
        GuiField<int>(bubbles[0].bytes, GuiObjectField::y) = MulDiv(native[0].y - 2, height, 600);
        UpdateTrainingBubbles(controller, width, height, lookup, [&](void*) {
            ok &= Expect(GuiField<int>(bubbles[0].bytes, GuiObjectField::x) == native[0].x - 3 &&
                GuiField<int>(bubbles[0].bytes, GuiObjectField::y) == native[0].y - 2,
                "animation restart lost the live native position");
        });
        ok &= Expect(GuiField<int>(bubbles[0].bytes, GuiObjectField::y) ==
            MulDiv(native[0].y - 2, height, 600), "restart did not restore fitted position");
        // Unregistered controls pass through without coordinate/anchor writes.
        const int old_x = GuiField<int>(bubbles[0].bytes, GuiObjectField::x);
        UpdateTrainingBubbles(controller, width, height,
            [](void*) -> const NativeGeometry* { return nullptr; }, [&](void*) {
                ok &= Expect(GuiField<int>(bubbles[0].bytes, GuiObjectField::x) == old_x,
                    "unregistered bubble was transformed");
            });
    }
    return ok ? 0 : 1;
}
