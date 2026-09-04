#include "ui_page_geometry.h"

#include "d3d9_proxy_internal.h"
#include "gui_object.h"

#include <cstdint>
#include <iterator>

namespace stardom {

PageTextMetrics FitPageTextMetrics(int font_size, int line_gap,
                                   int viewport_height) {
    const int font = MulDiv(font_size, viewport_height, 600);
    // Text labels have an implicit one-pixel inter-line gap (005307C8).
    return {font, MulDiv(font_size + line_gap + 1, viewport_height, 600) - font - 1};
}

int CorrectCompanyColumnCenter(int submitted_x, int fitted_width,
                               int native_width) {
    return submitted_x + fitted_width / 2 - native_width / 2;
}

CompanyListMetrics FitCompanyListMetrics(const CompanyListMetrics& native,
                                        int viewport_width, int viewport_height) {
    const int font = MulDiv(native.font_size, viewport_height, 600);
    // Round the complete baseline interval once so every row matches the
    // separately scaled month-board positions, with no accumulated drift.
    const int pitch = MulDiv(native.font_size + native.line_gap,
                             viewport_height, 600);
    return {MulDiv(native.left, viewport_width, 800),
            MulDiv(native.top, viewport_height, 600),
            MulDiv(native.right, viewport_width, 800),
            MulDiv(native.bottom, viewport_height, 600), font, pitch - font};
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
