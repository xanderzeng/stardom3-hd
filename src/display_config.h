#pragma once

#include <array>
#include <string>

namespace stardom {

enum class AspectRatio {
    FourThree,
    SixteenNine,
};

struct ResolutionMode {
    int width;
    int height;
    AspectRatio aspect_ratio;
};

inline constexpr ResolutionMode kDefaultResolution{
    1920, 1080, AspectRatio::SixteenNine};

inline constexpr std::array<ResolutionMode, 7> kSupportedResolutions{{
    {1280, 720, AspectRatio::SixteenNine},
    {1920, 1080, AspectRatio::SixteenNine},
    {2560, 1440, AspectRatio::SixteenNine},
    {3840, 2160, AspectRatio::SixteenNine},
    {800, 600, AspectRatio::FourThree},
    {1024, 768, AspectRatio::FourThree},
    {1600, 1200, AspectRatio::FourThree},
}};

struct ResolutionValidation {
    bool supported = false;
    ResolutionMode mode = kDefaultResolution;
};

ResolutionValidation ValidateResolution(int width, int height);
const char* AspectRatioName(AspectRatio aspect_ratio);
std::string SupportedResolutionList();

}  // namespace stardom
