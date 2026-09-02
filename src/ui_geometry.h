#pragma once

#include "display_config.h"

namespace stardom {

struct LegacyCanvas {
    static constexpr int width = 800;
    static constexpr int height = 600;
};

struct PointI {
    int x = 0;
    int y = 0;
};

struct RectI {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

RectI AspectFitLegacyCanvas(int output_width, int output_height);
RectI AspectFitLegacyCanvas(const ResolutionMode& resolution);
int TransformAnchoredCoordinate(int position, int extent, int legacy_size,
                                int output_size);
PointI TransformWorldCoordinate(int x, int y, int width, int height,
                                int output_width, int output_height);
int ResolveNativeAnimationCoordinate(int submitted_position,
                                     int current_scaled_position,
                                     int native_position,
                                     int maximum_increment = 16);
bool ResolveCenteredPageOverlayPosition(const RectI& current,
                                        const RectI& page_content,
                                        int output_width, int output_height,
                                        PointI& resolved);

}  // namespace stardom
