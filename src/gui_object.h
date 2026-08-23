#pragma once

#include <cstddef>

namespace stardom {

struct GuiObjectField {
    static constexpr size_t x = 0x80;
    static constexpr size_t y = 0x84;
    static constexpr size_t width = 0x88;
    static constexpr size_t height = 0x8C;
    static constexpr size_t visible = 0x99;
    static constexpr size_t parent = 0xF0;
    static constexpr size_t first_child = 0xF4;
    static constexpr size_t next_sibling = 0xF8;
    static constexpr size_t readable_extent = 0xFC;
};

template <typename T>
T& GuiField(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

template <typename T>
const T& GuiField(const void* object, size_t offset) {
    return *reinterpret_cast<const T*>(
        static_cast<const unsigned char*>(object) + offset);
}

inline void* GuiPointer(void* object, size_t offset) {
    return GuiField<void*>(object, offset);
}

}  // namespace stardom
