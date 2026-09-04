#include "ui_page_geometry.h"

#include "d3d9_proxy_internal.h"
#include "gui_object.h"

#include <cstdint>
#include <iterator>

namespace stardom {

void FitInventoryTargetPosition(int card_x, int card_y, int viewport_x,
        int viewport_y, int viewport_width, int viewport_height, int& x, int& y) {
    x = viewport_x + card_x + MulDiv(x - card_x, viewport_width, LegacyCanvas::width);
    y = viewport_y + card_y + MulDiv(y - card_y, viewport_height, LegacyCanvas::height);
}

bool IsInventoryTargetTree(void* object, void* primary_root) {
    if (!primary_root || !CanReadGuiObject(object) ||
        GuiPointer(object, GuiObjectField::parent) != primary_root ||
        GuiField<int>(object, GuiObjectField::width) != 370 ||
        GuiField<int>(object, GuiObjectField::height) != 90) return false;
    void* card = GuiPointer(object, GuiObjectField::first_child);
    if (!CanReadGuiObject(card) || GuiPointer(card, GuiObjectField::next_sibling) ||
        GuiField<int>(card, GuiObjectField::x) != 0 ||
        GuiField<int>(card, GuiObjectField::y) != 30 ||
        GuiField<int>(card, GuiObjectField::width) != 363 ||
        GuiField<int>(card, GuiObjectField::height) != 56) return false;
    unsigned stars = 0;
    bool give = false;
    int count = 0;
    void* child = GuiPointer(card, GuiObjectField::first_child);
    for (; CanReadGuiObject(child) && count < 6; ++count) {
        const int x = GuiField<int>(child, GuiObjectField::x);
        const int y = GuiField<int>(child, GuiObjectField::y);
        const int w = GuiField<int>(child, GuiObjectField::width);
        const int h = GuiField<int>(child, GuiObjectField::height);
        for (int i = 0; i < 4; ++i)
            if (x == 20 + i * 70 && y == -26 && w == 70 && h == 80) stars |= 1u << i;
        give |= x == 311 && y == 17 && w == 46 && h == 22;
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return !child && count == 5 && stars == 15 && give;
}

bool IsInventoryPageTree(void* object, void* primary_root) {
    if (!primary_root || !CanReadGuiObject(object) ||
        GuiPointer(object, GuiObjectField::parent) != primary_root ||
        GuiField<int>(object, GuiObjectField::width) != 800 ||
        GuiField<int>(object, GuiObjectField::height) != 600) return false;
    void* card = GuiPointer(object, GuiObjectField::first_child);
    if (!CanReadGuiObject(card) || GuiPointer(card, GuiObjectField::next_sibling) ||
        GuiField<int>(card, GuiObjectField::x) != 200 ||
        GuiField<int>(card, GuiObjectField::y) != 35 ||
        GuiField<int>(card, GuiObjectField::width) != 420 ||
        GuiField<int>(card, GuiObjectField::height) != 542) return false;
    unsigned rows = 0;
    int tabs = 0, count = 0;
    bool up = false, down = false, track = false, exit = false;
    void* child = GuiPointer(card, GuiObjectField::first_child);
    for (; CanReadGuiObject(child) && count < 16; ++count) {
        const int x = GuiField<int>(child, GuiObjectField::x);
        const int y = GuiField<int>(child, GuiObjectField::y);
        const int w = GuiField<int>(child, GuiObjectField::width);
        const int h = GuiField<int>(child, GuiObjectField::height);
        tabs += x >= 125 && x <= 344 && y == 12 && w == 40 && h == 40;
        for (int row = 0; row < 5; ++row)
            if (x == 0 && y == 65 + row * 85 &&
                w == (row == 4 ? 370 : 380) && h == 90) rows |= 1u << row;
        up |= x == 375 && y == 60 && w == 28 && h == 79;
        down |= x == 375 && y == 410 && w == 28 && h == 79;
        track |= x == 375 && y == 139 && w == 28 && h == 271;
        exit |= x == 283 && y == 500 && w == 98 && h == 28;
        child = GuiPointer(child, GuiObjectField::next_sibling);
    }
    return !child && count == 12 && tabs == 3 && rows == 31 &&
        up && down && track && exit;
}

PageTextMetrics FitPageTextMetrics(int font_size, int line_gap,
                                   int viewport_height) {
    const int font = MulDiv(font_size, viewport_height, LegacyCanvas::height);
    // Text labels have an implicit one-pixel inter-line gap (005307C8).
    return {font, MulDiv(font_size + line_gap + 1, viewport_height, LegacyCanvas::height) - font - 1};
}

int CorrectCompanyColumnCenter(int submitted_x, int fitted_width,
                               int native_width) {
    return submitted_x + fitted_width / 2 - native_width / 2;
}

CompanyListMetrics FitCompanyListMetrics(const CompanyListMetrics& native,
                                        int viewport_width, int viewport_height) {
    const int font = MulDiv(native.font_size, viewport_height, LegacyCanvas::height);
    // Round the complete baseline interval once so every row matches the
    // separately scaled month-board positions, with no accumulated drift.
    const int pitch = MulDiv(native.font_size + native.line_gap,
                             viewport_height, LegacyCanvas::height);
    return {MulDiv(native.left, viewport_width, LegacyCanvas::width),
            MulDiv(native.top, viewport_height, LegacyCanvas::height),
            MulDiv(native.right, viewport_width, LegacyCanvas::width),
            MulDiv(native.bottom, viewport_height, LegacyCanvas::height), font, pitch - font};
}

bool IsCompanyChartColumn(int x, int y, int width, int height) {
    return x >= 0 && x <= 374 && y >= 131 && y <= 135 &&
        width >= 68 && width <= 72 && height >= 235 && height <= 243;
}

bool IsCompanyTabButton(int x, int y, int width, int height) {
    return x >= 207 && x <= 545 && y >= 39 && y <= 43 &&
        width >= 38 && width <= 42 && height >= 38 && height <= 42;
}

namespace {

struct GeometryLookupCacheEntry {
    NativeGeometry* geometries = nullptr;
    void* object = nullptr;
    size_t index = 0;
};

thread_local GeometryLookupCacheEntry g_geometry_lookup_cache[128]{};

size_t GeometryLookupCacheSlot(NativeGeometry* geometries, void* object) {
    const uintptr_t geometry_key =
        reinterpret_cast<uintptr_t>(geometries) >> 4;
    const uintptr_t object_key = reinterpret_cast<uintptr_t>(object) >> 4;
    return static_cast<size_t>((geometry_key ^ object_key ^
        (object_key >> 11)) & (std::size(g_geometry_lookup_cache) - 1));
}

void CacheGeometryLookup(NativeGeometry* geometries, void* object,
                         size_t index) {
    g_geometry_lookup_cache[GeometryLookupCacheSlot(geometries, object)] =
        {geometries, object, index};
}

}  // namespace

NativeGeometry* FindGeometry(NativeGeometry* geometries, size_t count,
                             void* object) {
    if (!geometries || !object) {
        return nullptr;
    }
    GeometryLookupCacheEntry& cached =
        g_geometry_lookup_cache[GeometryLookupCacheSlot(geometries, object)];
    if (cached.geometries == geometries && cached.object == object &&
        cached.index < count && geometries[cached.index].object == object) {
        return &geometries[cached.index];
    }
    for (size_t i = 0; i < count; ++i) {
        if (geometries[i].object == object) {
            CacheGeometryLookup(geometries, object, i);
            return &geometries[i];
        }
    }
    return nullptr;
}

NativeGeometry* RememberGeometry(NativeGeometry* geometries, size_t capacity,
                                 size_t& count, void* object) {
    if (!CanReadGuiObject(object)) {
        return nullptr;
    }
    if (NativeGeometry* existing = FindGeometry(geometries, count, object)) {
        return existing;
    }
    if (count >= capacity) {
        return nullptr;
    }
    NativeGeometry& geometry = geometries[count++];
    geometry.object = object;
    geometry.x = GuiField<int>(object, GuiObjectField::x);
    geometry.y = GuiField<int>(object, GuiObjectField::y);
    geometry.width = GuiField<int>(object, GuiObjectField::width);
    geometry.height = GuiField<int>(object, GuiObjectField::height);
    CacheGeometryLookup(geometries, object, count - 1);
    return &geometry;
}

bool IsDescendantOf(void* object, void* root, int maximum_depth) {
    // Most specialised pages are absent during ordinary gameplay. Avoid
    // walking and validating the complete parent chain for every missing
    // page type from the high-frequency GuiMove hook.
    if (!object || !root) {
        return false;
    }
    void* cursor = object;
    for (int depth = 0; CanReadGuiObject(cursor) && depth < maximum_depth;
         ++depth) {
        if (cursor == root) {
            return true;
        }
        cursor = GuiPointer(cursor, GuiObjectField::parent);
    }
    return false;
}

namespace {

// Called only after dispatch has established root/descendant membership.
bool ApplyMatchedPageMove(const StaticPageMoveRule& rule, bool is_root,
                          void* object, int screen_width, int screen_height,
                          int& x, int& y, RepeatedPositionPredicate is_repeated) {
    if (is_root) {
        const auto [left, top, width, height] =
            AspectFitLegacyCanvas(screen_width, screen_height);
        GuiField<int>(object, GuiObjectField::width) = width;
        GuiField<int>(object, GuiObjectField::height) = height;
        x = left;
        y = top;
        return true;
    }
    if (is_repeated(object, x, y)) {
        return true;
    }
    NativeGeometry* native = RememberGeometry(
        rule.geometries, rule.capacity, *rule.count, object);
    if (!native) {
        return false;
    }
    const auto [left, top, width, height] =
        AspectFitLegacyCanvas(screen_width, screen_height);
    GuiField<int>(object, GuiObjectField::width) =
        MulDiv(native->width, width, LegacyCanvas::width);
    GuiField<int>(object, GuiObjectField::height) =
        MulDiv(native->height, height, LegacyCanvas::height);
    const bool submitted =
        rule.coordinates == PageMoveCoordinates::SubmittedNative;
    x = MulDiv(submitted ? x : native->x, width, LegacyCanvas::width);
    y = MulDiv(submitted ? y : native->y, height, LegacyCanvas::height);
    return true;
}

}  // namespace

bool ApplyStaticPageMove(const StaticPageMoveRule* rules, size_t rule_count,
                         void* object, void* parent, int screen_width,
                         int screen_height, int& x, int& y,
                         RepeatedPositionPredicate is_repeated) {
    if (!object) {
        return false;
    }
    for (size_t i = 0; i < rule_count; ++i) {
        const auto& rule = rules[i];
        const bool is_root = object == rule.root;
        if (!is_root && (!parent || !IsDescendantOf(
                object, rule.root, rule.maximum_ancestor_depth))) {
            continue;
        }
        if (ApplyMatchedPageMove(rule, is_root, object, screen_width,
                                  screen_height, x, y, is_repeated)) {
            return true;
        }
        // Preserve cache-exhaustion fallthrough into later independent rules.
    }
    return false;
}

bool ApplyPageGroupMove(const StaticPageMoveRule& shared_rule,
                        void* const* roots, size_t root_count,
                        void* object, void* parent, int screen_width,
                        int screen_height, int& x, int& y,
                        RepeatedPositionPredicate is_repeated) {
    if (!object) {
        return false;
    }
    for (size_t i = 0; i < root_count; ++i) {
        if (object == roots[i]) {
            return ApplyMatchedPageMove(shared_rule, true, object, screen_width,
                                         screen_height, x, y, is_repeated);
        }
    }
    if (parent) {
        for (size_t i = 0; i < root_count; ++i) {
            if (IsDescendantOf(object, roots[i],
                               shared_rule.maximum_ancestor_depth)) {
                // All group members share the same cache and coordinate policy.
                // On exhaustion the caller's next specialised branch must run.
                return ApplyMatchedPageMove(shared_rule, false, object,
                    screen_width, screen_height, x, y, is_repeated);
            }
        }
    }
    return false;
}

const StaticPageReflowRule* ReflowStaticPage(
    const StaticPageReflowRule* rules, size_t rule_count,
    void* object, void* parent, int width, int height,
    void (*remember_root)(void*), void (*mark_processed)(void*),
    bool& discovered) {
    discovered = false;
    if (!object) {
        return nullptr;
    }
    for (size_t i = 0; i < rule_count; ++i) {
        const auto& rule = rules[i];
        if (object != rule.root) {
            if (!rule.matches(object, parent, width, height)) {
                continue;
            }
            rule.reset(object);
            remember_root(object);
            discovered = true;
        }
        if (discovered || rule.refresh == PageRefreshPolicy::ReapplySubtree) {
            rule.scale(object, 0);
        }
        mark_processed(object);
        return &rule;
    }
    return nullptr;
}

}  // namespace stardom
