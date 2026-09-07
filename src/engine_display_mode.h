#pragma once
#include <cstddef>
#include <cstdint>

namespace stardom {
struct EngineDisplayMode {
    std::uint32_t width, height, refresh_rate, format, flags;
};
static_assert(sizeof(EngineDisplayMode) == 20);

inline bool UpdateSelectedEngineDisplayMode(EngineDisplayMode* modes,
    std::size_t count, std::size_t selected, unsigned width, unsigned height) {
    if (!modes || selected >= count || width < 800 || height < 600) return false;
    modes[selected].width = width;
    modes[selected].height = height;
    return true;
}
}
