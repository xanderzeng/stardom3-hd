#pragma once

#include <windows.h>
#include <d3d9.h>

namespace stardom {

// Only active inside the verified native, five-pass glyph draw routine.
bool InstallFontOutlineHook(HMODULE executable);

// Restricts added contrast to the inventory description currently rendering.
class InventoryTextContrast {
public:
    explicit InventoryTextContrast(bool enabled);
    ~InventoryTextContrast();
    InventoryTextContrast(const InventoryTextContrast&) = delete;
    InventoryTextContrast& operator=(const InventoryTextContrast&) = delete;
private:
    bool saved_;
};

// Restores every modified D3D state on destruction, before returning to the game.
class FontOutlineDraw {
public:
    FontOutlineDraw(IDirect3DDevice9* device, const void* vertices,
                    UINT count, UINT stride);
    // The native glyph's vertices are also the source of the game's dynamic
    // vertex-buffer upload. Avoid reading back WRITEONLY GPU buffers.
    FontOutlineDraw(IDirect3DDevice9* device, UINT native_vertex_count);
    ~FontOutlineDraw();
    FontOutlineDraw(const FontOutlineDraw&) = delete;
    FontOutlineDraw& operator=(const FontOutlineDraw&) = delete;
private:
    IDirect3DDevice9* device_ = nullptr;
    float constants_[9 * 4]{};
};

} // namespace stardom
