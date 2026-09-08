#include "../src/crash_diagnostics.cpp"
#include <vector>
namespace stardom {
bool debug_test = false;
bool IsDebugModeEnabled() { return debug_test; }
void Log(const char*, ...) {}
}
bool TestRuntimeErrorHook() {
    using namespace stardom;
    auto* fixture = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x17a000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!fixture) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(fixture);
    dos->e_lfanew = 0x100;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(fixture + 0x100);
    nt->FileHeader.TimeDateStamp = 0x7678a845;
    nt->OptionalHeader.SizeOfImage = 0x17a000;
    constexpr unsigned char body[] = {0x8b,0xff,0x55,0x8b,0xec,0xff,0x75,0x08,0x52,0x8b,0xd1,0xb9,0xef,0xbe,0xad,0xde,
        0xb8,0x78,0x56,0x34,0x12,0x83,0xc4,0x08,0x5d,0xc3};
    auto* entry = fixture + 0xb51e4;
    std::memcpy(entry, body, sizeof(body));
    const HMODULE module = reinterpret_cast<HMODULE>(fixture);
    debug_test = false;
    ConfigureRuntimeErrorDiagnostics(true, module);
    bool ok = !runtime_error_entry && std::memcmp(entry, body, sizeof(body)) == 0;
    debug_test = true;
    nt->FileHeader.TimeDateStamp = 0;
    ConfigureRuntimeErrorDiagnostics(true, module);
    ok = ok && !runtime_error_entry;
    nt->FileHeader.TimeDateStamp = 0x7678a845;
    ConfigureRuntimeErrorDiagnostics(true, module);
    ok = ok && runtime_error_entry == entry;
    const char* message = "test validation";
    const char* filename = "test.cpp";
    unsigned result = 0, stack_before = 0, stack_after = 0;
    __asm {
        mov stack_before, esp
        push 123
        mov ecx, message
        mov edx, filename
        call entry
        add esp, 4
        mov result, eax
        mov stack_after, esp
    }
    ok = ok && result == 0x12345678 && stack_before == stack_after && runtime_errors == 1;
    ConfigureRuntimeErrorDiagnostics(false, module);
    ok = ok && !runtime_error_entry && std::memcmp(entry, body, sizeof(body)) == 0;
    VirtualFree(fixture, 0, MEM_RELEASE);
    return ok;
}
int main() {
    using namespace stardom;
    wchar_t directory[MAX_PATH]{};
    if (!GetCurrentDirectoryW(MAX_PATH, directory)) return 1;
    ConfigureCrashDiagnostics(false, directory);
    if (handler || dbghelp) return 2;
    debug_test = true;
    ConfigureCrashDiagnostics(true, directory);
    if (!handler || !write_dump) return 3;
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(0x1234);
    EXCEPTION_POINTERS pointers{&record, &context};
    if (CaptureKnownCrash(&pointers) != EXCEPTION_CONTINUE_SEARCH || captured) return 4;
    record.ExceptionAddress = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x290FA2);
    debug_test = false;
    CaptureKnownCrash(&pointers);
    if (captured || GetFileAttributesW(dump_path) != INVALID_FILE_ATTRIBUTES) return 5;
    debug_test = true;
    if (CaptureKnownCrash(&pointers) != EXCEPTION_CONTINUE_SEARCH || captured != 1) return 6;
    HANDLE file = CreateFileW(dump_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 7;
    MINIDUMP_HEADER header{};
    DWORD read = 0;
    bool valid = ReadFile(file, &header, sizeof(header), &read, nullptr) &&
        read == sizeof(header) && header.Signature == MINIDUMP_SIGNATURE;
    std::vector<MINIDUMP_DIRECTORY> streams(header.NumberOfStreams);
    SetFilePointer(file, header.StreamDirectoryRva, nullptr, FILE_BEGIN);
    valid = valid && ReadFile(file, streams.data(),
        static_cast<DWORD>(streams.size() * sizeof(MINIDUMP_DIRECTORY)), &read, nullptr);
    bool exception_found = false;
    for (const auto& stream : streams) {
        if (stream.StreamType != ExceptionStream) continue;
        MINIDUMP_EXCEPTION_STREAM saved{};
        SetFilePointer(file, stream.Location.Rva, nullptr, FILE_BEGIN);
        exception_found = ReadFile(file, &saved, sizeof(saved), &read, nullptr) &&
            saved.ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            saved.ExceptionRecord.ExceptionAddress == reinterpret_cast<uintptr_t>(record.ExceptionAddress) &&
            saved.ThreadId == GetCurrentThreadId();
    }
    CloseHandle(file);
    CaptureKnownCrash(&pointers); // One capture per process; never overwrite.
    RemoveCrashDiagnostics();
    valid = valid && exception_found && !handler;
    DeleteFileW(dump_path);
    std::printf("debug gating, unrelated exception filtering, dump exception stream: %s\n", valid ? "PASS" : "FAIL");
    return valid && TestRuntimeErrorHook() ? 0 : 8;
}
