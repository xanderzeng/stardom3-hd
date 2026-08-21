#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool Matches(const unsigned char* p, const unsigned char* end,
             const unsigned char* pattern, size_t size) {
    return static_cast<size_t>(end - p) >= size &&
        std::equal(pattern, pattern + size, p);
}

bool IsImmediateCompare(const unsigned char* p, const unsigned char* end,
                        uint32_t value) {
    if (end - p < 5) return false;
    if (p[0] == 0x3D && *reinterpret_cast<const uint32_t*>(p + 1) == value) {
        return true;
    }
    if (end - p >= 6 && p[0] == 0x81 && (p[1] & 0xF8) == 0xF8 &&
        *reinterpret_cast<const uint32_t*>(p + 2) == value) {
        return true;
    }
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"usage: scan_process_constants <pid>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t maximum = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    size_t candidates = 0;
    while (cursor < maximum) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(cursor),
                            &info, sizeof(info))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const bool executable = info.State == MEM_COMMIT &&
            !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (executable && base >= 0x00400000 && base < 0x01000000 &&
            info.RegionSize <= 0x1000000) {
            std::vector<unsigned char> bytes(info.RegionSize);
            SIZE_T read = 0;
            if (ReadProcessMemory(process, info.BaseAddress, bytes.data(),
                                  bytes.size(), &read) && read > 0) {
                const unsigned char* begin = bytes.data();
                const unsigned char* end = begin + read;
                for (size_t i = 0; i + 6 < read; ++i) {
                    if (!IsImmediateCompare(begin + i, end, 800)) continue;
                    const size_t search_end = (std::min)(
                        static_cast<size_t>(read), i + 192);
                    bool paired = false;
                    for (size_t j = i + 5; j + 6 < search_end; ++j) {
                        if (IsImmediateCompare(begin + j, end, 600)) {
                            paired = true;
                            break;
                        }
                    }
                    if (!paired) continue;
                    const size_t from = i > 32 ? i - 32 : 0;
                    const size_t to = (std::min)(
                        static_cast<size_t>(read), i + 224);
                    std::printf("candidate %zu at %08llX region=%08llX\n",
                        ++candidates,
                        static_cast<unsigned long long>(base + i),
                        static_cast<unsigned long long>(base));
                    for (size_t line = from; line < to; line += 16) {
                        std::printf("%08llX  ",
                            static_cast<unsigned long long>(base + line));
                        for (size_t k = 0; k < 16 && line + k < to; ++k) {
                            std::printf("%02X ", bytes[line + k]);
                        }
                        std::printf("\n");
                    }
                }
            }
        }
        const uintptr_t next = base + info.RegionSize;
        if (next <= cursor) break;
        cursor = next;
    }
    CloseHandle(process);
    std::printf("total candidates=%zu\n", candidates);
    return 0;
}
