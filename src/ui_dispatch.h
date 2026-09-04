#pragma once

#include "gui_object.h"
#include <cstddef>

namespace stardom {

// The first handled branch owns the operation. Mutations to context made by
// an unhandled branch remain visible to the following branch, as in an if chain.
template <typename Handler, size_t Count, typename Context>
bool DispatchFirstHandled(const Handler (&handlers)[Count], Context& context) {
    for (auto handler : handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

// enter returns false to skip a node's children. Snapshot each next sibling
// before visiting the child: a game's Move callback can mutate sibling links.
template <typename Enter, typename Leave>
void VisitGuiSubtree(void* object, int depth, int maximum_depth,
                     size_t maximum_children, Enter& enter, Leave& leave) {
    if (!CanReadGuiObject(object) || depth > maximum_depth || !enter(object, depth)) {
        return;
    }
    void* child = GuiPointer(object, GuiObjectField::first_child);
    size_t visited = 0;
    while (CanReadGuiObject(child) && visited++ < maximum_children) {
        void* next = GuiPointer(child, GuiObjectField::next_sibling);
        VisitGuiSubtree(child, depth + 1, maximum_depth, maximum_children, enter, leave);
        child = next;
    }
    leave(object, depth);
}

}  // namespace stardom
