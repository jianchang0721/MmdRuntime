#include "DebugPanel.h"

#include <cstdio>
#include <string>

#include "AppState.h"
#include "ConfigStorage.h"
#include "OverlayWindowManager.h"
#include "RecoveryManager.h"
#include "RuntimeState.h"
#include "Theme.h"
#include "ToastManager.h"
#include "UIComponents.h"
#include "imgui.h"

namespace DebugPanel {
static const char* ScreenName(Screen screen) {
    switch (screen) {
        case Screen::Login: return "Login";
        case Screen::Games: return "Games";
        case Screen::Menu: return "Menu";
    }
    return "Unknown";
}

static void CountManifest(const FeatureManifest* manifest, int& tabs, int& sections, int& components) {
    tabs = sections = components = 0;
    if (!manifest) return;
    tabs = (int)manifest->tabs.size();
    for (const auto& tab : manifest->tabs) {
        sections += (int)tab.sections.size();
        components += (int)tab.components.size();
        for (const auto& section : tab.sections) components += (int)section.components.size();
    }
}

static void TextPair(const char* label, const std::string& value) {
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(190.0f);
    ImGui::TextUnformatted(value.c_str());
}

static void TextPair(const char* label, int value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%d", value);
    TextPair(label, buffer);
}

static void TextPair(const char* label, float value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.1f", value);
    TextPair(label, buffer);
}

void Render() {
    if (!g_BuildConfig.allowDebugPanel || !g_ShowDebugPanel) return;

    const FeatureManifest* manifest = g_ModuleManager.GetActiveManifest();
    int tabCount = 0, sectionCount = 0, componentCount = 0;
    CountManifest(manifest, tabCount, sectionCount, componentCount);

    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 620.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::WithAlpha(Theme::Colors::Panel, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::WithAlpha(Theme::Colors::Accent, 0.34f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::Sizes::ChildRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::Begin("Developer Debug", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);

    UI::Section("Overlay", "Template diagnostics");
    TextPair("FPS", ImGui::GetIO().Framerate);
    TextPair("Screen width", WindowManager::screenW);
    TextPair("Screen height", WindowManager::screenH);
    TextPair("Current screen", ScreenName(WindowManager::currentScreen));
    TextPair("Previous screen", ScreenName(WindowManager::previousScreen));
    TextPair("Active tab", g_ModuleState.activeTab);
    TextPair("Theme preset", Theme::PresetName(Theme::GetPreset()));
    TextPair("Toast count", Toast::Count());
    TextPair("Config status", g_LastConfigStatus);
    TextPair("Touch pass-through", g_TouchPassThroughEnabled ? "Enabled" : "Disabled");
    TextPair("Window x", WindowManager::lastWindowPosition.x);
    TextPair("Window y", WindowManager::lastWindowPosition.y);
    TextPair("Window width", WindowManager::lastWindowSize.x);
    TextPair("Window height", WindowManager::lastWindowSize.y);
    TextPair("Snapped", "Not active");
    TextPair("Overlay permission", g_OverlayState.overlayPermissionGranted ? "Granted" : "Missing");
    TextPair("Draw over apps", g_OverlayState.drawOverAppsEnabled ? "Enabled" : "Disabled");
    TextPair("Renderer ready", g_OverlayState.rendererReady ? "Yes" : "No");
    TextPair("Surface active", g_OverlayState.surfaceActive ? "Yes" : "No");
    TextPair("ImGui ready", g_OverlayState.imguiReady ? "Yes" : "No");
    TextPair("Orientation", g_OverlayState.landscape ? "Landscape" : "Portrait");
    TextPair("App foreground", g_OverlayState.appInForeground ? "Yes" : "No");
    TextPair("Overlay error", g_OverlayState.lastOverlayError);
    TextPair("Renderer error", g_OverlayState.lastRendererError);

    ImGui::Separator();
    UI::Section("Build", nullptr);
    TextPair("Build mode", BuildModeName(g_BuildConfig.mode));
    TextPair("Debug panel", g_BuildConfig.allowDebugPanel ? "Allowed" : "Blocked");
    TextPair("Config reset", g_BuildConfig.allowConfigReset ? "Allowed" : "Blocked");
    TextPair("Module reload", g_BuildConfig.allowModuleReload ? "Allowed" : "Blocked");
    TextPair("Verbose logs", g_BuildConfig.allowVerboseLogs ? "Allowed" : "Blocked");
    TextPair("Demo modules", g_BuildConfig.allowDemoModules ? "Allowed" : "Blocked");

    ImGui::Separator();
    UI::Section("Module", "Local module state");
    TextPair("Loaded", g_ModuleManager.HasActiveModule() ? "Yes" : "No");
    TextPair("Module id", manifest ? manifest->moduleId : "");
    TextPair("Module name", manifest ? manifest->moduleName : "");
    TextPair("Version", manifest ? manifest->version : "");
    TextPair("Game id", manifest ? manifest->gameId : "");
    TextPair("Manifest version", manifest ? manifest->manifestVersion : 0);
    TextPair("Min template", manifest ? manifest->minTemplateVersion : 0);
    TextPair("Tabs", tabCount);
    TextPair("Sections", sectionCount);
    TextPair("Components", componentCount);
    TextPair("Config path", manifest ? Config::GetModuleConfigPath(manifest->moduleId) : "");
    TextPair("Last error", g_ModuleManager.GetLastError());
    TextPair("Validation error", g_ModuleManager.GetLastValidationError());
    TextPair("Last game id", g_ModuleManager.GetLastSelectedGameId());
    TextPair("Last game name", g_ModuleManager.GetLastSelectedGameName());
    TextPair("Active profile", g_ActiveProfileId);

    ImGui::Separator();
    UI::Section("State", nullptr);
    TextPair("Bool values", (int)g_ModuleState.boolValues.size());
    TextPair("Float values", (int)g_ModuleState.floatValues.size());
    TextPair("Int values", (int)g_ModuleState.intValues.size());
    TextPair("String values", (int)g_ModuleState.stringValues.size());
    TextPair("Section states", (int)g_ModuleState.sectionOpenValues.size());
    TextPair("Recovery enabled", Recovery::enabled ? "Yes" : "No");
    TextPair("Recovery count", Recovery::recoveryCount);
    TextPair("Recovery frame", Recovery::lastRecoveryFrame);
    TextPair("Last recovery", Recovery::lastRecoveryAction);

    ImGui::Separator();
    UI::Section("Actions", nullptr);
    if (g_BuildConfig.allowModuleReload && UI::Button("Reload Active Module", ImVec2(210.0f, Theme::Sizes::ButtonHeight))) {
        if (g_ModuleManager.ReloadActiveModule()) Toast::Success("Module reloaded");
        else Toast::Error(g_ModuleManager.GetLastError());
    }
    if (g_BuildConfig.allowModuleReload) ImGui::SameLine();
    if (UI::Button("Unload Module", ImVec2(170.0f, Theme::Sizes::ButtonHeight))) {
        g_ModuleManager.UnloadCurrentModule();
        WindowManager::GoTo(Screen::Games);
        Toast::Info("Module unloaded");
    }
    if (UI::Button("Reset Module State", ImVec2(210.0f, Theme::Sizes::ButtonHeight))) {
        g_ModuleManager.ResetActiveModuleState();
        Toast::Success("Module state reset");
    }
    ImGui::SameLine();
    if (UI::Button("Save Config", ImVec2(170.0f, Theme::Sizes::ButtonHeight))) {
        if (manifest && Config::SaveModuleConfig(manifest->moduleId, g_ModuleState, manifest)) Toast::Success("Config saved");
        else Toast::Error("Config failed to save");
    }
    if (UI::Button("Load Config", ImVec2(210.0f, Theme::Sizes::ButtonHeight))) {
        if (manifest && Config::LoadModuleConfig(manifest->moduleId, g_ModuleState, manifest)) Toast::Success("Config loaded");
        else Toast::Warning("Config failed to load");
    }
    ImGui::SameLine();
    if (UI::Button("Return to Games", ImVec2(170.0f, Theme::Sizes::ButtonHeight))) {
        WindowManager::GoTo(Screen::Games);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
}
