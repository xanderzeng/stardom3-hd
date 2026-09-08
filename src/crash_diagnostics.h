#pragma once
#include <windows.h>
namespace stardom {
void ConfigureCrashDiagnostics(bool enabled, const wchar_t* directory);
void RemoveCrashDiagnostics();
void ConfigureRuntimeErrorDiagnostics(bool enabled, HMODULE runtime);
}
