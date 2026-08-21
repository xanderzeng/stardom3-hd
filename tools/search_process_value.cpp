#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: search_process_value <pid> <uint32-value>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uint32_t wanted = static_cast<uint32_t>(std::wcstoul(argv[2], nullptr, 0));
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
    size_t matches = 0;
    while (cursor < maximum && matches < 512) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQueryEx(process, reinterpret_cast<void*>(cursor),
                            &info, sizeof(info))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const bool readable = info.State == MEM_COMMIT &&
            !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            info.RegionSize <= 0x2000000;
        if (readable) {
            std::vector<unsigned char> bytes(info.RegionSize);
            SIZE_T read = 0;
            if (ReadProcessMemory(process, info.BaseAddress, bytes.data(),
                                  bytes.size(), &read)) {
                for (size_t i = 0; i + sizeof(wanted) <= read; i += 4) {
                    if (*reinterpret_cast<const uint32_t*>(bytes.data() + i) != wanted) {
                        continue;
                    }
                    std::printf("%08llX region=%08llX protect=%08lX\n",
                        static_cast<unsigned long long>(base + i),
                        static_cast<unsigned long long>(base), info.Protect);
                    ++matches;
                }
            }
        }
        const uintptr_t next = base + info.RegionSize;
        if (next <= cursor) break;
        cursor = next;
    }
    CloseHandle(process);
    std::printf("total matches=%zu\n", matches);
    return 0;
}
