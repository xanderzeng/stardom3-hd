#pragma once

#include <cstddef>

namespace stardom {

void LogProbeBytes(const unsigned char* address, size_t count);
void RunGUIRuntimeProbe();

}  // namespace stardom
