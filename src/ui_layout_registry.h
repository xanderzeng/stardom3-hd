#pragma once

#include <cstddef>

namespace stardom {

bool IsKnownLayoutRoot(void* object);
void RememberLayoutRoot(void* object);
bool IsObjectInList(void* object, void* const* objects, size_t count);
void RememberWorldProjectedObject(void* object);
bool IsWorldProjectedObject(void* object);
void RememberWorldMoveSource(void* source_return);
bool IsWorldMoveSource(void* source_return);
void RememberGroupCanvas(void* object);
bool IsGroupCanvas(void* object);
void RememberWorldRoot(void* object);
bool IsWorldRoot(void* object);
bool IsProcessedLayoutObject(void* object);
void RememberProcessedLayoutObject(void* object);

}  // namespace stardom
