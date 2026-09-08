#include "../src/native_resource_recovery.cpp"
#include <cstdio>
#include <cstdlib>
namespace stardom { void Log(const char*, ...) {} }
using namespace stardom;
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
int main() {
    Check(!InstallNativeResourceRecovery(GetModuleHandleW(nullptr)), "reject non-game binary");
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path,L"\\d3d9.dll");
    const HMODULE runtime = LoadLibraryW(path);
    using Make = IDirect3D9* (WINAPI*)(UINT);
    auto* d3d = reinterpret_cast<Make>(GetProcAddress(runtime,"Direct3DCreate9"))(D3D_SDK_VERSION);
    const HWND window = CreateWindowW(L"STATIC",L"Native recovery test",WS_OVERLAPPEDWINDOW,
        0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS p{};
    p.BackBufferWidth=320; p.BackBufferHeight=240; p.Windowed=TRUE;
    p.SwapEffect=D3DSWAPEFFECT_DISCARD; p.hDeviceWindow=window;
    IDirect3DDevice9* device=nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,&p,&device)), "create real device");
    alignas(void*) unsigned char owner[0x2c]{};
    Field<IDirect3DDevice9*>(owner,0x10)=device;
    auto& target=Field<IDirect3DTexture9*>(owner,0x18);
    Check(SUCCEEDED(device->CreateTexture(32,16,1,D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&target,nullptr)),"create procedural target");
    Track(owner,nullptr,32,16,1,D3DFMT_A8R8G8B8);
    IDirect3DTexture9* blocker=nullptr;
    Check(SUCCEEDED(device->CreateTexture(8,8,1,D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&blocker,nullptr)),"create external blocker");
    ReleaseNativeResourcesForReset(device);
    Check(!target && allocations.at(owner).released,"release and preserve native owner");
    Check(FAILED(device->Reset(&p)),"external resource still blocks reset");
    ReleaseNativeResourcesForReset(device);
    Check(!target,"failed reset retry does not double release");
    blocker->Release();
    p.BackBufferWidth=320; p.BackBufferHeight=240;
    Check(SUCCEEDED(device->Reset(&p)),"reset succeeds after last blocker released");
    Check(RestoreNativeResourcesAfterReset(device) && target,"recreate native target");
    D3DSURFACE_DESC desc{};
    Check(SUCCEEDED(target->GetLevelDesc(0,&desc)) && desc.Width==32 && desc.Height==16,
        "preserve native allocation dimensions");
    IDirect3DSurface9* surface=nullptr;
    Check(SUCCEEDED(target->GetSurfaceLevel(0,&surface)),"recreated surface available");
    Check(SUCCEEDED(device->ColorFill(surface,nullptr,0xff123456)),"recreated surface renderable");
    surface->Release();
    ReleaseNativeResourcesForReset(device);
    p.BackBufferWidth=320; p.BackBufferHeight=240;
    Check(SUCCEEDED(device->Reset(&p)) && RestoreNativeResourcesAfterReset(device),
        "second full recovery succeeds");
    allocations.erase(owner); target->Release();
    Check(device->Release()==0,"no diagnostic or recovery device reference leak");
    d3d->Release(); DestroyWindow(window);
    std::puts("native resource recovery passed");
}
