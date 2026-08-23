#include "d3d9_proxy_internal.h"
#include "gui_object.h"

#include <cstdint>

namespace stardom {

bool CanReadGuiObject(const void* object) {
    if (!object) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(object, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(object);
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) +
        info.RegionSize;
    return end >= start + GuiObjectField::readable_extent;
}

}  // namespace stardom
