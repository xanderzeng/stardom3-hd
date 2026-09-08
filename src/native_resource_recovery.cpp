#include "native_resource_recovery.h"
#include <cstdint>
#include <cstring>
#include <map>
#include <set>

namespace stardom {
void Log(const char*, ...);
namespace {
using Factory = void* (__thiscall*)(void*, UINT, UINT, UINT, D3DFORMAT);
using Destroy = void (__thiscall*)(void*);
using ManagerEvent = bool (__thiscall*)(void*);
Factory factory = nullptr;
Destroy destroy_texture = nullptr, invalidate_texture = nullptr;
ManagerEvent manager_lost = nullptr, manager_reset = nullptr;

// Native procedural textures (005C3710) bypass RenderWare's raster registry.
// Keep the native owner object stable while replacing only its D3D allocation.
struct Allocation {
    IDirect3DDevice9* device;
    void* manager;
    UINT width, height, levels;
    D3DFORMAT format;
    bool released = false;
};
std::map<void*, Allocation> allocations;
std::map<void*, IDirect3DDevice9*> suspended_managers;
template<class T> T& Field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}
void Track(void* object, void* manager, UINT width, UINT height,
           UINT levels, D3DFORMAT format) {
    if (!object || !Field<IDirect3DTexture9*>(object, 0x18)) return;
    allocations[object] = {Field<IDirect3DDevice9*>(object, 0x10), manager,
        width, height, levels, format};
}
void* __fastcall CreateTarget(void* manager, void*, UINT width, UINT height,
                              UINT levels, D3DFORMAT format) {
    void* object = factory(manager, width, height, levels, format);
    Track(object, manager, width, height, levels, format);
    return object;
}
void __fastcall DestroyTarget(void* object, void*) {
    allocations.erase(object);
    destroy_texture(object);
}
void __fastcall InvalidateTarget(void* object, void*) {
    allocations.erase(object);
    invalidate_texture(object);
}

struct EntryHook {
    unsigned char* entry = nullptr;
    unsigned char original[8]{};
    unsigned char* trampoline = nullptr;
    size_t size = 0;
    bool Install(unsigned char* target, const unsigned char* expected,
                 size_t length, void* replacement) {
        if (std::memcmp(target, expected, length)) return false;
        trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr,
            length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline) return false;
        std::memcpy(original, target, length);
        std::memcpy(trampoline, target, length);
        trampoline[length] = 0xE9;
        const auto back = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target + length) -
            reinterpret_cast<uintptr_t>(trampoline + length + 5));
        std::memcpy(trampoline + length + 1, &back, 4);
        DWORD protection;
        if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &protection)) {
            VirtualFree(trampoline, 0, MEM_RELEASE); trampoline = nullptr; return false;
        }
        target[0] = 0xE9;
        const auto jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(replacement) -
            reinterpret_cast<uintptr_t>(target + 5));
        std::memcpy(target + 1, &jump, 4);
        std::memset(target + 5, 0x90, length - 5);
        DWORD ignored;
        VirtualProtect(target, length, protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), trampoline, length + 5);
        FlushInstructionCache(GetCurrentProcess(), target, length);
        entry = target; size = length;
        return true;
    }
    void Rollback() {
        if (!entry) return;
        DWORD protection;
        if (VirtualProtect(entry, size, PAGE_EXECUTE_READWRITE, &protection)) {
            std::memcpy(entry, original, size);
            DWORD ignored;
            VirtualProtect(entry, size, protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, size);
        }
        entry = nullptr;
    }
};
EntryHook create_hook, destroy_hook, invalidate_hook;

// RenderWare can reach these texture consumers with a null raster after a
// failed Reset, even when its cached client dimensions still match. Clear must
// fail for this frame; camera begin must reach its existing cooperative-level
// recovery block, otherwise skipping the frame would prevent all future resets.
EntryHook clear_guard, camera_guard;
bool InstallRasterGuard(EntryHook& hook, unsigned char* entry,
                        const unsigned char* expected, bool camera,
                        unsigned char* unavailable, uint32_t device_slot = 0xA194D8) {
    auto* bridge = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!bridge) return false;
    size_t n = 0;
    bridge[n++] = 0x83;
    bridge[n++] = camera ? 0x7D : 0x3B; // cmp dword ptr [ebp/ebx], 0
    if (camera) bridge[n++] = 0;
    bridge[n++] = 0;
    bridge[n++] = 0x75; bridge[n++] = camera ? 10 : 5;
    if (camera) {
        bridge[n++] = 0xA1; // recovery expects EAX = the engine device
        std::memcpy(bridge+n, &device_slot, 4); n += 4;
    }
    bridge[n++] = 0xE9;
    const auto displacement = static_cast<int32_t>(reinterpret_cast<uintptr_t>(unavailable) -
        reinterpret_cast<uintptr_t>(bridge+n+4));
    std::memcpy(bridge+n, &displacement, 4); n += 4;
    std::memcpy(bridge+n, expected, 5); n += 5;
    bridge[n++] = 0xE9;
    const auto resume = static_cast<int32_t>(reinterpret_cast<uintptr_t>(entry+5) -
        reinterpret_cast<uintptr_t>(bridge+n+4));
    std::memcpy(bridge+n, &resume, 4); n += 4;
    FlushInstructionCache(GetCurrentProcess(), bridge, n);
    if (hook.Install(entry, expected, 5, bridge)) return true;
    VirtualFree(bridge, 0, MEM_RELEASE);
    return false;
}
}

bool InstallNativeResourceRecovery(HMODULE executable) {
    if (factory) return true;
    auto* base = reinterpret_cast<unsigned char*>(executable);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    // These SEH prologues embed absolute addresses; support only the inspected
    // non-relocated Stardom3 binary and refuse other games/proxy smoke hosts.
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        reinterpret_cast<uintptr_t>(base) != 0x400000 ||
        nt->OptionalHeader.SizeOfImage != 0x683000) return false;
    constexpr unsigned char make[] = {0x6A,0xFF,0x68,0xBA,0xAE,0x6E,0x00};
    constexpr unsigned char drop[] = {0x6A,0xFF,0x68,0xC6,0xAD,0x6E,0x00};
    constexpr unsigned char invalidate[] = {0x56,0x8B,0xF1,0x8B,0x46,0x18};
    constexpr unsigned char lost[] = {0x53,0x8B,0xD9,0x56,0x57,0x8D,0x7B,0x18};
    constexpr unsigned char reset[] = {0x83,0xEC,0x18,0x33,0xC0,0x55,0x8B,0xE9};
    constexpr unsigned char clear_raster[] = {0x8A,0x43,0x09,0xA8,0x0F};
    constexpr unsigned char camera_raster[] = {0x8A,0x45,0x09,0xA8,0x0F};
    constexpr unsigned char clear_failure[] = {0x5F,0x5E,0x5D,0x33,0xC0,0x5B,0x83,0xC4,0x68,0xC3};
    constexpr unsigned char camera_recovery[] = {0xA1,0xD8,0x94,0xA1,0x00,0x83,0xC4,0x10};
    if (std::memcmp(base+0x1C3710,make,sizeof(make)) ||
        std::memcmp(base+0x1C2930,drop,sizeof(drop)) ||
        std::memcmp(base+0x1C2990,invalidate,sizeof(invalidate)) ||
        std::memcmp(base+0x1C3080,lost,sizeof(lost)) ||
        std::memcmp(base+0x1C3180,reset,sizeof(reset)) ||
        std::memcmp(base+0x290F72,clear_raster,sizeof(clear_raster)) ||
        std::memcmp(base+0x292357,camera_raster,sizeof(camera_raster)) ||
        std::memcmp(base+0x291281,clear_failure,sizeof(clear_failure)) ||
        std::memcmp(base+0x292559,camera_recovery,sizeof(camera_recovery))) return false;
    // Enter after the caller-cleanup instruction at 0069255E: this guard has
    // not pushed the four render-state arguments that the normal path removes.
    constexpr unsigned char recovery_device[] = {0x8B,0x10,0x50,0xFF,0x52,0x0C};
    if (std::memcmp(base+0x292561,recovery_device,sizeof(recovery_device))) return false;
    if (!destroy_hook.Install(base+0x1C2930,drop,sizeof(drop),reinterpret_cast<void*>(&DestroyTarget)) ||
        !invalidate_hook.Install(base+0x1C2990,invalidate,sizeof(invalidate),reinterpret_cast<void*>(&InvalidateTarget)) ||
        !create_hook.Install(base+0x1C3710,make,sizeof(make),reinterpret_cast<void*>(&CreateTarget)) ||
        !InstallRasterGuard(clear_guard, base+0x290F72, clear_raster, false, base+0x291281) ||
        !InstallRasterGuard(camera_guard, base+0x292357, camera_raster, true, base+0x292561)) {
        camera_guard.Rollback(); clear_guard.Rollback();
        create_hook.Rollback(); invalidate_hook.Rollback(); destroy_hook.Rollback();
        return false;
    }
    destroy_texture = reinterpret_cast<Destroy>(destroy_hook.trampoline);
    invalidate_texture = reinterpret_cast<Destroy>(invalidate_hook.trampoline);
    factory = reinterpret_cast<Factory>(create_hook.trampoline);
    manager_lost = reinterpret_cast<ManagerEvent>(base+0x1C3080);
    manager_reset = reinterpret_cast<ManagerEvent>(base+0x1C3180);
    Log("Installed native procedural texture/device recovery");
    return true;
}

void ReleaseNativeResourcesForReset(IDirect3DDevice9* device) {
    std::set<void*> managers;
    for (const auto& [object, allocation] : allocations)
        if (allocation.device == device && allocation.manager)
            managers.insert(allocation.manager);
    // Native manager owns additional DEFAULT vertex/index buffers. Its own
    // loss/reset methods preserve their descriptions, unlike deleting it.
    for (void* manager : managers) {
        if (!suspended_managers.count(manager)) {
            suspended_managers[manager] = device;
            if (manager_lost) manager_lost(manager);
        }
    }
    for (auto& [object, allocation] : allocations) {
        if (allocation.device != device || allocation.released) continue;
        auto*& texture = Field<IDirect3DTexture9*>(object, 0x18);
        if (texture) { texture->Release(); texture = nullptr; }
        allocation.released = true;
    }
}

bool RestoreNativeResourcesAfterReset(IDirect3DDevice9* device) {
    bool ok = true;
    for (auto it = suspended_managers.begin(); it != suspended_managers.end();) {
        if (it->second != device) { ++it; continue; }
        if (manager_reset) ok = manager_reset(it->first) && ok;
        it = suspended_managers.erase(it);
    }
    for (auto& [object, allocation] : allocations) {
        if (allocation.device != device || !allocation.released) continue;
        IDirect3DTexture9* texture = nullptr;
        const HRESULT result = device->CreateTexture(allocation.width,
            allocation.height, allocation.levels, D3DUSAGE_RENDERTARGET,
            allocation.format, D3DPOOL_DEFAULT, &texture, nullptr);
        if (FAILED(result)) { ok = false; continue; }
        Field<IDirect3DTexture9*>(object, 0x18) = texture;
        allocation.released = false;
    }
    if (!ok) Log("Native procedural resource recreation incomplete");
    return ok;
}
}
