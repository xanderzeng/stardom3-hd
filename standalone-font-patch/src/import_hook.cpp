#include "import_hook.h"

#include <cstring>

namespace stardom_font {
namespace {

bool WriteImportSlot(ULONG_PTR* slot, ULONG_PTR value) {
    DWORD old_protection = 0;
    if (!slot || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE,
                                 &old_protection)) {
        return false;
    }
    *slot = value;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
    return true;
}

}  // namespace

bool PatchImport(HMODULE module, const char* imported_dll,
                 const char* imported_name, void* replacement,
                 ImportHook& hook) {
    if (!module || !imported_dll || !imported_name || !replacement ||
        hook.slot) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) {
        return false;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* dll_name = reinterpret_cast<const char*>(
            base + descriptor->Name);
        if (_stricmp(dll_name, imported_dll) != 0 ||
            !descriptor->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->OriginalFirstThunk);
        auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++functions) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            imported_name) != 0) {
                continue;
            }
            hook.slot = &functions->u1.Function;
            hook.original = reinterpret_cast<void*>(functions->u1.Function);
            if (!WriteImportSlot(
                    hook.slot, reinterpret_cast<ULONG_PTR>(replacement))) {
                hook = {};
                return false;
            }
            return true;
        }
    }
    return false;
}

bool RestoreImport(ImportHook& hook) {
    if (!hook.slot) {
        return true;
    }
    const bool restored = WriteImportSlot(
        hook.slot, reinterpret_cast<ULONG_PTR>(hook.original));
    if (restored) {
        hook = {};
    }
    return restored;
}

}  // namespace stardom_font
