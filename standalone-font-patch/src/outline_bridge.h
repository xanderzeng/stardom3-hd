#pragma once
#include <d3d9.h>
namespace stardom_font {
void AttachOutline(IDirect3D9* d3d, bool extended = false);
void DetachOutline();
}
