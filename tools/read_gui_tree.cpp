#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

struct GuiNode32 {
    uint32_t vtable = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    unsigned char visible = 0;
    uint32_t parent = 0;
    uint32_t first_child = 0;
    uint32_t next_sibling = 0;
};

template <typename T>
bool ReadValue(HANDLE process, uint32_t address, T& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(
        static_cast<uintptr_t>(address)), &value, sizeof(value), &read) &&
        read == sizeof(value);
}

bool ReadNode(HANDLE process, uint32_t address, GuiNode32& node) {
    return ReadValue(process, address, node.vtable) &&
        ReadValue(process, address + 0x80, node.x) &&
        ReadValue(process, address + 0x84, node.y) &&
        ReadValue(process, address + 0x88, node.width) &&
        ReadValue(process, address + 0x8C, node.height) &&
        ReadValue(process, address + 0x99, node.visible) &&
        ReadValue(process, address + 0xF0, node.parent) &&
        ReadValue(process, address + 0xF4, node.first_child) &&
        ReadValue(process, address + 0xF8, node.next_sibling);
}

void PrintTree(HANDLE process, uint32_t address, int depth, int max_depth,
               int parent_x, int parent_y,
               std::unordered_set<uint32_t>& visited, int& remaining) {
    uint32_t current = address;
    while (current && remaining > 0) {
        if (!visited.insert(current).second) {
            std::printf("%*s%08X cycle\n", depth * 2, "", current);
            return;
        }
        GuiNode32 node{};
        if (!ReadNode(process, current, node)) {
            std::printf("%*s%08X unreadable\n", depth * 2, "", current);
            return;
        }
        --remaining;
        const int absolute_x = parent_x + node.x;
        const int absolute_y = parent_y + node.y;
        std::printf("%*s%08X vt=%08X rect=%d,%d %dx%d abs=%d,%d vis=%u parent=%08X child=%08X next=%08X\n",
            depth * 2, "", current, node.vtable, node.x, node.y,
            node.width, node.height, absolute_x, absolute_y,
            static_cast<unsigned>(node.visible),
            node.parent, node.first_child, node.next_sibling);
        if (node.vtable == 0x006F0348) {
            uint32_t text_pointer = 0;
            uint32_t text_length = 0;
            if (ReadValue(process, current + 0x128, text_pointer) &&
                ReadValue(process, current + 0x12C, text_length) &&
                text_pointer && text_length > 0 && text_length <= 64) {
                unsigned char text[64]{};
                SIZE_T text_read = 0;
                if (ReadProcessMemory(process,
                        reinterpret_cast<const void*>(
                            static_cast<uintptr_t>(text_pointer)),
                        text, text_length, &text_read) && text_read > 0) {
                    std::printf("%*stext[%08X,%u]=", depth * 2 + 2, "",
                        text_pointer, text_length);
                    for (SIZE_T i = 0; i < text_read; ++i) {
                        std::printf("%02X", text[i]);
                    }
                    std::printf("\n");
                }
            }
        }
        if (node.first_child && depth < max_depth) {
            PrintTree(process, node.first_child, depth + 1, max_depth,
                absolute_x, absolute_y, visited, remaining);
        }
        current = node.next_sibling;
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5) {
        std::fwprintf(stderr, L"usage: read_gui_tree <pid> <root> [max-depth] [max-nodes]\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uint32_t root = static_cast<uint32_t>(std::wcstoul(argv[2], nullptr, 0));
    const int max_depth = argc >= 4 ? std::wcstol(argv[3], nullptr, 0) : 8;
    int remaining = argc >= 5 ? std::wcstol(argv[4], nullptr, 0) : 1000;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    std::unordered_set<uint32_t> visited;
    PrintTree(process, root, 0, max_depth, 0, 0, visited, remaining);
    CloseHandle(process);
    return 0;
}
