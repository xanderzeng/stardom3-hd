#include "ui_layout_registry.h"
#include "ui_layout_state.h"

#include <cstdint>
#include <cstdio>

namespace stardom {
void Log(const char*, ...) {}
}  // namespace stardom

namespace {
bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}
}  // namespace

int main() {
    using namespace stardom;
    bool ok = true;
    void* first = reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000));
    void* collision = reinterpret_cast<void*>(static_cast<uintptr_t>(
        0x10000 + UnifiedUILayoutState::processed_slot_count * 16));

    ok &= Expect(!IsProcessedLayoutObject(first), "new object not processed");
    RememberProcessedLayoutObject(first);
    RememberProcessedLayoutObject(collision);
    ok &= Expect(IsProcessedLayoutObject(first), "inserted object found");
    ok &= Expect(IsProcessedLayoutObject(collision), "collision found");
    ok &= Expect(g_unified_ui.processed_count == 2, "unique insert count");
    RememberProcessedLayoutObject(first);
    ok &= Expect(g_unified_ui.processed_count == 2, "duplicate insert count");

    for (size_t i = 2; i < UnifiedUILayoutState::processed_capacity; ++i) {
        RememberProcessedLayoutObject(reinterpret_cast<void*>(
            static_cast<uintptr_t>(0x200000 + i * 16)));
    }
    ok &= Expect(g_unified_ui.processed_count ==
        UnifiedUILayoutState::processed_capacity, "original capacity kept");
    return ok ? 0 : 1;
}
