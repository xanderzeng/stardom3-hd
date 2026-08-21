#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::fwprintf(stderr, L"usage: read_process_memory <pid> <address> <size>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uintptr_t address = static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 0));
    const size_t size = static_cast<size_t>(std::wcstoull(argv[3], nullptr, 0));
    if (!pid || !address || !size || size > 0x10000) {
        std::fwprintf(stderr, L"invalid argument\n");
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    std::vector<unsigned char> bytes(size);
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(
        process, reinterpret_cast<const void*>(address), bytes.data(), bytes.size(), &read);
    CloseHandle(process);
    if (!ok || read == 0) {
        std::fwprintf(stderr, L"ReadProcessMemory failed: %lu\n", GetLastError());
        return 1;
    }

    for (size_t offset = 0; offset < read; offset += 16) {
        std::printf("%08llX  ", static_cast<unsigned long long>(address + offset));
        for (size_t column = 0; column < 16; ++column) {
            if (offset + column < read) {
                std::printf("%02X ", bytes[offset + column]);
            } else {
                std::printf("   ");
            }
        }
        std::printf(" ");
        for (size_t column = 0; column < 16 && offset + column < read; ++column) {
            const unsigned char value = bytes[offset + column];
            std::printf("%c", value >= 0x20 && value <= 0x7E ? value : '.');
        }
        std::printf("\n");
    }
    return 0;
}
