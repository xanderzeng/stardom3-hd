#include "ui_page_geometry.h"

#include "d3d9_proxy_internal.h"
#include "gui_object.h"

#include <cstdint>
#include <iterator>

namespace stardom {

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

}  // namespace stardom
