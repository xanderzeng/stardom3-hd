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
    const auto text_native = FitPageTextMetrics(12, 0, 600);
    const auto text_hd = FitPageTextMetrics(12, 0, 1080);
    const auto text_multiline = FitPageTextMetrics(16, 3, 1080);
    ok &= Expect(text_native.font_size == 12 && text_native.line_gap == 0,
        "native page font and gap must remain unchanged");
    ok &= Expect(text_hd.font_size == 22 && text_hd.line_gap == 0 &&
                 text_multiline.font_size + text_multiline.line_gap + 1 == 36,
        "fitted text must scale glyphs and the complete baseline pitch");
    ok &= Expect(CorrectCompanyColumnCenter(66, 126, 70) == 94 &&
                 CorrectCompanyColumnCenter(258, 126, 70) == 286 &&
                 CorrectCompanyColumnCenter(94, 70, 70) == 94,
        "undo fitted half-width without changing native column spacing");
    const CompanyListMetrics list_native{0, 7, 299, 378, 12, 8};
    const auto list_fitted = FitCompanyListMetrics(list_native, 1440, 1080);
    ok &= Expect(list_fitted.top == 13 && list_fitted.right == 538 &&
                 list_fitted.bottom == 680 && list_fitted.font_size == 22 &&
                 list_fitted.line_gap == 14,
        "list text bounds and glyphs must fit with the background");
    ok &= Expect(list_fitted.font_size + list_fitted.line_gap == 36 &&
                 (list_fitted.bottom - list_fitted.top + list_fitted.line_gap + 1) /
                     (list_fitted.font_size + list_fitted.line_gap) == 18 &&
                 list_native.font_size == 12 && list_native.line_gap == 8,
        "render fit preserves visible rows and leaves logical metrics native");
    for (int x : {66, 129, 194, 258, 320, 95, 159, 223, 287, 351}) {
        ok &= Expect(IsCompanyChartColumn(x, 133, 70, 241),
            "recentered and resource chart columns must both match");
    }
    ok &= Expect(IsCompanyChartColumn(66, 133, 70, 237),
        "first artist column has a shorter portrait container");
    ok &= Expect(!IsCompanyChartColumn(25, 137, 43, 160) &&
                 !IsCompanyChartColumn(119, 239, 126, 427),
        "axis and already fitted geometry must not match native columns");
    for (int x : {209, 219, 255, 265, 311, 357, 403, 405, 451, 497, 543}) {
        ok &= Expect(IsCompanyTabButton(x, 41, 40, 40),
            "all tab selections and interpolated slots must be recognized");
    }
    ok &= Expect(!IsCompanyTabButton(498, 526, 92, 32) &&
                 !IsCompanyTabButton(394, 74, 72, 72),
        "exit and fitted buttons must not match native tabs");
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
