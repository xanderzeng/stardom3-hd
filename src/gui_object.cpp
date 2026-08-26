#include "d3d9_proxy_internal.h"
#include "gui_object.h"

#include <cstdint>
#include <iterator>

namespace stardom {

namespace {

struct ReadableRegion {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

thread_local unsigned int g_read_batch_depth = 0;
thread_local size_t g_next_read_region = 0;
thread_local ReadableRegion g_read_regions[8]{};

}  // namespace

GuiObjectReadBatch::GuiObjectReadBatch() {
    if (g_read_batch_depth++ == 0) {
        g_next_read_region = 0;
        for (auto& region : g_read_regions) {
            region = {};
        }
    }
}

GuiObjectReadBatch::~GuiObjectReadBatch() {
    if (--g_read_batch_depth == 0) {
        for (auto& region : g_read_regions) {
            region = {};
        }
    }
}

bool CanReadGuiObject(const void* object) {
    if (!object) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(object);
    const uintptr_t required_end = start + GuiObjectField::readable_extent;
    if (required_end < start) {
        return false;
    }
    if (g_read_batch_depth != 0) {
        for (const auto& region : g_read_regions) {
            if (start >= region.begin && required_end <= region.end) {
                return true;
            }
        }
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(object, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) +
        info.RegionSize;
    if (end < required_end) {
        return false;
    }
    if (g_read_batch_depth != 0) {
        ReadableRegion& cached =
            g_read_regions[g_next_read_region++ % std::size(g_read_regions)];
        cached.begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
        cached.end = end;
    }
    return true;
}

}  // namespace stardom
