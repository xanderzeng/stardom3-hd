#include "crash_diagnostics.h"
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace stardom {
bool IsDebugModeEnabled();
void Log(const char*, ...);
namespace {
PVOID handler = nullptr;
HMODULE dbghelp = nullptr;
using DumpFn = decltype(&MiniDumpWriteDump);
DumpFn write_dump = nullptr;
wchar_t dump_path[MAX_PATH]{};
volatile LONG captured = 0;
unsigned char* runtime_error_entry = nullptr;
void* runtime_error_trampoline = nullptr;
unsigned char runtime_error_original[5]{};
unsigned runtime_errors = 0;

void __cdecl RecordRuntimeError(const char* message, const char* file, unsigned line) {
    if (!IsDebugModeEnabled() || ++runtime_errors > 64) return;
    Log("D3D9 validation failure: %.600s file=%.200s line=%u",
        message ? message : "(null)", file ? file : "(null)", line);
}

// The runtime uses ECX/EDX plus a caller-cleaned stack argument. Preserve all
// registers/flags and forward through the original five-byte prologue.
__declspec(naked) void ObserveRuntimeError() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+40]
        push edx
        push ecx
        call RecordRuntimeError
        add esp, 12
        popad
        popfd
        jmp dword ptr [runtime_error_trampoline]
    }
}

void RestoreRuntimeErrorHook() {
    if (!runtime_error_entry) return;
    DWORD previous = 0;
    if (VirtualProtect(runtime_error_entry, 5, PAGE_EXECUTE_READWRITE, &previous)) {
        std::memcpy(runtime_error_entry, runtime_error_original, 5);
        DWORD ignored = 0;
        VirtualProtect(runtime_error_entry, 5, previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), runtime_error_entry, 5);
        runtime_error_entry = nullptr;
    }
}

LONG CALLBACK CaptureKnownCrash(EXCEPTION_POINTERS* exception) {
    if (!IsDebugModeEnabled() || !write_dump || !exception ||
        !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto address = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    // Capture only the observed game fault, not handled first-chance AVs.
    if (address != base + 0x290FA2 || InterlockedCompareExchange(&captured, 1, 0))
        return EXCEPTION_CONTINUE_SEARCH;
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
        const BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithThreadInfo |
                                      MiniDumpWithUnloadedModules), &info, nullptr, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        Log("Known crash dump: success=%d error=%lu EIP=%08lX EAX=%08lX EBX=%08lX", ok, error,
            exception->ContextRecord->Eip, exception->ContextRecord->Eax, exception->ContextRecord->Ebx);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}

void RemoveCrashDiagnostics() {
    RestoreRuntimeErrorHook();
    if (handler) { RemoveVectoredExceptionHandler(handler); handler = nullptr; }
    // Keep dbghelp loaded until process exit, avoiding loader work during detach.
}

void ConfigureRuntimeErrorDiagnostics(bool enabled, HMODULE runtime) {
    if (!enabled || !IsDebugModeEnabled()) { RestoreRuntimeErrorHook(); return; }
    if (!runtime || runtime_error_entry) return;
    auto* base = reinterpret_cast<unsigned char*>(runtime);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    // Temporary diagnostic for the exact runtime inspected in the crash dump.
    // Refuse all other binaries rather than guessing private function addresses.
    if (nt->FileHeader.TimeDateStamp != 0x7678A845 ||
        nt->OptionalHeader.SizeOfImage != 0x17A000) {
        Log("D3D9 validation capture: runtime version unsupported"); return;
    }
    constexpr unsigned char signature[] = {0x8b,0xff,0x55,0x8b,0xec,0xff,0x75,0x08,0x52,0x8b,0xd1,0xb9,0xef,0xbe,0xad,0xde};
    auto* entry = base + 0xB51E4;
    if (std::memcmp(entry, signature, sizeof(signature))) {
        Log("D3D9 validation capture: signature mismatch"); return;
    }
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 10,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline) return;
    std::memcpy(trampoline, entry, 5);
    trampoline[5] = 0xe9;
    const auto back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry + 5) -
        reinterpret_cast<uintptr_t>(trampoline + 10));
    std::memcpy(trampoline + 6, &back, 4);
    DWORD previous = 0;
    if (!VirtualProtect(trampoline, 10, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(trampoline, 0, MEM_RELEASE); return;
    }
    if (!VirtualProtect(entry, 5, PAGE_EXECUTE_READWRITE, &previous)) {
        VirtualFree(trampoline, 0, MEM_RELEASE); return;
    }
    std::memcpy(runtime_error_original, entry, 5);
    runtime_error_trampoline = trampoline;
    unsigned char patch[5] = {0xe9};
    const auto target = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&ObserveRuntimeError) -
        reinterpret_cast<uintptr_t>(entry + 5));
    std::memcpy(patch + 1, &target, 4);
    std::memcpy(entry, patch, 5);
    DWORD ignored = 0;
    VirtualProtect(entry, 5, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 10);
    FlushInstructionCache(GetCurrentProcess(), entry, 5);
    runtime_error_entry = entry;
    Log("D3D9 validation capture installed (DebugMode only)");
}

void ConfigureCrashDiagnostics(bool enabled, const wchar_t* directory) {
    if (!enabled || !IsDebugModeEnabled()) { RemoveCrashDiagnostics(); return; }
    if (handler) return;
    if (!dbghelp) {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH - 13) return;
        wcscat_s(path, L"\\dbghelp.dll");
        dbghelp = LoadLibraryW(path);
    }
    if (!dbghelp) { Log("Crash diagnostics: cannot load system dbghelp"); return; }
    write_dump = reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!write_dump) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (_snwprintf_s(dump_path, MAX_PATH, _TRUNCATE,
        L"%s\\Stardom3.Crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", directory,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId()) < 0) return;
    captured = 0;
    handler = AddVectoredExceptionHandler(1, CaptureKnownCrash);
    Log("Crash diagnostics: known-fault dump armed=%d path=%ls", handler != nullptr, dump_path);
}
}
