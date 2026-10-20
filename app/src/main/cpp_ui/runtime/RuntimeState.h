#pragma once

#include <string>

#include "TemplateModels.h"

const char* BuildModeName(BuildMode mode);
void ApplyBuildMode(BuildMode mode);

namespace OverlayState {
void UpdateFromAndroid(bool overlayPermission, bool drawOverApps, bool surfaceActive,
                       bool appForeground, int width, int height);
void SetRendererReady(bool ready);
void SetSurfaceActive(bool active);
void SetTouchPassThrough(bool enabled);
void SetImguiReady(bool ready);
void SetOverlayError(const std::string& error);
void SetRendererError(const std::string& error);
bool IsReadyToRender();
}
