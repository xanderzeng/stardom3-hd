#pragma once
#include <windows.h>
#include <d3d9.h>
namespace stardom {
bool InstallNativeResourceRecovery(HMODULE executable);
void ReleaseNativeResourcesForReset(IDirect3DDevice9* device);
bool RestoreNativeResourcesAfterReset(IDirect3DDevice9* device);
}
