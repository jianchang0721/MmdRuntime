#include "RuntimeState.h"

#include <algorithm>

#include "AppState.h"

const char* BuildModeName(BuildMode mode) {
    switch (mode) {
        case BuildMode::Debug: return "Debug";
        case BuildMode::Development: return "Development";
        case BuildMode::Demo: return "Demo";
        case BuildMode::Release: return "Release";
    }
    return "Development";
}

void ApplyBuildMode(BuildMode mode) {
    g_BuildConfig.mode = mode;
    g_BuildConfig.strictManifestValidation = true;
    switch (mode) {
        case BuildMode::Debug:
            g_BuildConfig.allowDebugPanel = true;
            g_BuildConfig.allowConfigReset = true;
            g_BuildConfig.allowModuleReload = true;
            g_BuildConfig.allowVerboseLogs = true;
            g_BuildConfig.allowDemoModules = true;
            g_BuildConfig.showTemplateWatermark = false;
            g_ShowDebugPanel = true;
            break;
        case BuildMode::Demo:
            g_BuildConfig.allowDebugPanel = false;
            g_BuildConfig.allowConfigReset = true;
            g_BuildConfig.allowModuleReload = false;
            g_BuildConfig.allowVerboseLogs = false;
            g_BuildConfig.allowDemoModules = true;
            g_BuildConfig.showTemplateWatermark = false;
            g_ShowDebugPanel = false;
            break;
        case BuildMode::Release:
            g_BuildConfig.allowDebugPanel = false;
            g_BuildConfig.allowConfigReset = false;
            g_BuildConfig.allowModuleReload = false;
            g_BuildConfig.allowVerboseLogs = false;
            g_BuildConfig.allowDemoModules = true;
            g_BuildConfig.showTemplateWatermark = false;
            g_ShowDebugPanel = false;
            break;
        case BuildMode::Development:
        default:
            g_BuildConfig.allowDebugPanel = true;
            g_BuildConfig.allowConfigReset = true;
            g_BuildConfig.allowModuleReload = true;
            g_BuildConfig.allowVerboseLogs = false;
            g_BuildConfig.allowDemoModules = true;
            g_BuildConfig.showTemplateWatermark = false;
            g_ShowDebugPanel = true;
            break;
    }
}

namespace OverlayState {
void UpdateFromAndroid(bool overlayPermission, bool drawOverApps, bool surfaceActive,
                       bool appForeground, int width, int height) {
    g_OverlayState.overlayPermissionGranted = overlayPermission;
    g_OverlayState.drawOverAppsEnabled = drawOverApps;
    g_OverlayState.surfaceActive = surfaceActive;
    g_OverlayState.appInForeground = appForeground;
    g_OverlayState.screenWidth = std::max(1, width);
    g_OverlayState.screenHeight = std::max(1, height);
    g_OverlayState.landscape = width > height;
    if (!overlayPermission || !drawOverApps) {
        g_OverlayState.lastOverlayError = "Overlay permission missing";
    } else if (g_OverlayState.lastOverlayError == "Overlay permission missing") {
        g_OverlayState.lastOverlayError.clear();
    }
}

void SetRendererReady(bool ready) { g_OverlayState.rendererReady = ready; }
void SetSurfaceActive(bool active) { g_OverlayState.surfaceActive = active; }
void SetTouchPassThrough(bool enabled) {
    g_TouchPassThroughEnabled = enabled;
    g_OverlayState.touchPassThroughEnabled = enabled;
}
void SetImguiReady(bool ready) { g_OverlayState.imguiReady = ready; }
void SetOverlayError(const std::string& error) { g_OverlayState.lastOverlayError = error; }
void SetRendererError(const std::string& error) { g_OverlayState.lastRendererError = error; }
bool IsReadyToRender() {
    return g_OverlayState.rendererReady && g_OverlayState.surfaceActive && g_OverlayState.imguiReady;
}
}
