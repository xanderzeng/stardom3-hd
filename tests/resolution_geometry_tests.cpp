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

    const stardom::RectI page_content{260, 70, 530, 378};
    stardom::PointI overlay{};
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {407, 221, 386, 163}, page_content, 1920, 1080, overlay) &&
              overlay.x == 967 && overlay.y == 461,
          "native page overlay was not centered");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {1527, 461, 386, 163}, page_content, 1920, 1080, overlay) &&
              overlay.x == 967 && overlay.y == 461,
          "right-anchored page overlay was not recovered");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {967, 461, 386, 163}, page_content, 1920, 1080, overlay) &&
              overlay.x == 967 && overlay.y == 461,
          "centered page overlay transform was not idempotent");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {647, 281, 386, 163}, page_content, 1280, 720, overlay) &&
              overlay.x == 647 && overlay.y == 281,
          "centered page overlay was not idempotent at 1280x720");
    Check(!stardom::ResolveCenteredPageOverlayPosition(
              {1584, 0, 336, 35}, page_content, 1920, 1080, overlay),
          "top-right HUD was mistaken for a page overlay");
    Check(!stardom::ResolveCenteredPageOverlayPosition(
              {1584, 0, 336, 35}, {0, 0, 800, 600},
              1920, 1080, overlay),
          "top-right HUD was mistaken for a full-page overlay");
    Check(!stardom::ResolveCenteredPageOverlayPosition(
              {1024, 240, 336, 35}, {0, 0, 800, 600},
              1920, 1080, overlay),
          "miscentered top HUD was accepted as a page overlay");

    // The schedule exposes a populated full-canvas child and stages its
    // detached picker with native X but already-centered Y. Recovering the
    // authored pair must still produce one complete centered translation.
    const stardom::RectI full_page_content{0, 0, 800, 600};
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {148, 407, 222, 381}, full_page_content,
              1920, 1080, overlay) &&
              overlay.x == 708 && overlay.y == 407,
          "mixed-space schedule overlay was not centered");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {708, 407, 222, 381}, full_page_content,
              1920, 1080, overlay) &&
              overlay.x == 708 && overlay.y == 407,
          "centered full-page overlay transform was not idempotent");

    // Compact dropdowns are independent roots too. This producer list was
    // first edge-anchored to the expanded right side even though its authored
    // rectangle is fully owned by the active centered page.
    const stardom::RectI notice_page_content{135, 85, 530, 423};
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {1635, 398, 112, 90}, notice_page_content,
              1920, 1080, overlay) &&
              overlay.x == 1075 && overlay.y == 398,
          "right-anchored producer dropdown was not recovered");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {1075, 398, 112, 90}, notice_page_content,
              1920, 1080, overlay) &&
              overlay.x == 1075 && overlay.y == 398,
          "centered producer dropdown transform was not idempotent");

    // Page filter strips are shallow detached roots. They use the same
    // authored coordinate space as larger popups and must receive the full
    // centered-page translation even though their height is below 60px.
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {165, 381, 330, 45}, notice_page_content,
              1920, 1080, overlay) &&
              overlay.x == 725 && overlay.y == 621,
          "shallow page filter strip was not centered");
    Check(stardom::ResolveCenteredPageOverlayPosition(
              {725, 621, 330, 45}, notice_page_content,
              1920, 1080, overlay) &&
              overlay.x == 725 && overlay.y == 621,
          "centered page filter strip transform was not idempotent");

    return failures == 0 ? 0 : 1;
}
