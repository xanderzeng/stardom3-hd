#pragma once

#include <windows.h>
#include <d3d9.h>

namespace stardom {

using SetViewportFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetRenderTargetFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using BeginSceneFn = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PresentFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using DrawPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT (STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
    D3DFORMAT, const void*, UINT);

struct DeviceHookState {
    IDirect3DDevice9* device = nullptr;
    SetViewportFn original_set_viewport = nullptr;
    SetRenderTargetFn original_set_render_target = nullptr;
    BeginSceneFn original_begin_scene = nullptr;
    PresentFn original_present = nullptr;
    DrawPrimitiveFn original_draw_primitive = nullptr;
    DrawIndexedPrimitiveFn original_draw_indexed_primitive = nullptr;
    DrawPrimitiveUPFn original_draw_primitive_up = nullptr;
    DrawIndexedPrimitiveUPFn original_draw_indexed_primitive_up = nullptr;
    void** original_vtable = nullptr;
    UINT width = 0;
    UINT height = 0;
    bool title_pillarbox_cleared = false;
    bool title_pillarbox_logged = false;
    bool title_ready_before_draw = false;
    bool title_transition_mask_logged = false;
    UINT active_target_width = 0;
    UINT active_target_height = 0;
    IDirect3DSurface9* main_target_surface = nullptr;
    bool active_target_is_main = true;
    int ui_scale_mode = 0;
    volatile LONG logged_first_ui_viewport = 0;
    volatile LONG ui_draw_log_count = 0;
    volatile LONG suppressed_transparent_draws = 0;
    volatile LONG probed_container_draws = 0;
    bool suppress_transparent_ui = false;
    bool ui_draw_diagnostics = false;
    bool ui_container_probe = false;
    bool suppress_proxy_containers = false;
    bool gui_runtime_probe = false;
    volatile LONG gui_runtime_probe_done = 0;
};

extern DeviceHookState g_device_hook;

}  // namespace stardom
