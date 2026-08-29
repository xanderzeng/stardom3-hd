#include "ui_geometry.h"

#include <algorithm>
#include <cstdint>

namespace stardom {

RectI AspectFitLegacyCanvas(int output_width, int output_height) {
    RectI viewport{};
    if (output_width <= 0 || output_height <= 0) {
        return viewport;
    }

    if (static_cast<std::int64_t>(output_width) * LegacyCanvas::height <=
        static_cast<std::int64_t>(output_height) * LegacyCanvas::width) {
        viewport.width = output_width;
        viewport.height = static_cast<int>(
            static_cast<std::int64_t>(viewport.width) * LegacyCanvas::height /
            LegacyCanvas::width);
    } else {
        viewport.height = output_height;
        viewport.width = static_cast<int>(
            static_cast<std::int64_t>(viewport.height) * LegacyCanvas::width /
            LegacyCanvas::height);
    }
    viewport.x = (output_width - viewport.width) / 2;
    viewport.y = (output_height - viewport.height) / 2;
    return viewport;
}

RectI AspectFitLegacyCanvas(const ResolutionMode& resolution) {
    return AspectFitLegacyCanvas(resolution.width, resolution.height);
}

int TransformAnchoredCoordinate(int position, int extent, int legacy_size,
                                int output_size) {
    if (output_size <= legacy_size || position < 0 || position > legacy_size) {
        return position;
    }

    const int center_twice = position * 2 + std::max(extent, 0);
    const int first_third_twice = (legacy_size * 2) / 3;
    const int second_third_twice = (legacy_size * 4) / 3;
    const int expansion = output_size - legacy_size;
    if (center_twice < first_third_twice) {
        return position;
    }
    if (center_twice > second_third_twice) {
        return position + expansion;
    }
    return position + expansion / 2;
}

PointI TransformWorldCoordinate(int x, int y, int width, int height,
                                int output_width, int output_height) {
    const int center_x_twice = x * 2 + std::max(width, 0);
    const int center_y_twice = y * 2 + std::max(height, 0);
    return {
        static_cast<int>((static_cast<std::int64_t>(center_x_twice) *
            output_width) / (LegacyCanvas::width * 2)) - width / 2,
        static_cast<int>((static_cast<std::int64_t>(center_y_twice) *
            output_height) / (LegacyCanvas::height * 2)) - height / 2,
    };
}

bool ResolveCenteredPageOverlayPosition(const RectI& current,
                                        const RectI& page_content,
                                        int output_width, int output_height,
                                        PointI& resolved) {
    if (current.width <= 0 || current.height <= 0 ||
        page_content.width <= 0 || page_content.height <= 0 ||
        output_width < LegacyCanvas::width ||
        output_height < LegacyCanvas::height) {
        return false;
    }

    const int center_x = (output_width - LegacyCanvas::width) / 2;
    const int center_y = (output_height - LegacyCanvas::height) / 2;
    const int expansion_x = output_width - LegacyCanvas::width;
    const int expansion_y = output_height - LegacyCanvas::height;
    const std::int64_t overlay_area =
        static_cast<std::int64_t>(current.width) * current.height;
    std::int64_t best_overlap = 0;
    PointI best_native{};

    const auto consider = [&](int native_x, int native_y) {
        if (native_x < 0 || native_x > LegacyCanvas::width ||
            native_y < 0 || native_y > LegacyCanvas::height) {
            return;
        }
        const int intersection_width = std::max(0,
            std::min(native_x + current.width,
                     page_content.x + page_content.width) -
            std::max(native_x, page_content.x));
        const int intersection_height = std::max(0,
            std::min(native_y + current.height,
                     page_content.y + page_content.height) -
            std::max(native_y, page_content.y));
        const std::int64_t overlap =
            static_cast<std::int64_t>(intersection_width) *
            intersection_height;
        if (overlap > best_overlap) {
            best_overlap = overlap;
            best_native = {native_x, native_y};
        }
    };

    // A coordinate pair can only be in one of the spaces the runtime emits:
    // authored legacy coordinates, the complete centered-page translation,
    // or the result of the generic edge-anchor transform. Do not combine
    // independently guessed x/y shifts; at smaller widescreen resolutions
    // those artificial pairs can overlap the same tall content region and
    // move an already-centered overlay a second time.
    consider(current.x, current.y);
    consider(current.x - center_x, current.y - center_y);

    const int x_shifts[] = {0, center_x, expansion_x};
    const int y_shifts[] = {0, center_y, expansion_y};
    for (const int x_shift : x_shifts) {
        for (const int y_shift : y_shifts) {
            const int native_x = current.x - x_shift;
            const int native_y = current.y - y_shift;
            if (TransformAnchoredCoordinate(
                    native_x, current.width, LegacyCanvas::width,
                    output_width) != current.x ||
                TransformAnchoredCoordinate(
                    native_y, current.height, LegacyCanvas::height,
                    output_height) != current.y) {
                continue;
            }
            consider(native_x, native_y);
        }
    }

    // Page-owned panels normally sit almost entirely over their owner's
    // content. Requiring two thirds prevents edge HUD elements which merely
    // touch a centered page from changing coordinate spaces.
    if (best_overlap * 3 < overlay_area * 2) {
        return false;
    }
    // Shallow roots recovered to the legacy canvas top edge are global HUD
    // strips, not page-owned overlays. A full 800x600 page otherwise makes
    // every top HUD appear fully contained (TodayDate is 336x35), causing the
    // page translation to move it from the output top-right into the center.
    // Taller popups and shallow controls located inside page content remain
    // eligible for the common ownership rule.
    if (current.height < 60 && best_native.y == 0) {
        return false;
    }
    resolved = {best_native.x + center_x, best_native.y + center_y};
    return true;
}

}  // namespace stardom
