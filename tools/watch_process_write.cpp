#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

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
        // Local DR0, write access (01), four-byte length (11).
        context.Dr7 = (context.Dr7 & ~0x000F0003u) |
            0x00000001u | (0x1u << 16) | (0x3u << 18);
        ok = SetThreadContext(thread, &context) != FALSE;
    }
    ResumeThread(thread);
    CloseHandle(thread);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::fwprintf(stderr, L"usage: watch_process_write <pid> <address>\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 0));
    const uintptr_t address = static_cast<uintptr_t>(
        std::wcstoull(argv[2], nullptr, 0));
    if (!DebugActiveProcess(pid)) {
        std::fwprintf(stderr, L"DebugActiveProcess failed: %lu\n", GetLastError());
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    const ULONGLONG deadline = GetTickCount64() + 90000;
    bool captured = false;
    while (GetTickCount64() < deadline && !captured) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEvent(&event, 1000)) continue;
        DWORD status = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            ArmThread(event.dwThreadId, address);
            if (event.u.CreateProcessInfo.hFile) {
                CloseHandle(event.u.CreateProcessInfo.hFile);
            }
        } else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            ArmThread(event.dwThreadId, address);
        } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& exception = event.u.Exception.ExceptionRecord;
            if (exception.ExceptionCode == EXCEPTION_SINGLE_STEP) {
                HANDLE thread = OpenThread(THREAD_GET_CONTEXT, FALSE,
                                           event.dwThreadId);
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
                if (thread && GetThreadContext(thread, &context)) {
                    std::printf(
                        "write captured pid=%lu tid=%lu eip=%08lX "
                        "eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX "
                        "esi=%08lX edi=%08lX ebp=%08lX esp=%08lX dr6=%08lX\n",
                        pid, event.dwThreadId, context.Eip, context.Eax,
                        context.Ebx, context.Ecx, context.Edx, context.Esi,
                        context.Edi, context.Ebp, context.Esp, context.Dr6);
                    captured = true;
                }
                if (thread) CloseHandle(thread);
            } else {
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            captured = true;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
    }
    DebugActiveProcessStop(pid);
    if (!captured) {
        std::printf("timeout waiting for write to %08llX\n",
            static_cast<unsigned long long>(address));
        return 3;
    }
    return 0;
}
