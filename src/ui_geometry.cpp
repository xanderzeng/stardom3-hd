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

}  // namespace stardom
