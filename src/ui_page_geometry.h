#pragma once

#include <cstddef>
#include <type_traits>

namespace stardom {

// Most roots only reset their cache (void); bounded multi-root registries can
// refuse a new root (bool). The subtree must stop before any writes on failure.
template <typename PrepareRoot>
bool PrepareAspectFitRoot(PrepareRoot prepare, void* object) {
    if constexpr (std::is_same_v<decltype(prepare(object)), bool>) {
        return prepare(object);
    } else {
        static_assert(std::is_same_v<decltype(prepare(object)), void>,
                      "Root preparation must return void or bool");
        prepare(object);
        return true;
    }
}

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

// Standard aspect-fit roots only. Special per-control animation transforms
// remain outside this dispatcher. Rules run root then descendants in order.
enum class PageMoveCoordinates { CachedNative, SubmittedNative };
struct StaticPageMoveRule {
    void* root;
    NativeGeometry* geometries;
    size_t capacity;
    size_t* count;
    int maximum_ancestor_depth = 10;
    PageMoveCoordinates coordinates = PageMoveCoordinates::CachedNative;
};
using RepeatedPositionPredicate = bool (*)(void*, int, int);
enum class PageRefreshPolicy { ReapplySubtree, PreserveControllerState };
struct StaticPageReflowRule {
    void* root;
    bool (*matches)(void*, void*, int, int);
    void (*reset)(void*);
    void (*scale)(void*, int);
    const char* name;
    PageRefreshPolicy refresh = PageRefreshPolicy::ReapplySubtree;
};
// Known-root refresh skips discovery/reset. A new match is reset and registered
// before scaling. Controller-owned pages must not replay their cached subtree
// on refresh. Both paths still mark the root processed.
const StaticPageReflowRule* ReflowStaticPage(
    const StaticPageReflowRule* rules, size_t rule_count,
    void* object, void* parent, int width, int height,
    void (*remember_root)(void*), void (*mark_processed)(void*),
    bool& discovered);

bool ApplyStaticPageMove(const StaticPageMoveRule* rules, size_t rule_count,
                         void* object, void* parent, int screen_width,
                         int screen_height, int& x, int& y,
                         RepeatedPositionPredicate is_repeated);

// Coexisting pages may share one geometry cache. Unlike independent rules,
// every group root must be checked before any descendant match.
bool ApplyPageGroupMove(const StaticPageMoveRule& shared_rule,
                        void* const* roots, size_t root_count,
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
