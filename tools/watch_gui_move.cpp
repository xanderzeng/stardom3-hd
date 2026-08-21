#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

template <typename T>
bool ReadValue(HANDLE process, uintptr_t address, T& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<const void*>(address),
                             &value, sizeof(value), &read) &&
           read == sizeof(value);
}

bool ArmThread(DWORD tid, uintptr_t address) {
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                               THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!thread) return false;
    SuspendThread(thread);
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    bool ok = GetThreadContext(thread, &context) != FALSE;
    if (ok) {
        context.Dr0 = address;
        context.Dr6 = 0;
        // Local DR0, execute access, one-byte length.
        context.Dr7 = (context.Dr7 & ~0x000F0003u) | 0x00000001u;
        ok = SetThreadContext(thread, &context) != FALSE;
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: watch_gui_move <pid> <move-address>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uintptr_t move_address = std::wcstoull(argv[2], nullptr, 0);
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                 FALSE, pid);
    if (!process) return 1;
    if (!DebugActiveProcess(pid)) {
        std::fwprintf(stderr, L"DebugActiveProcess failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    const ULONGLONG deadline = GetTickCount64() + 120000;
    bool captured = false;
    while (GetTickCount64() < deadline && !captured) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 1000)) continue;
        DWORD status = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            ArmThread(event.dwThreadId, move_address);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            ArmThread(event.dwThreadId, move_address);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto code = event.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP) {
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                           FALSE, event.dwThreadId);
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                if (thread && GetThreadContext(thread, &context)) {
                    int width = 0, height = 0, x = 0, y = 0;
                    uintptr_t return_address = 0;
                    uintptr_t source_return_address = 0;
                    ReadValue(process, context.Ecx + 0x88, width);
                    ReadValue(process, context.Ecx + 0x8C, height);
                    ReadValue(process, context.Esp + 4, x);
                    ReadValue(process, context.Esp + 8, y);
                    ReadValue(process, context.Esp, return_address);
                    // The wrapper at 004D7100 pushes x/y and then calls the
                    // shared GUI move function. Its own return address is
                    // therefore three stack slots above this call's return.
                    ReadValue(process, context.Esp + 12,
                              source_return_address);
                    const bool location_label = width >= 165 && width <= 180 &&
                        height >= 32 && height <= 40;
                    const bool compact_world_tag = width >= 30 && width <= 120 &&
                        height >= 18 && height <= 24;
                    if (location_label || compact_world_tag) {
                        std::printf("GUI move self=%08lX rect=%d,%d %dx%d "
                                    "return=%08llX call=%08llX source_return=%08llX source_call=%08llX\n",
                                    context.Ecx, x, y, width, height,
                                    static_cast<unsigned long long>(return_address),
                                    static_cast<unsigned long long>(return_address - 5),
                                    static_cast<unsigned long long>(source_return_address),
                                    static_cast<unsigned long long>(
                                        source_return_address >= 5 ?
                                        source_return_address - 5 : 0));
                        unsigned char bytes[256]{};
                        SIZE_T read = 0;
                        const uintptr_t start = return_address - 133;
                        if (ReadProcessMemory(process,
                                reinterpret_cast<const void*>(start), bytes,
                                sizeof(bytes), &read)) {
                            for (SIZE_T i = 0; i < read; i += 16) {
                                std::printf("%08llX  ",
                                    static_cast<unsigned long long>(start + i));
                                for (SIZE_T k = 0; k < 16 && i + k < read; ++k)
                                    std::printf("%02X ", bytes[i + k]);
                                std::printf("\n");
                            }
                        }
                        captured = true;
                    }
                    // Resume past the current hardware breakpoint once.
                    context.EFlags |= 0x10000;
                    context.Dr6 = 0;
                    SetThreadContext(thread, &context);
                }
                if (thread) CloseHandle(thread);
            } else if (code != EXCEPTION_BREAKPOINT) {
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            captured = true;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    DebugActiveProcessStop(pid);
    CloseHandle(process);
    if (!captured) {
        std::printf("timeout waiting for 172x36 GUI move\n");
        return 3;
    }
    return 0;
}
