#include "render_diagnostics.h"

#include "d3d9_proxy_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace stardom {
namespace {

bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
        base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool IsExecutableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || protection == PAGE_NOACCESS) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

std::vector<uintptr_t> FindBytesInModule(HMODULE module, const void* needle,
                                         size_t needle_size,
                                         bool executable_only,
                                         size_t max_results) {
    std::vector<uintptr_t> results;
    if (!module || !needle || needle_size == 0) {
        return results;
    }
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return results;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return results;
    }
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    uintptr_t cursor = base;
    while (cursor < end && results.size() < max_results) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                          sizeof(info))) {
            break;
        }
        const uintptr_t region_start = std::max(
            cursor, reinterpret_cast<uintptr_t>(info.BaseAddress));
        const uintptr_t queried_end =
            reinterpret_cast<uintptr_t>(info.BaseAddress) +
            static_cast<uintptr_t>(info.RegionSize);
        const uintptr_t region_end = std::min<uintptr_t>(end, queried_end);
        const bool usable = info.State == MEM_COMMIT &&
            IsReadableProtection(info.Protect) &&
            (!executable_only || IsExecutableProtection(info.Protect));
        if (usable && region_end >= region_start + needle_size) {
            for (uintptr_t address = region_start;
                 address + needle_size <= region_end &&
                     results.size() < max_results;
                 ++address) {
                if (std::memcmp(reinterpret_cast<const void*>(address), needle,
                                needle_size) == 0) {
                    results.push_back(address);
                }
            }
        }
        if (region_end <= cursor) {
            break;
        }
        cursor = region_end;
    }
    return results;
}

void ProbeGUIStringsInModule(HMODULE module, const char* module_name) {
    if (!module) {
        return;
    }
    Log("GUI runtime probe module=%s base=%p", module_name, module);
    const char* targets[] = {"GuiRoot", "TodayDate", "GameMain", "GameShort"};
    for (const char* target : targets) {
        const auto strings = FindBytesInModule(
            module, target, std::strlen(target) + 1, false, 16);
        Log("GUI probe string=%s matches=%u", target,
            static_cast<unsigned>(strings.size()));
        for (uintptr_t string_address : strings) {
            Log("GUI string %s at=%p", target,
                reinterpret_cast<void*>(string_address));
            const uint32_t direct_value = static_cast<uint32_t>(string_address);
            const auto direct_xrefs = FindBytesInModule(
                module, &direct_value, sizeof(direct_value), true, 24);
            for (uintptr_t xref : direct_xrefs) {
                Log("GUI direct xref %s code=%p", target,
                    reinterpret_cast<void*>(xref));
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<void*>(xref), &info,
                                 sizeof(info))) {
                    const uintptr_t region_start =
                        reinterpret_cast<uintptr_t>(info.BaseAddress);
                    const uintptr_t region_end = region_start + info.RegionSize;
                    const uintptr_t dump_start =
                        xref >= region_start + 8 ? xref - 8 : xref;
                    const size_t dump_size = static_cast<size_t>(
                        std::min<uintptr_t>(32, region_end - dump_start));
                    LogProbeBytes(
                        reinterpret_cast<const unsigned char*>(dump_start),
                        dump_size);
                }
            }

            const auto pointer_slots = FindBytesInModule(
                module, &direct_value, sizeof(direct_value), false, 32);
            for (uintptr_t slot : pointer_slots) {
                if (std::find(direct_xrefs.begin(), direct_xrefs.end(), slot) !=
                    direct_xrefs.end()) {
                    continue;
                }
                const uint32_t slot_value = static_cast<uint32_t>(slot);
                const auto indirect_xrefs = FindBytesInModule(
                    module, &slot_value, sizeof(slot_value), true, 16);
                for (uintptr_t xref : indirect_xrefs) {
                    Log("GUI indirect xref %s slot=%p code=%p", target,
                        reinterpret_cast<void*>(slot),
                        reinterpret_cast<void*>(xref));
                }
            }
        }
    }
}

}  // namespace

void LogProbeBytes(const unsigned char* address, size_t count) {
    char line[256]{};
    size_t used = 0;
    for (size_t i = 0; i < count && used + 4 < sizeof(line); ++i) {
        const int written = sprintf_s(
            line + used, sizeof(line) - used, "%02X ", address[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    Log("GUI xref bytes: %s", line);
}

void RunGUIRuntimeProbe() {
    Log("GUI runtime probe begin");
    ProbeGUIStringsInModule(GetModuleHandleW(nullptr), "Stardom3.exe");
    ProbeGUIStringsInModule(GetModuleHandleW(L"Stardom3.dll"), "Stardom3.dll");
    Log("GUI runtime probe end");
}

}  // namespace stardom
