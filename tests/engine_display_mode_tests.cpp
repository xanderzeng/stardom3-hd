#include "engine_display_mode.h"
#include <cstdio>

int main() {
    stardom::EngineDisplayMode modes[] = {
        {2560, 1440, 170, 22, 0}, {800, 600, 170, 22, 1}, {1024, 768, 60, 22, 1}};
    bool ok = stardom::UpdateSelectedEngineDisplayMode(modes, 3, 1, 1920, 1080);
    ok &= modes[1].width == 1920 && modes[1].height == 1080;
    ok &= modes[1].refresh_rate == 170 && modes[1].format == 22 && modes[1].flags == 1;
    ok &= modes[0].width == 2560 && modes[2].width == 1024;
    // Center-screen mouse input must normalize to the same center as the
    // render viewport; the old 800-wide camera normalized x=960 to 1.2.
    ok &= 960.0 / modes[1].width == 0.5 && 540.0 / modes[1].height == 0.5;
    ok &= !stardom::UpdateSelectedEngineDisplayMode(modes, 3, 3, 1920, 1080);
    ok &= !stardom::UpdateSelectedEngineDisplayMode(modes, 3, 1, 0, 0);
    ok &= stardom::UpdateSelectedEngineDisplayMode(modes, 3, 1, 1024, 768);
    ok &= modes[1].width == 1024 && modes[1].height == 768;
    std::printf("Engine display mode: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
