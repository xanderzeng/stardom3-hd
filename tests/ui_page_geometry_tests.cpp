#include "gui_object.h"
#include "ui_page_geometry.h"
#include "ui_dispatch.h"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

struct TestGuiObject {
    alignas(void*) unsigned char bytes[0x100]{};
};

void SetGeometry(TestGuiObject& object, int x, int y,
                 int width, int height) {
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::x) = x;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::y) = y;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::width) = width;
    stardom::GuiField<int>(object.bytes, stardom::GuiObjectField::height) = height;
}

bool repeated_position = false;
int repeated_checks = 0;
bool IsRepeatedForTest(void*, int, int) {
    ++repeated_checks;
    return repeated_position;
}

std::string reflow_events;
void* reflow_candidate = nullptr;
void* reflow_parent = nullptr;
int reflow_page = 0;
int reflow_width = 1920, reflow_height = 1080;
void* reflow_roots[5]{};
TestGuiObject reflow_child{};
stardom::NativeGeometry reflow_cache[5][1]{};
size_t reflow_counts[5]{};
template<int Page>
bool MatchReflow(void* object, void* parent, int width, int height) {
    reflow_events += 'a' + Page;
    return Page == reflow_page && object == reflow_candidate &&
        parent == reflow_parent && width == 800 && height == 600;
}
template<int Page>
void ResetReflow(void* object) {
    reflow_events += 'R';
    reflow_roots[Page] = object;
    reflow_counts[Page] = 0;
}
void RegisterReflow(void*) { reflow_events += 'L'; }
void MarkReflow(void*) { reflow_events += 'P'; }
template<int Page>
void ScaleReflow(void* object, int depth) {
    reflow_events += depth == 0 ? 'S' : '!';
    // Exercise the real geometry dispatcher with a cached child, modelling the
    // unchanged subtree callback. GUI resource recognition is tested in-game.
    const stardom::StaticPageMoveRule rule{
        object, reflow_cache[Page], 1, &reflow_counts[Page]};
    for (void* node : {object, static_cast<void*>(reflow_child.bytes)}) {
        int x = -1, y = -1;
        stardom::ApplyStaticPageMove(&rule, 1, node, object,
            reflow_width, reflow_height, x, y, IsRepeatedForTest);
        stardom::GuiField<int>(node, stardom::GuiObjectField::x) = x;
        stardom::GuiField<int>(node, stardom::GuiObjectField::y) = y;
    }
}

bool TestStaticPageReflow() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject parent{}, first{}, reopened{}, unrelated{};
    reflow_parent = parent.bytes;
    repeated_position = false;
    for (int page = 0; page < 5; ++page) {
        for (auto& root : reflow_roots) root = nullptr;
        reflow_page = page;
        reflow_width = 1920; reflow_height = 1080;
        auto run = [&](void* object, int width, int height, bool& discovered) {
            const StaticPageReflowRule rules[] = {
                {reflow_roots[0], MatchReflow<0>, ResetReflow<0>, ScaleReflow<0>, "tutorial"},
                {reflow_roots[1], MatchReflow<1>, ResetReflow<1>, ScaleReflow<1>, "question"},
                {reflow_roots[2], MatchReflow<2>, ResetReflow<2>, ScaleReflow<2>, "profile"},
                {reflow_roots[3], MatchReflow<3>, ResetReflow<3>, ScaleReflow<3>, "contract"},
                {reflow_roots[4], MatchReflow<4>, ResetReflow<4>, ScaleReflow<4>, "signing"},
            };
            reflow_events.clear();
            return ReflowStaticPage(rules, std::size(rules), object, parent.bytes, width, height,
                RegisterReflow, MarkReflow, discovered) != nullptr;
        };
        reflow_candidate = first.bytes;
        GuiField<void*>(reflow_child.bytes, GuiObjectField::parent) = first.bytes;
        SetGeometry(reflow_child, 100, 50, 80, 40);
        bool discovered = false;
        const std::string preceding = std::string("abcde").substr(0, page);
        ok &= Expect(run(first.bytes, 800, 600, discovered) && discovered &&
                     reflow_events == preceding + char('a' + page) + "RLSP" &&
                     reflow_counts[page] == 1 &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::x) == 180,
            "first discovery resets/registers/scales/marks each page in order");
        ok &= Expect(run(first.bytes, 1440, 1080, discovered) && !discovered &&
                     reflow_events == preceding + "SP" &&
                     reflow_cache[page][0].width == 80 &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::width) == 144,
            "known-root refresh skips recognition/reset and preserves native cache");
        reflow_width = 800; reflow_height = 600;
        ok &= Expect(run(first.bytes, 1440, 1080, discovered) && !discovered &&
                     GuiField<int>(first.bytes, GuiObjectField::width) == 800 &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::x) == 100 &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::width) == 80,
            "resolution refresh restores native geometry without cumulative scaling");
        reflow_candidate = reopened.bytes;
        GuiField<void*>(reflow_child.bytes, GuiObjectField::parent) = reopened.bytes;
        SetGeometry(reflow_child, 200, 60, 90, 50);
        ok &= Expect(run(reopened.bytes, 800, 600, discovered) && discovered &&
                     reflow_roots[page] == reopened.bytes && reflow_counts[page] == 1 &&
                     reflow_cache[page][0].x == 200,
            "reopened replacement root resets stale geometry before scaling");
        ok &= Expect(!run(unrelated.bytes, 800, 600, discovered) && !discovered &&
                     reflow_events == "abcde",
            "unregistered special pages fall through without layout callbacks");
        ok &= Expect(!run(nullptr, 800, 600, discovered) && !discovered &&
                     reflow_events.empty(), "null roots do not dispatch");
    }
    // A recognisable earlier rule still precedes a later known-root match.
    reflow_page = 0;
    reflow_candidate = reopened.bytes;
    const StaticPageReflowRule overlap[] = {
        {nullptr, MatchReflow<0>, ResetReflow<0>, ScaleReflow<0>, "first"},
        {reopened.bytes, MatchReflow<1>, ResetReflow<1>, ScaleReflow<1>, "second"},
    };
    reflow_events.clear();
    bool discovered = false;
    ok &= Expect(ReflowStaticPage(overlap, 2, reopened.bytes, parent.bytes, 800, 600,
                     RegisterReflow, MarkReflow, discovered) == &overlap[0] &&
                 discovered && reflow_events == "aRLSP",
        "reflow preserves per-rule known/discovery priority, not all known roots first");
    return ok;
}

bool TestStaticPageMoves() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject roots[3]{}, children[3]{}, unrelated{};
    NativeGeometry caches[3][2]{};
    size_t counts[3]{};
    StaticPageMoveRule rules[3]{};
    for (size_t i = 0; i < 3; ++i) {
        rules[i] = {roots[i].bytes, caches[i], 2, &counts[i]};
        SetGeometry(children[i], 100, 50, 80, 40);
        GuiField<void*>(children[i].bytes, GuiObjectField::parent) = roots[i].bytes;
    }
    auto move = [&](void* object, void* parent, int& x, int& y) {
        return ApplyStaticPageMove(rules, 3, object, parent, 1920, 1080,
                                   x, y, IsRepeatedForTest);
    };
    for (size_t i = 0; i < 3; ++i) {
        int x = 999, y = 888;
        repeated_position = true;
        repeated_checks = 0;
        ok &= Expect(move(roots[i].bytes, nullptr, x, y) &&
                     x == 240 && y == 0 && repeated_checks == 0 &&
                     GuiField<int>(roots[i].bytes, GuiObjectField::width) == 1440 &&
                     GuiField<int>(roots[i].bytes, GuiObjectField::height) == 1080 &&
                     counts[i] == 0,
            "each page root fits before repeated-position or descendant handling");
        repeated_position = false;
        x = 999; y = 888;
        ok &= Expect(move(children[i].bytes, roots[i].bytes, x, y) &&
                     x == 180 && y == 90 && counts[i] == 1 &&
                     GuiField<int>(children[i].bytes, GuiObjectField::width) == 144 &&
                     GuiField<int>(children[i].bytes, GuiObjectField::height) == 72,
            "each page descendant scales authored local geometry, not submitted coordinates");
        x = 777; y = 666;
        ok &= Expect(move(children[i].bytes, roots[i].bytes, x, y) &&
                     x == 180 && y == 90 && counts[i] == 1 &&
                     caches[i][0].width == 80,
            "subsequent moves reuse native cache without cumulative scaling");
        repeated_position = true;
        SetGeometry(children[i], 180, 90, 123, 456);
        ok &= Expect(move(children[i].bytes, roots[i].bytes, x, y) &&
                     x == 180 && y == 90 && counts[i] == 1 &&
                     GuiField<int>(children[i].bytes, GuiObjectField::width) == 123,
            "repeated moves bypass both cache updates and resizing");
    }
    repeated_position = false;
    int x = 7, y = 9;
    repeated_checks = 0;
    ok &= Expect(!move(unrelated.bytes, nullptr, x, y) &&
                 !move(unrelated.bytes, roots[0].bytes, x, y) &&
                 !move(children[0].bytes, nullptr, x, y) &&
                 !move(nullptr, nullptr, x, y) &&
                 x == 7 && y == 9 && repeated_checks == 0,
        "unregistered/special pages and parentless descendants fall through untouched");

    // An earlier page's descendant must win over a later page's root.
    GuiField<void*>(roots[1].bytes, GuiObjectField::parent) = roots[0].bytes;
    SetGeometry(roots[1], 20, 30, 40, 50);
    ok &= Expect(move(roots[1].bytes, roots[0].bytes, x, y) &&
                 x == 36 && y == 54 && counts[0] == 2,
        "dispatch keeps per-rule root/descendant ordering");
    counts[0] = 0;
    rules[0].capacity = 0;
    ok &= Expect(move(roots[1].bytes, roots[0].bytes, x, y) &&
                 x == 240 && y == 0 && counts[0] == 0,
        "exhausted cache falls through to the next matching rule");
    x = 7; y = 9;
    SetGeometry(children[0], 100, 50, 80, 40);
    ok &= Expect(!move(children[0].bytes, roots[0].bytes, x, y) &&
                 x == 7 && y == 9 &&
                 GuiField<int>(children[0].bytes, GuiObjectField::width) == 80,
        "exhausted cache with no later match leaves original move untouched");
    repeated_position = true;
    ok &= Expect(move(children[0].bytes, roots[0].bytes, x, y) &&
                 counts[0] == 0 && x == 7 && y == 9,
        "repeated moves still succeed when the geometry cache is full");
    repeated_position = false;
    rules[0].root = nullptr;
    repeated_checks = 0;
    ok &= Expect(!move(children[0].bytes, roots[0].bytes, x, y) && repeated_checks == 0,
        "absent page roots cannot claim another page's descendants");
    return ok;
}

bool TestStaticPageAncestorLimits() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject chain[11]{};
    for (size_t i = 1; i < std::size(chain); ++i) {
        GuiField<void*>(chain[i].bytes, GuiObjectField::parent) = chain[i - 1].bytes;
    }
    NativeGeometry cache[1]{};
    size_t count = 0;
    StaticPageMoveRule rule{chain[0].bytes, cache, 1, &count};
    repeated_position = false;
    // The depth includes the object itself: limit 6 accepts five parent links,
    // not six. Test the old default (10) as well as the question page's limit.
    for (int limit : {6, 8, 10}) {
        rule.maximum_ancestor_depth = limit;
        count = 0;
        SetGeometry(chain[limit - 1], 100, 50, 80, 40);
        int x = 1, y = 2;
        ok &= Expect(ApplyStaticPageMove(&rule, 1, chain[limit - 1].bytes,
                         chain[limit - 2].bytes, 1920, 1080, x, y,
                         IsRepeatedForTest) && x == 180 && y == 90 && count == 1,
            "last permitted ancestor level remains fitted");
        SetGeometry(chain[limit], 100, 50, 80, 40);
        x = 1; y = 2;
        repeated_checks = 0;
        ok &= Expect(!ApplyStaticPageMove(&rule, 1, chain[limit].bytes,
                         chain[limit - 1].bytes, 1920, 1080, x, y,
                         IsRepeatedForTest) && x == 1 && y == 2 &&
                     repeated_checks == 0 && count == 1 &&
                     GuiField<int>(chain[limit].bytes, GuiObjectField::width) == 80,
            "first excluded ancestor level falls through without geometry changes");
    }
    // A short-depth miss must not prevent a later broad rule from matching.
    NativeGeometry later_cache[1]{};
    size_t later_count = 0;
    const StaticPageMoveRule overlap[] = {
        {chain[0].bytes, cache, 1, &count, 6},
        {chain[0].bytes, later_cache, 1, &later_count, 10},
    };
    SetGeometry(chain[6], 100, 50, 80, 40);
    int x = 1, y = 2;
    ok &= Expect(ApplyStaticPageMove(overlap, 2, chain[6].bytes, chain[5].bytes,
                     1920, 1080, x, y, IsRepeatedForTest) &&
                 later_count == 1 && x == 180 && y == 90,
        "ancestor-limit miss preserves later-rule dispatch");
    return ok;
}

bool TestSubmittedPageMoves() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject root{}, child{};
    NativeGeometry cache[1]{};
    size_t count = 0;
    const StaticPageMoveRule rule{root.bytes, cache, 1, &count, 8,
                                  PageMoveCoordinates::SubmittedNative};
    GuiField<void*>(child.bytes, GuiObjectField::parent) = root.bytes;
    SetGeometry(child, 10, 20, 80, 40);
    repeated_position = false;
    int x = 300, y = 200;
    auto move = [&]() {
        return ApplyStaticPageMove(&rule, 1, child.bytes, root.bytes,
                                   1920, 1080, x, y, IsRepeatedForTest);
    };
    ok &= Expect(move() && x == 540 && y == 360 && count == 1 &&
                 cache[0].x == 10 && cache[0].y == 20 &&
                 GuiField<int>(child.bytes, GuiObjectField::width) == 144,
        "controller move scales submitted position with cached authored size");
    x = -25; y = 5;
    ok &= Expect(move() && x == -45 && y == 9 && count == 1 &&
                 cache[0].width == 80 && cache[0].x == 10 &&
                 GuiField<int>(child.bytes, GuiObjectField::height) == 72,
        "later live moves follow changed coordinates without rewriting native cache");
    repeated_position = true;
    SetGeometry(child, 540, 360, 123, 456);
    x = 540; y = 360;
    ok &= Expect(move() && x == 540 && y == 360 &&
                 GuiField<int>(child.bytes, GuiObjectField::width) == 123,
        "echoed fitted coordinates bypass live rescaling and resizing");
    int root_x = 999, root_y = 888;
    ok &= Expect(ApplyStaticPageMove(&rule, 1, root.bytes, nullptr, 1920, 1080,
                     root_x, root_y, IsRepeatedForTest) &&
                 root_x == 240 && root_y == 0,
        "live-coordinate policy never changes root aspect-fit anchoring");
    repeated_position = false;
    // Existing entries still work when full; a new object falls through.
    count = 0;
    StaticPageMoveRule full = rule;
    full.capacity = 0;
    x = 300; y = 200;
    ok &= Expect(!ApplyStaticPageMove(&full, 1, child.bytes, root.bytes, 1920, 1080,
                     x, y, IsRepeatedForTest) && x == 300 && y == 200 && count == 0,
        "uncached live move falls through unchanged when cache is exhausted");
    return ok;
}

bool TestControllerOwnedRefresh() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject parent{}, root{}, replacement{}, unrelated{};
    reflow_parent = parent.bytes;
    reflow_candidate = root.bytes;
    reflow_page = 0;
    reflow_width = 1920; reflow_height = 1080;
    repeated_position = false;
    GuiField<void*>(reflow_child.bytes, GuiObjectField::parent) = root.bytes;
    SetGeometry(reflow_child, 100, 50, 80, 40);
    StaticPageReflowRule rule{nullptr, MatchReflow<0>, ResetReflow<0>, ScaleReflow<0>,
        "controller page", PageRefreshPolicy::PreserveControllerState};
    bool discovered = false;
    auto run = [&](void* object) {
        reflow_events.clear();
        return ReflowStaticPage(&rule, 1, object, parent.bytes, 800, 600,
            RegisterReflow, MarkReflow, discovered) == &rule;
    };
    ok &= Expect(run(root.bytes) && discovered && reflow_events == "aRLSP" &&
                 GuiField<int>(reflow_child.bytes, GuiObjectField::x) == 180,
        "controller page still gets the complete initial discovery fit");
    rule.root = root.bytes;
    // Model controller movement and size changes between maintenance passes.
    SetGeometry(reflow_child, 517, -12, 99, 13);
    for (int i = 0; i < 3; ++i) {
        ok &= Expect(run(root.bytes) && !discovered && reflow_events == "P" &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::x) == 517 &&
                     GuiField<int>(reflow_child.bytes, GuiObjectField::height) == 13 &&
                     reflow_counts[0] == 1,
            "maintenance neither scales/resets/registers nor disturbs live child state");
    }
    reflow_width = 800; reflow_height = 600;
    ok &= Expect(run(root.bytes) && !discovered && reflow_events == "P" &&
                 GuiField<int>(reflow_child.bytes, GuiObjectField::x) == 517,
        "controller-owned refresh does not silently replay geometry on output change");
    reflow_candidate = replacement.bytes;
    GuiField<void*>(reflow_child.bytes, GuiObjectField::parent) = replacement.bytes;
    SetGeometry(reflow_child, 200, 60, 90, 50);
    ok &= Expect(run(replacement.bytes) && discovered && reflow_events == "aRLSP" &&
                 reflow_cache[0][0].x == 200,
        "replacement controller root reinitialises and fits the new native geometry");
    ok &= Expect(!run(unrelated.bytes) && !discovered && reflow_events == "a",
        "unmatched controller rule leaves later specialised branches reachable");
    return ok;
}

bool TestRootPreparation() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject root{};
    int calls = 0;
    void* observed = nullptr;
    auto reset = [&](void* object) {
        ++calls;
        observed = object;
    };
    ok &= Expect(PrepareAspectFitRoot(reset, root.bytes) &&
                 calls == 1 && observed == root.bytes,
        "legacy void reset executes exactly once and permits subtree fitting");
    bool admitted = true;
    auto register_root = [&](void* object) -> bool {
        ++calls;
        observed = object;
        return admitted;
    };
    ok &= Expect(PrepareAspectFitRoot(register_root, root.bytes) && calls == 2,
        "successful bounded root registration permits subtree fitting");
    admitted = false;
    SetGeometry(root, 10, 20, 800, 600);
    ok &= Expect(!PrepareAspectFitRoot(register_root, root.bytes) && calls == 3 &&
                 observed == root.bytes &&
                 GuiField<int>(root.bytes, GuiObjectField::width) == 800,
        "rejected root registration propagates failure without changing geometry");
    admitted = true;
    ok &= Expect(PrepareAspectFitRoot(register_root, root.bytes) && calls == 4,
        "a prior rejection is not cached across future root registration attempts");
    return ok;
}

bool TestOrderedDispatch() {
    struct Context { int coordinate = 5; int calls = 0; int handled_at = -1; };
    using Handler = bool (*)(Context&);
    const Handler handlers[] = {
        [](Context& c) { ++c.calls; c.coordinate += 7; return c.handled_at == 0; },
        [](Context& c) { ++c.calls; c.coordinate *= 2; return c.handled_at == 1; },
        [](Context& c) { ++c.calls; c.coordinate -= 3; return c.handled_at == 2; },
    };
    bool ok = true;
    for (int selected = -1; selected < 3; ++selected) {
        Context context;
        context.handled_at = selected;
        const bool handled = stardom::DispatchFirstHandled(handlers, context);
        const int expected[] = {12, 24, 21};
        ok &= Expect(handled == (selected >= 0) &&
                     context.calls == (selected < 0 ? 3 : selected + 1) &&
                     context.coordinate == expected[selected < 0 ? 2 : selected],
            "ordered dispatch stops at first handler and retains fallthrough mutations");
    }
    return ok;
}

bool TestSubtreeTraversal() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject nodes[4]{};
    auto connect = [&]() {
        GuiField<void*>(nodes[0].bytes, GuiObjectField::first_child) = nodes[1].bytes;
        GuiField<void*>(nodes[1].bytes, GuiObjectField::next_sibling) = nodes[2].bytes;
        GuiField<void*>(nodes[1].bytes, GuiObjectField::first_child) = nodes[3].bytes;
    };
    auto index = [&](void* object) {
        for (int i = 0; i < 4; ++i) if (object == nodes[i].bytes) return i;
        return -1;
    };
    std::string events;
    int reject = -1;
    bool mutate = false, guarded = false, correct_guard = true;
    auto enter = [&](void* object, int depth) {
        const int i = index(object);
        events += char('A' + i);
        if (i == reject) return false;
        if (depth == 0) guarded = true;
        correct_guard &= guarded;
        if (mutate && i == 1) {
            GuiField<void*>(object, GuiObjectField::next_sibling) = nullptr;
        }
        return true;
    };
    auto leave = [&](void* object, int depth) {
        correct_guard &= guarded;
        events += char('a' + index(object));
        if (depth == 0) guarded = false;
    };
    connect();
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 512, enter, leave);
    ok &= Expect(events == "ABDdbCca" && correct_guard && !guarded,
        "subtree traversal keeps pre/post order and root guard active through descendants");
    events.clear();
    VisitGuiSubtree(nodes[0].bytes, 0, 0, 512, enter, leave);
    ok &= Expect(events == "Aa", "maximum depth is inclusive and excludes deeper node callbacks");
    events.clear();
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 1, enter, leave);
    ok &= Expect(events == "ABDdba",
        "child budget applies independently to each parent");
    events.clear(); reject = 1;
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 512, enter, leave);
    ok &= Expect(events == "ABCca", "rejected node skips descendants and leave callback");
    events.clear(); reject = 0;
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 512, enter, leave);
    ok &= Expect(events == "A" && !guarded, "root preparation rejection stops the subtree");
    events.clear(); reject = -1; mutate = true;
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 512, enter, leave);
    ok &= Expect(events == "ABDdbCca", "next sibling is retained before child callbacks mutate links");
    events.clear();
    VisitGuiSubtree(nullptr, 0, 8, 512, enter, leave);
    VisitGuiSubtree(nodes[0].bytes, 9, 8, 512, enter, leave);
    ok &= Expect(events.empty(), "invalid root and excessive initial depth trigger no callbacks");
    events.clear(); mutate = false; connect();
    GuiField<void*>(nodes[2].bytes, GuiObjectField::next_sibling) = nodes[2].bytes;
    VisitGuiSubtree(nodes[0].bytes, 0, 8, 3, enter, leave);
    ok &= Expect(events == "ABDdbCcCca",
        "sibling cycle is bounded by the original per-parent child limit");
    return ok;
}

bool TestPageGroupMoves() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject roots[3]{}, children[3]{}, outsider{};
    void* root_list[] = {roots[0].bytes, roots[1].bytes, roots[2].bytes};
    NativeGeometry cache[3]{};
    size_t count = 0;
    const StaticPageMoveRule group{nullptr, cache, std::size(cache), &count,
        10, PageMoveCoordinates::SubmittedNative};
    size_t root_count = std::size(root_list);
    auto move = [&](void* object, void* parent, int& x, int& y) {
        return ApplyPageGroupMove(group, root_list, root_count, object, parent,
            1920, 1080, x, y, IsRepeatedForTest);
    };
    repeated_position = false;
    for (size_t i = 0; i < 3; ++i) {
        GuiField<void*>(children[i].bytes, GuiObjectField::parent) = roots[i].bytes;
        SetGeometry(children[i], 10, 20, 80, 40);
        int x = 100 + static_cast<int>(i) * 100, y = 50;
        ok &= Expect(move(children[i].bytes, roots[i].bytes, x, y) &&
                     x == 180 + static_cast<int>(i) * 180 && y == 90 &&
                     count == i + 1 && cache[i].object == children[i].bytes &&
                     cache[i].x == 10,
            "all activity pages use one shared cache and live submitted positions");
    }
    // Nest a later root under an earlier one: unlike ordinary rule ordering,
    // group membership must win over the earlier root's descendant match.
    GuiField<void*>(roots[2].bytes, GuiObjectField::parent) = roots[0].bytes;
    SetGeometry(roots[2], 30, 40, 80, 60);
    int x = 30, y = 40;
    repeated_position = true;
    repeated_checks = 0;
    ok &= Expect(move(roots[2].bytes, roots[0].bytes, x, y) &&
                 x == 240 && y == 0 && count == 3 && repeated_checks == 0 &&
                 GuiField<int>(roots[2].bytes, GuiObjectField::width) == 1440,
        "all group roots precede descendants even when nested and cache-full");
    x = 360; y = 90;
    ok &= Expect(move(children[1].bytes, roots[1].bytes, x, y) &&
                 x == 360 && y == 90 && count == 3,
        "repeated group child move bypasses scaling");
    repeated_position = false;
    x = -25; y = 100;
    ok &= Expect(move(children[0].bytes, roots[0].bytes, x, y) &&
                 x == -45 && y == 180 && count == 3 && cache[0].x == 10 &&
                 GuiField<int>(children[0].bytes, GuiObjectField::width) == 144,
        "cache-full group still updates existing members without cumulative scale");
    GuiField<void*>(outsider.bytes, GuiObjectField::parent) = roots[0].bytes;
    SetGeometry(outsider, 10, 20, 30, 40);
    x = 55; y = 66;
    ok &= Expect(!move(outsider.bytes, roots[0].bytes, x, y) &&
                 x == 55 && y == 66 && count == 3 &&
                 GuiField<int>(outsider.bytes, GuiObjectField::width) == 30,
        "uncached group member falls through untouched when shared cache is full");
    root_count = 0;
    repeated_checks = 0;
    ok &= Expect(!move(children[0].bytes, roots[0].bytes, x, y) &&
                 repeated_checks == 0 && x == 55 && y == 66,
        "cleared root registry prevents stale group membership");
    root_count = 1;
    root_list[0] = roots[1].bytes;
    count = 0;
    SetGeometry(children[1], 70, 80, 90, 100);
    ok &= Expect(move(children[1].bytes, roots[1].bytes, x, y) && count == 1 &&
                 cache[0].object == children[1].bytes && cache[0].width == 90,
        "restarted group registry uses fresh native geometry after cache reset");
    x = 11; y = 12;
    ok &= Expect(!move(children[0].bytes, roots[0].bytes, x, y) &&
                 !move(children[1].bytes, nullptr, x, y) &&
                 !move(nullptr, nullptr, x, y) && x == 11 && y == 12,
        "removed roots, parentless descendants, and null objects fall through");
    return ok;
}

}  // namespace

bool TestInventoryPage() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject primary{}, root{}, card{}, controls[12]{};
    SetGeometry(root, 560, 240, 800, 600);
    SetGeometry(card, 200, 35, 420, 542);
    GuiField<void*>(root.bytes, GuiObjectField::parent) = primary.bytes;
    GuiField<void*>(root.bytes, GuiObjectField::first_child) = card.bytes;
    GuiField<void*>(card.bytes, GuiObjectField::parent) = root.bytes;
    GuiField<void*>(card.bytes, GuiObjectField::first_child) = controls[0].bytes;
    for (int i = 0; i < 12; ++i) {
        GuiField<void*>(controls[i].bytes, GuiObjectField::parent) = card.bytes;
        if (i < 11) GuiField<void*>(controls[i].bytes, GuiObjectField::next_sibling) = controls[i + 1].bytes;
    }
    for (int i = 0; i < 3; ++i) SetGeometry(controls[i], 125 + i * 65, 12, 40, 40);
    for (int i = 0; i < 5; ++i) SetGeometry(controls[i + 3], 0, 65 + i * 85, i == 4 ? 370 : 380, 90);
    SetGeometry(controls[8], 375, 60, 28, 79);
    SetGeometry(controls[9], 375, 410, 28, 79);
    SetGeometry(controls[10], 375, 139, 28, 271);
    SetGeometry(controls[11], 283, 500, 98, 28);
    ok &= Expect(IsInventoryPageTree(root.bytes, primary.bytes), "inventory resource tree must match");
    SetGeometry(controls[1], 279, 12, 40, 40);
    SetGeometry(controls[2], 344, 12, 40, 40);
    ok &= Expect(IsInventoryPageTree(root.bytes, primary.bytes), "selected inventory tab offsets must match");
    SetGeometry(card, 40, 31, 720, 537);
    ok &= Expect(!IsInventoryPageTree(root.bytes, primary.bytes), "save/load card must remain native");
    SetGeometry(card, 200, 35, 420, 542);
    SetGeometry(controls[7], 0, 405, 380, 90);
    ok &= Expect(!IsInventoryPageTree(root.bytes, primary.bytes), "incomplete inventory signature must not match");
    SetGeometry(controls[7], 0, 405, 370, 90);
    GuiField<void*>(controls[11].bytes, GuiObjectField::next_sibling) = controls[0].bytes;
    ok &= Expect(!IsInventoryPageTree(root.bytes, primary.bytes), "cyclic inventory signature must terminate");
    GuiField<void*>(controls[11].bytes, GuiObjectField::next_sibling) = nullptr;
    ok &= Expect(!IsInventoryPageTree(root.bytes, card.bytes), "inventory must be a direct root child");

    NativeGeometry cache[16]{};
    size_t count = 0;
    RememberGeometry(cache, 16, count, card.bytes);
    for (auto& control : controls) RememberGeometry(cache, 16, count, control.bytes);
    const StaticPageMoveRule rule{root.bytes, cache, 16, &count, 6, PageMoveCoordinates::SubmittedNative};
    repeated_position = false;
    int x = 200, y = 35;
    ok &= Expect(ApplyStaticPageMove(&rule, 1, card.bytes, root.bytes, 1920, 1080, x, y, IsRepeatedForTest) &&
        x == 360 && y == 63 && GuiField<int>(card.bytes, GuiObjectField::width) == 756 &&
        GuiField<int>(card.bytes, GuiObjectField::height) == 976, "inventory card must fit 1080p");
    x = 220; y = 12;
    ok &= Expect(ApplyStaticPageMove(&rule, 1, controls[1].bytes, card.bytes, 1920, 1080, x, y, IsRepeatedForTest) &&
        x == 396 && y == 22 && GuiField<int>(controls[1].bytes, GuiObjectField::width) == 72,
        "inventory tabs must use current interpolation, not their initial slots");
    x = 279; y = 12;
    ok &= Expect(ApplyStaticPageMove(&rule, 1, controls[1].bytes, card.bytes, 1920, 1080, x, y, IsRepeatedForTest) &&
        x == 502 && GuiField<int>(controls[1].bytes, GuiObjectField::width) == 72,
        "inventory tab refresh must not double scale");
    for (int i = 0; i < 5; ++i) {
        x = 0; y = 65 + i * 85;
        ok &= Expect(ApplyStaticPageMove(&rule, 1, controls[i + 3].bytes, card.bytes, 1920, 1080, x, y, IsRepeatedForTest) &&
            y == 117 + i * 153 && GuiField<int>(controls[i + 3].bytes, GuiObjectField::height) == 162,
            "all five inventory rows must preserve spacing");
    }
    const auto memo = FitCompanyListMetrics({1, 0, 199, 59, 12, 3}, 1440, 1080);
    ok &= Expect(memo.left == 2 && memo.right == 358 && memo.bottom == 106 &&
        memo.font_size == 22 && memo.line_gap == 5, "inventory memo viewport and baseline must scale together");
    return ok;
}

bool TestInventoryTarget() {
    using namespace stardom;
    bool ok = true;
    TestGuiObject primary{}, root{}, card{}, options[5]{};
    SetGeometry(root, 371, 484, 370, 90);
    SetGeometry(card, 0, 30, 363, 56);
    GuiField<void*>(root.bytes, GuiObjectField::parent) = primary.bytes;
    GuiField<void*>(root.bytes, GuiObjectField::first_child) = card.bytes;
    GuiField<void*>(card.bytes, GuiObjectField::first_child) = options[0].bytes;
    for (int i = 0; i < 5; ++i) {
        if (i < 4) SetGeometry(options[i], 20 + i * 70, -26, 70, 80);
        else SetGeometry(options[i], 311, 17, 46, 22);
        if (i < 4) GuiField<void*>(options[i].bytes, GuiObjectField::next_sibling) = options[i + 1].bytes;
    }
    ok &= Expect(IsInventoryTargetTree(root.bytes, primary.bytes), "complete recipient popup must match");
    SetGeometry(options[3], 230, 0, 70, 80);
    ok &= Expect(!IsInventoryTargetTree(root.bytes, primary.bytes), "unrelated popup must not match");
    SetGeometry(options[3], 230, -26, 70, 80);
    GuiField<void*>(options[4].bytes, GuiObjectField::next_sibling) = options[0].bytes;
    ok &= Expect(!IsInventoryTargetTree(root.bytes, primary.bytes), "cyclic recipient popup must terminate");
    GuiField<void*>(options[4].bytes, GuiObjectField::next_sibling) = nullptr;
    ok &= Expect(!IsInventoryTargetTree(root.bytes, card.bytes), "recipient popup must be a direct root child");
    for (int row = 0; row < 5; ++row) {
        int x = 360 + 11, y = 63 + 81 + row * 85;
        FitInventoryTargetPosition(360, 63, 240, 0, 1440, 1080, x, y);
        ok &= Expect(x == 620 && y == 209 + row * 153,
            "recipient popup must fit only native offsets for every item row");
        // The popup's decorative Give (311,47) aligns to the source row's
        // Give (323,65+row*85+62), allowing one-pixel independent rounding.
        ok &= Expect(x + 560 == 1180 && y + 85 == 294 + row * 153,
            "recipient Give must stay beside the clicked inventory row");
        x = 211; y = 35 + 81 + row * 85;
        FitInventoryTargetPosition(200, 35, 0, 0, 800, 600, x, y);
        ok &= Expect(x == 211 && y == 116 + row * 85, "native resolution must stay unchanged");
    }
    int x = 211, y = 116;
    FitInventoryTargetPosition(200, 35, 0, 100, 800, 600, x, y);
    ok &= Expect(x == 211 && y == 216, "letterbox origin must be added once");
    return ok;
}

int main() {
    using namespace stardom;
    bool ok = TestInventoryPage();
    ok &= TestInventoryTarget();
    ok &= TestStaticPageMoves();
    ok &= TestStaticPageAncestorLimits();
    ok &= TestStaticPageReflow();
    ok &= TestSubmittedPageMoves();
    ok &= TestControllerOwnedRefresh();
    ok &= TestPageGroupMoves();
    ok &= TestRootPreparation();
    ok &= TestOrderedDispatch();
    ok &= TestSubtreeTraversal();
    const auto text_native = FitPageTextMetrics(12, 0, 600);
    const auto text_hd = FitPageTextMetrics(12, 0, 1080);
    const auto text_multiline = FitPageTextMetrics(16, 3, 1080);
    ok &= Expect(text_native.font_size == 12 && text_native.line_gap == 0,
        "native page font and gap must remain unchanged");
    ok &= Expect(text_hd.font_size == 22 && text_hd.line_gap == 0 &&
                 text_multiline.font_size + text_multiline.line_gap + 1 == 36,
        "fitted text must scale glyphs and the complete baseline pitch");
    ok &= Expect(CorrectCompanyColumnCenter(66, 126, 70) == 94 &&
                 CorrectCompanyColumnCenter(258, 126, 70) == 286 &&
                 CorrectCompanyColumnCenter(94, 70, 70) == 94,
        "undo fitted half-width without changing native column spacing");
    const CompanyListMetrics list_native{0, 7, 299, 378, 12, 8};
    const auto list_fitted = FitCompanyListMetrics(list_native, 1440, 1080);
    ok &= Expect(list_fitted.top == 13 && list_fitted.right == 538 &&
                 list_fitted.bottom == 680 && list_fitted.font_size == 22 &&
                 list_fitted.line_gap == 14,
        "list text bounds and glyphs must fit with the background");
    ok &= Expect(list_fitted.font_size + list_fitted.line_gap == 36 &&
                 (list_fitted.bottom - list_fitted.top + list_fitted.line_gap + 1) /
                     (list_fitted.font_size + list_fitted.line_gap) == 18 &&
                 list_native.font_size == 12 && list_native.line_gap == 8,
        "render fit preserves visible rows and leaves logical metrics native");
    for (int x : {66, 129, 194, 258, 320, 95, 159, 223, 287, 351}) {
        ok &= Expect(IsCompanyChartColumn(x, 133, 70, 241),
            "recentered and resource chart columns must both match");
    }
    ok &= Expect(IsCompanyChartColumn(66, 133, 70, 237),
        "first artist column has a shorter portrait container");
    ok &= Expect(!IsCompanyChartColumn(25, 137, 43, 160) &&
                 !IsCompanyChartColumn(119, 239, 126, 427),
        "axis and already fitted geometry must not match native columns");
    for (int x : {209, 219, 255, 265, 311, 357, 403, 405, 451, 497, 543}) {
        ok &= Expect(IsCompanyTabButton(x, 41, 40, 40),
            "all tab selections and interpolated slots must be recognized");
    }
    ok &= Expect(!IsCompanyTabButton(498, 526, 92, 32) &&
                 !IsCompanyTabButton(394, 74, 72, 72),
        "exit and fitted buttons must not match native tabs");
    TestGuiObject first{};
    TestGuiObject second{};
    NativeGeometry geometries[2]{};
    size_t count = 0;

    SetGeometry(first, 10, 20, 30, 40);
    NativeGeometry* remembered = RememberGeometry(
        geometries, std::size(geometries), count, first.bytes);
    ok &= Expect(remembered && remembered->x == 10 && remembered->height == 40,
        "geometry must retain authored values");
    ok &= Expect(FindGeometry(geometries, count, first.bytes) == remembered,
        "cached geometry lookup must return the retained entry");

    count = 0;
    SetGeometry(second, 50, 60, 70, 80);
    NativeGeometry* replacement = RememberGeometry(
        geometries, std::size(geometries), count, second.bytes);
    ok &= Expect(replacement == &geometries[0] && replacement->x == 50,
        "reset array must invalidate an old lookup-cache entry");
    ok &= Expect(FindGeometry(geometries, count, first.bytes) == nullptr,
        "replaced object must not be returned from the lookup cache");

    GuiField<void*>(first.bytes, GuiObjectField::parent) = second.bytes;
    ok &= Expect(IsDescendantOf(first.bytes, second.bytes, 2),
        "valid parent chain must find its root");
    ok &= Expect(!IsDescendantOf(first.bytes, nullptr, 16),
        "missing root must be rejected before walking the parent chain");
    return ok ? 0 : 1;
}
