#include "gui_object.h"
#include "ui_page_geometry.h"

#include <cstdint>
#include <cstdio>
#include <iterator>

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

struct TestGuiObject {
    alignas(void*) unsigned char bytes[0x100]{};
};

void SetGeometry(TestGuiObject& object, int x, int y,
                 int width, int height) {
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::x) = x;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::y) = y;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::width) = width;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::height) = height;
}

}  // namespace

int main() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject first{};
    TestGuiObject second{};
    NativeGeometry geometries[2]{};
    size_t count = 0;

    SetGeometry(first, 10, 20, 30, 40);
    NativeGeometry* remembered = RememberGeometry(
        geometries, std::size(geometries), count, first.bytes);
    ok &= Expect(remembered && remembered->x == 10 && remembered->height == 40,
        "geometry must retain authored values");
    ok &= Expect(FindGeometry(geometries, count, first.bytes) == remembered,
        "cached geometry lookup must return the retained entry");

    count = 0;
    SetGeometry(second, 50, 60, 70, 80);
    NativeGeometry* replacement = RememberGeometry(
        geometries, std::size(geometries), count, second.bytes);
    ok &= Expect(replacement == &geometries[0] && replacement->x == 50,
        "reset array must invalidate an old lookup-cache entry");
    ok &= Expect(FindGeometry(geometries, count, first.bytes) == nullptr,
        "replaced object must not be returned from the lookup cache");

    GuiField<void*>(first.bytes, GuiObjectField::parent) = second.bytes;
    ok &= Expect(IsDescendantOf(first.bytes, second.bytes, 2),
        "valid parent chain must find its root");
    ok &= Expect(!IsDescendantOf(first.bytes, nullptr, 16),
        "missing root must be rejected before walking the parent chain");
    return ok ? 0 : 1;
}
