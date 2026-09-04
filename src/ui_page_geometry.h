#pragma once

#include <cstddef>

namespace stardom {

struct NativeGeometry {
    void* object = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

NativeGeometry* FindGeometry(NativeGeometry* geometries, size_t count,
                             void* object);
NativeGeometry* RememberGeometry(NativeGeometry* geometries, size_t capacity,
                                 size_t& count, void* object);
bool IsDescendantOf(void* object, void* root, int maximum_depth = 10);

// Only static aspect-fit pages belong here; animated/live-coordinate pages
// retain their specialised dispatch. Rules are evaluated root then descendants.
struct StaticPageMoveRule {
    void* root;
    NativeGeometry* geometries;
    size_t capacity;
    size_t* count;
};
using RepeatedPositionPredicate = bool (*)(void*, int, int);
bool ApplyStaticPageMove(const StaticPageMoveRule* rules, size_t rule_count,
                         void* object, void* parent, int screen_width,
                         int screen_height, int& x, int& y,
                         RepeatedPositionPredicate is_repeated);

// Company reports recenter their artist columns and animate their tab slots.
// Recognition must not depend on the initial resource's horizontal positions.
bool IsCompanyChartColumn(int x, int y, int width, int height);
bool IsCompanyTabButton(int x, int y, int width, int height);
int CorrectCompanyColumnCenter(int submitted_x, int fitted_width,
                               int native_width);

struct CompanyListMetrics {
    int left, top, right, bottom, font_size, line_gap;
};
CompanyListMetrics FitCompanyListMetrics(const CompanyListMetrics& native,
                                        int viewport_width, int viewport_height);
struct PageTextMetrics { int font_size, line_gap; };
PageTextMetrics FitPageTextMetrics(int font_size, int line_gap,
                                   int viewport_height);

}  // namespace stardom
