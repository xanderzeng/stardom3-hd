#pragma once

namespace stardom {

// A logical GUI root remains 800x600 for layout discovery. Only the native
// movement constraint needs output bounds while accepting transformed pixels.
class ScopedGuiMoveBounds {
public:
    ScopedGuiMoveBounds(int& width, int& height, int output_width, int output_height)
        : width_(width), height_(height), saved_width_(width), saved_height_(height) {
        if (output_width > width_) width_ = output_width;
        if (output_height > height_) height_ = output_height;
    }
    ~ScopedGuiMoveBounds() {
        width_ = saved_width_;
        height_ = saved_height_;
    }
    ScopedGuiMoveBounds(const ScopedGuiMoveBounds&) = delete;
    ScopedGuiMoveBounds& operator=(const ScopedGuiMoveBounds&) = delete;
private:
    int& width_;
    int& height_;
    int saved_width_;
    int saved_height_;
};

}  // namespace stardom
