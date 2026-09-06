#include "outline_bridge.h"
#include "font_hook.h"
#include "font_outline.h"
#include "gui_object.h"
#include <map>
#include <vector>
#include <cstring>
#include <cstdint>

// Standalone has no development probes or diagnostic output. Shared renderer
// diagnostics are disabled here; normal execution performs no diagnostic I/O.
namespace stardom {
bool IsDebugModeEnabled() { return false; }
void Log(const char*, ...) {}
}
namespace stardom_font {
namespace {
struct Patch { void** slot; void* before; void* after; };
std::vector<Patch> patches;
std::map<void**, void*> originals;
bool PatchSlot(void** slot, void* hook) {
    if (*slot == hook) return true;
    DWORD protection;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    originals[slot] = *slot;
    patches.push_back({slot, *slot, hook});
    *slot = hook;
    DWORD ignored;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);
    return true;
}
template<class Fn, class Object> Fn Original(Object* object, unsigned index) {
    return reinterpret_cast<Fn>(originals.at(*reinterpret_cast<void***>(object) + index));
}
using DP = HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DIP = HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using UP = HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
using IUP = HRESULT(WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT,UINT,const void*,D3DFORMAT,const void*,UINT);
HRESULT WINAPI Draw(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT start,UINT n) {
    stardom::FontOutlineDraw scope(d, n==2 && (t==D3DPT_TRIANGLEFAN || t==D3DPT_TRIANGLESTRIP) ? 4 : 0);
    return Original<DP>(d,81)(d,t,start,n);
}
HRESULT WINAPI DrawIndexed(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,INT base,UINT min,UINT vertices,UINT start,UINT n) {
    stardom::FontOutlineDraw scope(d,n==2 && vertices==4 ? 4 : 0);
    return Original<DIP>(d,82)(d,t,base,min,vertices,start,n);
}
HRESULT WINAPI DrawUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT n,const void* v,UINT stride) {
    stardom::FontOutlineDraw scope(d,v,n==2 && (t==D3DPT_TRIANGLEFAN || t==D3DPT_TRIANGLESTRIP) ? 4 : 0,stride);
    return Original<UP>(d,83)(d,t,n,v,stride);
}
HRESULT WINAPI DrawIndexedUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE t,UINT min,UINT vertices,UINT n,const void* indices,D3DFORMAT format,const void* v,UINT stride) {
    stardom::FontOutlineDraw scope(d,v,n==2 && vertices==4 ? 4 : 0,stride);
    return Original<IUP>(d,84)(d,t,min,vertices,n,indices,format,v,stride);
}
void AttachDevice(IDirect3DDevice9* d) {
    auto** v = *reinterpret_cast<void***>(d);
    PatchSlot(v+81,reinterpret_cast<void*>(&Draw));
    PatchSlot(v+82,reinterpret_cast<void*>(&DrawIndexed));
    PatchSlot(v+83,reinterpret_cast<void*>(&DrawUP));
    PatchSlot(v+84,reinterpret_cast<void*>(&DrawIndexedUP));
}
using Create = HRESULT(WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
HRESULT WINAPI CreateDevice(IDirect3D9* d,UINT a,D3DDEVTYPE t,HWND w,DWORD b,D3DPRESENT_PARAMETERS* p,IDirect3DDevice9** out) {
    const HRESULT hr=Original<Create>(d,16)(d,a,t,w,b,p,out);
    if(SUCCEEDED(hr) && out && *out) AttachDevice(*out);
    return hr;
}
using CreateEx = HRESULT(WINAPI*)(IDirect3D9Ex*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,D3DDISPLAYMODEEX*,IDirect3DDevice9Ex**);
HRESULT WINAPI CreateDeviceEx(IDirect3D9Ex* d,UINT a,D3DDEVTYPE t,HWND w,DWORD b,D3DPRESENT_PARAMETERS* p,D3DDISPLAYMODEEX* mode,IDirect3DDevice9Ex** out) {
    const HRESULT hr=Original<CreateEx>(d,20)(d,a,t,w,b,p,mode,out);
    if(SUCCEEDED(hr) && out && *out) AttachDevice(*out);
    return hr;
}

using TextDraw = void(__thiscall*)(void*,void*,DWORD,DWORD);
TextDraw label_draw=nullptr, memo_draw=nullptr;
struct Detour { unsigned char* target; unsigned char bytes[10]; size_t count; void* trampoline; };
std::vector<Detour> detours;
bool NativeCard(void* object,int width,int height) {
    using namespace stardom;
    for(int depth=0; CanReadGuiObject(object) && depth<6; ++depth) {
        if(GuiField<int>(object,GuiObjectField::width)==width &&
           GuiField<int>(object,GuiObjectField::height)==height) {
            void* root=GuiPointer(object,GuiObjectField::parent);
            return CanReadGuiObject(root) && GuiField<int>(root,GuiObjectField::width)==800 &&
                GuiField<int>(root,GuiObjectField::height)==600;
        }
        object=GuiPointer(object,GuiObjectField::parent);
    }
    return false;
}
void __fastcall Label(void* self,void*,void* renderer,DWORD time,DWORD flags) {
    stardom::GuiObjectReadBatch batch;
    stardom::InventoryTextContrast scope(NativeCard(self,720,537),true);
    label_draw(self,renderer,time,flags);
}
void __fastcall Memo(void* self,void*,void* renderer,DWORD time,DWORD flags) {
    using namespace stardom;
    GuiObjectReadBatch batch;
    InventoryTextContrast scope(CanReadGuiObject(self) &&
        GuiField<int>(self,GuiObjectField::width)==200 &&
        GuiField<int>(self,GuiObjectField::height)==60 && NativeCard(self,420,542));
    memo_draw(self,renderer,time,flags);
}
bool HookText(unsigned char* target,const unsigned char* expected,size_t count,void* hook,TextDraw& original) {
    if(original) return true;
    if(std::memcmp(target,expected,count)) return false;
    auto* trampoline=static_cast<unsigned char*>(VirtualAlloc(nullptr,count+5,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!trampoline) return false;
    std::memcpy(trampoline,target,count);
    trampoline[count]=0xE9;
    *reinterpret_cast<int32_t*>(trampoline+count+1)=static_cast<int32_t>((target+count)-(trampoline+count+5));
    DWORD protection;
    if(!VirtualProtect(target,count,PAGE_EXECUTE_READWRITE,&protection)) {VirtualFree(trampoline,0,MEM_RELEASE);return false;}
    Detour record{}; record.target=target;record.count=count;record.trampoline=trampoline;
    std::memcpy(record.bytes,target,count);detours.push_back(record);
    original=reinterpret_cast<TextDraw>(trampoline);
    target[0]=0xE9;
    *reinterpret_cast<int32_t*>(target+1)=static_cast<int32_t>(static_cast<unsigned char*>(hook)-target-5);
    std::memset(target+5,0x90,count-5);
    DWORD ignored;VirtualProtect(target,count,protection,&ignored);
    FlushInstructionCache(GetCurrentProcess(),target,count);
    FlushInstructionCache(GetCurrentProcess(),trampoline,count+5);
    return true;
}
}
void AttachOutline(IDirect3D9* d,bool extended) {
    if(!d || !OutlineEnabled()) return;
    // Unrecognised executables retain plain font replacement, no native hooks.
    if(!stardom::InstallFontOutlineHook(GetModuleHandleW(nullptr))) return;
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const unsigned char label[]={0x8B,0x44,0x24,0x0C,0x83,0xEC,0x34};
    const unsigned char memo[]={0x8B,0x44,0x24,0x0C,0x81,0xEC,0x3C,1,0,0};
    HookText(base+0x130310,label,sizeof(label),reinterpret_cast<void*>(&Label),label_draw);
    HookText(base+0x133700,memo,sizeof(memo),reinterpret_cast<void*>(&Memo),memo_draw);
    auto** v=*reinterpret_cast<void***>(d);
    PatchSlot(v+16,reinterpret_cast<void*>(&CreateDevice));
    if(extended) PatchSlot(v+20,reinterpret_cast<void*>(&CreateDeviceEx));
}
void DetachOutline() {
    for(auto i=patches.rbegin();i!=patches.rend();++i) {
        DWORD p;
        if(*i->slot==i->after && VirtualProtect(i->slot,sizeof(void*),PAGE_READWRITE,&p)) {
            *i->slot=i->before; DWORD ignored;VirtualProtect(i->slot,sizeof(void*),p,&ignored);
        }
    }
    patches.clear();originals.clear();
    for(auto& d:detours) {DWORD p;if(VirtualProtect(d.target,d.count,PAGE_EXECUTE_READWRITE,&p)) {
        std::memcpy(d.target,d.bytes,d.count);DWORD ignored;VirtualProtect(d.target,d.count,p,&ignored);
        FlushInstructionCache(GetCurrentProcess(),d.target,d.count);VirtualFree(d.trampoline,0,MEM_RELEASE);
    }}
    detours.clear();label_draw=nullptr;memo_draw=nullptr;
    stardom::UninstallFontOutlineHook(GetModuleHandleW(nullptr));
}
}
