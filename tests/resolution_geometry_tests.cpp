#include "display_config.h"
#include "ui_geometry.h"

#include <array>
#include <cstdio>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        ++failures;
    }
}

void CheckViewport(int output_width, int output_height,
                   int x, int y, int width, int height) {
    const stardom::RectI actual =
        stardom::AspectFitLegacyCanvas(output_width, output_height);
    Check(actual.x == x && actual.y == y &&
          actual.width == width && actual.height == height,
          "aspect-fit viewport mismatch");
}

}  // namespace

int main() {
    for (const auto& mode : stardom::kSupportedResolutions) {
        const auto result = stardom::ValidateResolution(mode.width, mode.height);
        Check(result.supported, "whitelisted resolution was rejected");
        Check(result.mode.width == mode.width && result.mode.height == mode.height,
              "validated resolution changed dimensions");
    }

    constexpr std::array<std::array<int, 2>, 7> rejected{{
        {{1366, 768}}, {{1920, 1200}}, {{2560, 1080}}, {{0, 0}},
        {{-1920, 1080}}, {{7680, 4320}}, {{800, 768}},
    }};
    for (const auto& dimensions : rejected) {
        Check(!stardom::ValidateResolution(
            dimensions[0], dimensions[1]).supported,
            "non-whitelisted resolution was accepted");
    }

    CheckViewport(1280, 720, 160, 0, 960, 720);
    CheckViewport(1920, 1080, 240, 0, 1440, 1080);
    CheckViewport(2560, 1440, 320, 0, 1920, 1440);
    CheckViewport(800, 600, 0, 0, 800, 600);
    CheckViewport(1024, 768, 0, 0, 1024, 768);
    CheckViewport(1600, 1200, 0, 0, 1600, 1200);

    Check(stardom::TransformAnchoredCoordinate(10, 20, 800, 1920) == 10,
          "left anchor moved");
    Check(stardom::TransformAnchoredCoordinate(390, 20, 800, 1920) == 950,
          "center anchor was not centered");
    Check(stardom::TransformAnchoredCoordinate(770, 20, 800, 1920) == 1890,
          "right anchor was not moved to the right edge");
    Check(stardom::TransformAnchoredCoordinate(950, 20, 800, 1920) == 950,
          "output-space coordinate was transformed twice");

    const auto world = stardom::TransformWorldCoordinate(
        390, 290, 20, 20, 1920, 1080);
    Check(world.x == 950 && world.y == 530,
          "world-space center transform mismatch");

    return failures == 0 ? 0 : 1;
}
