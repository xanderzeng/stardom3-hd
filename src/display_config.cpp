#include "display_config.h"

#include <cstdio>

namespace stardom {

ResolutionValidation ValidateResolution(int width, int height) {
    for (const auto& mode : kSupportedResolutions) {
        if (mode.width == width && mode.height == height) {
            return {true, mode};
        }
    }
    return {false, kDefaultResolution};
}

const char* AspectRatioName(AspectRatio aspect_ratio) {
    return aspect_ratio == AspectRatio::FourThree ? "4:3" : "16:9";
}

std::string SupportedResolutionList() {
    std::string result;
    char mode[32]{};
    for (const auto& resolution : kSupportedResolutions) {
        if (!result.empty()) {
            result += ", ";
        }
        std::snprintf(mode, sizeof(mode), "%dx%d",
                      resolution.width, resolution.height);
        result += mode;
    }
    return result;
}

}  // namespace stardom
