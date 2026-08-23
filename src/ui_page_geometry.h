#pragma once

#include <cstddef>

namespace stardom {

struct NativeGeometry {
    void* object = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

NativeGeometry* FindGeometry(NativeGeometry* geometries, size_t count,
                             void* object);
NativeGeometry* RememberGeometry(NativeGeometry* geometries, size_t capacity,
                                 size_t& count, void* object);
bool IsDescendantOf(void* object, void* root, int maximum_depth = 10);

}  // namespace stardom
