#include "gui_move_bounds.h"
#include <cstdio>

// Reproduce the native GuiMove upper-edge constraint, including its negative
// result when a fitted child is larger than its logical parent.
int NativeClamp(int position, int child, int parent) {
    if (position < 0) return 0;
    return position + child >= parent ? parent - child : position;
}

int main() {
    int width = 800, height = 600;
    bool ok = NativeClamp(240, 1440, width) == -640 &&
        NativeClamp(0, 1080, height) == -480;
    {
        stardom::ScopedGuiMoveBounds bounds(width, height, 1920, 1080);
        ok &= NativeClamp(240, 1440, width) == 240;
        ok &= NativeClamp(0, 1080, height) == 0;
        ok &= NativeClamp(1810, 92, width) == 1810;
        {
            stardom::ScopedGuiMoveBounds nested(width, height, 1280, 720);
            ok &= width == 1920 && height == 1080;
        }
        ok &= width == 1920 && height == 1080;
    }
    ok &= width == 800 && height == 600;
    {
        stardom::ScopedGuiMoveBounds bounds(width, height, 1024, 768);
        ok &= NativeClamp(0, 1024, width) == 0 && NativeClamp(0, 768, height) == 0;
    }
    ok &= width == 800 && height == 600;
    std::printf("GUI native movement bounds: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
