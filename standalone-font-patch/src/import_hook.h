#pragma once

#include <windows.h>

namespace stardom_font {

struct ImportHook {
    ULONG_PTR* slot = nullptr;
    void* original = nullptr;
};

bool PatchImport(HMODULE module, const char* imported_dll,
                 const char* imported_name, void* replacement,
                 ImportHook& hook);
bool RestoreImport(ImportHook& hook);

}  // namespace stardom_font
