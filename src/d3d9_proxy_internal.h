#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <string>

#include "display_config.h"
#include "proxy_config.h"
#include "ui_geometry.h"
#include "ui_layout_state.h"

namespace stardom {

extern ULONGLONG g_attach_tick;

void Log(const char* format, ...);
void LogProbeBytes(const unsigned char* address, size_t count);

bool CanReadGuiObject(const void* object);
void RefreshUnifiedUILayout();
void RefreshUnifiedUILayoutNow();
void RefreshInGameCGOverlays();
bool InstallUnifiedUILayoutHook(UINT width, UINT height, int title_screen_mode);
bool PatchMapLocationProjectionBounds(UINT width, UINT height);
bool PatchAirportLocationLabelFilter();
bool PatchMapLocationVisibilityGuards();
bool InstallMapLocationDiagnosticHook();
void GetPhotoAlbumViewport(UINT output_width, UINT output_height,
                           int& x, int& y, int& width, int& height);
bool IsPhotoAlbumViewportNeeded();
bool IsPhotoAlbumCGVisible();
bool IsPhotoAlbumScreenVisible();
bool IsInGameCGVisible();
bool IsAnnouncementScreenVisible();
bool IsTrainingScreenPillarboxNeeded();
bool IsTitleScreenVisible();
bool IsTitleTutorialVisible();

bool InstallUIViewportHook(IDirect3DDevice9* device, UINT width, UINT height,
                           int ui_scale_mode);
bool InstallUIDrawHooks(IDirect3DDevice9* device, bool diagnostics,
                        bool suppress_transparent, bool container_probe,
                        bool suppress_proxy_containers, bool gui_runtime_probe);

bool InstallOpeningVideoHooks();
bool IsOpeningTitleTransitionPending();

}  // namespace stardom
