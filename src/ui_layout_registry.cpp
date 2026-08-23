#include "ui_layout_registry.h"

#include "d3d9_proxy_internal.h"

#include <iterator>

namespace stardom {

bool IsKnownLayoutRoot(void* object) {
    for (size_t i = 0; i < g_unified_ui.root_count; ++i) {
        if (g_unified_ui.roots[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberLayoutRoot(void* object) {
    if (!object || IsKnownLayoutRoot(object) ||
        g_unified_ui.root_count >= std::size(g_unified_ui.roots)) {
        return;
    }
    g_unified_ui.roots[g_unified_ui.root_count++] = object;
    Log("Unified UI root discovered: object=%p output=%ux%u", object,
        g_unified_ui.width, g_unified_ui.height);
}

bool IsObjectInList(void* object, void* const* objects, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (objects[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberWorldProjectedObject(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.world_projected_objects,
                                  g_unified_ui.world_projected_object_count) ||
        g_unified_ui.world_projected_object_count >=
            std::size(g_unified_ui.world_projected_objects)) {
        return;
    }
    g_unified_ui.world_projected_objects[
        g_unified_ui.world_projected_object_count++] = object;
}

bool IsWorldProjectedObject(void* object) {
    return IsObjectInList(object, g_unified_ui.world_projected_objects,
                          g_unified_ui.world_projected_object_count);
}

void RememberWorldMoveSource(void* source_return) {
    if (!source_return ||
        IsObjectInList(source_return, g_unified_ui.world_move_sources,
                       g_unified_ui.world_move_source_count) ||
        g_unified_ui.world_move_source_count >=
            std::size(g_unified_ui.world_move_sources)) {
        return;
    }
    g_unified_ui.world_move_sources[g_unified_ui.world_move_source_count++] =
        source_return;
    Log("Unified UI learned world move source return=%p", source_return);
}

bool IsWorldMoveSource(void* source_return) {
    return IsObjectInList(source_return, g_unified_ui.world_move_sources,
                          g_unified_ui.world_move_source_count);
}

void RememberGroupCanvas(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.group_canvases,
                                  g_unified_ui.group_canvas_count) ||
        g_unified_ui.group_canvas_count >=
            std::size(g_unified_ui.group_canvases)) {
        return;
    }
    g_unified_ui.group_canvases[g_unified_ui.group_canvas_count++] = object;
    Log("Unified UI centered page=%p", object);
}

bool IsGroupCanvas(void* object) {
    return IsObjectInList(object, g_unified_ui.group_canvases,
                          g_unified_ui.group_canvas_count);
}

void RememberWorldRoot(void* object) {
    if (!object || IsObjectInList(object, g_unified_ui.world_roots,
                                  g_unified_ui.world_root_count) ||
        g_unified_ui.world_root_count >= std::size(g_unified_ui.world_roots)) {
        return;
    }
    g_unified_ui.world_roots[g_unified_ui.world_root_count++] = object;
    Log("Unified UI world overlay=%p", object);
}

bool IsWorldRoot(void* object) {
    return IsObjectInList(object, g_unified_ui.world_roots,
                          g_unified_ui.world_root_count);
}

bool IsProcessedLayoutObject(void* object) {
    for (size_t i = 0; i < g_unified_ui.processed_count; ++i) {
        if (g_unified_ui.processed[i] == object) {
            return true;
        }
    }
    return false;
}

void RememberProcessedLayoutObject(void* object) {
    if (!object || IsProcessedLayoutObject(object) ||
        g_unified_ui.processed_count >= std::size(g_unified_ui.processed)) {
        return;
    }
    g_unified_ui.processed[g_unified_ui.processed_count++] = object;
}

}  // namespace stardom
