#include "ui_page_geometry.h"

#include "d3d9_proxy_internal.h"
#include "gui_object.h"

namespace stardom {

NativeGeometry* FindGeometry(NativeGeometry* geometries, size_t count,
                             void* object) {
    for (size_t i = 0; i < count; ++i) {
        if (geometries[i].object == object) {
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
    return &geometry;
}

bool IsDescendantOf(void* object, void* root, int maximum_depth) {
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
