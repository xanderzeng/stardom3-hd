#include "gui_object.h"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace {
template <typename T>
bool CheckField(size_t offset, T initial, T replacement) {
    alignas(void*) unsigned char bytes[0x100];
    unsigned char expected[sizeof(bytes)];
    std::memset(bytes, 0xA5, sizeof(bytes));
    std::memcpy(bytes + offset, &initial, sizeof(T));
    std::memcpy(expected, bytes, sizeof(bytes));
    const void* view = bytes;
    bool ok = stardom::GuiField<T>(view, offset) == initial;
    ok &= reinterpret_cast<const unsigned char*>(
        &stardom::GuiField<T>(view, offset)) == bytes + offset;
    stardom::GuiField<T>(bytes, offset) = replacement;
    std::memcpy(expected + offset, &replacement, sizeof(T));
    ok &= std::memcmp(bytes, expected, sizeof(bytes)) == 0;
    ok &= stardom::GuiField<T>(view, offset) == replacement;
    if (!ok) std::fprintf(stderr, "FAIL: GUI field at 0x%zx\n", offset);
    return ok;
}
}  // namespace

int main() {
    using namespace stardom;
    static_assert(std::is_same_v<decltype(GuiField<int>(
        static_cast<void*>(nullptr), 0)), int&>);
    static_assert(std::is_same_v<decltype(GuiField<int>(
        static_cast<const void*>(nullptr), 0)), const int&>);
    static_assert(GuiObjectField::x == 0x80 && GuiObjectField::y == 0x84 &&
        GuiObjectField::width == 0x88 && GuiObjectField::height == 0x8C &&
        GuiObjectField::visible == 0x99 && GuiObjectField::parent == 0xF0 &&
        GuiObjectField::first_child == 0xF4 && GuiObjectField::next_sibling == 0xF8);
    bool ok = true;
    ok &= CheckField<int>(GuiObjectField::x, -137, 1440);
    ok &= CheckField<int>(GuiObjectField::y, -600, 1080);
    ok &= CheckField<int>(GuiObjectField::width, 800, 1920);
    ok &= CheckField<int>(GuiObjectField::height, 600, 1080);
    ok &= CheckField<unsigned char>(GuiObjectField::visible, 1, 0);
    int first = 0, second = 0;
    ok &= CheckField<void*>(GuiObjectField::parent, &first, &second);
    ok &= CheckField<void*>(GuiObjectField::first_child, nullptr, &first);
    ok &= CheckField<void*>(GuiObjectField::next_sibling, &second, nullptr);
    return ok ? 0 : 1;
}
