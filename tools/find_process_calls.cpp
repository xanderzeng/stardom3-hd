#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: find_process_calls <pid> <target>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uintptr_t target = std::wcstoull(argv[2], nullptr, 0);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    size_t matches = 0;
    uintptr_t cursor = 0x00400000;
    while (cursor < 0x01000000) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(cursor),
                            &info, sizeof(info))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const bool readable = info.State == MEM_COMMIT &&
            !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        if (readable && info.RegionSize <= 0x1000000) {
            std::vector<unsigned char> bytes(info.RegionSize);
            SIZE_T read = 0;
            if (ReadProcessMemory(process, info.BaseAddress, bytes.data(),
                                  bytes.size(), &read)) {
                for (size_t i = 0; i + 5 <= read; ++i) {
                    if (bytes[i] != 0xE8) continue;
                    int32_t displacement = 0;
                    std::memcpy(&displacement, bytes.data() + i + 1,
                                sizeof(displacement));
                    const uintptr_t destination = base + i + 5 + displacement;
                    if (destination != target) continue;
                    ++matches;
                    const size_t from = i > 64 ? i - 64 : 0;
                    const size_t to = (std::min)(static_cast<size_t>(read),
                                                 i + 96);
                    std::printf("call %zu at %08llX\n", matches,
                        static_cast<unsigned long long>(base + i));
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
    std::printf("total calls=%zu\n", matches);
    return 0;
}
