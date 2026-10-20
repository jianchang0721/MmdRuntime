#include "RecoveryManager.h"

#include <algorithm>
#include <unordered_set>

#include "AppState.h"
#include "FeatureFactory.h"
#include "OverlayWindowManager.h"
#include "Theme.h"
#include "ToastManager.h"
#include "imgui.h"

namespace Recovery {
bool enabled = true;
int recoveryCount = 0;
int lastRecoveryFrame = -1;
std::string lastRecoveryAction = "None";
static std::unordered_set<std::string> toastKeys;

static void Mark(const std::string& action, const char* toast = nullptr) {
    lastRecoveryAction = action;
    lastRecoveryFrame = ImGui::GetFrameCount();
    ++recoveryCount;
    if (toast && toastKeys.insert(action).second) Toast::Warning(toast);
}

void ValidateAndRepairWindowState() {
    ImVec2 size = WindowManager::SizeFor(WindowManager::currentScreen);
    if (size.x <= 0.0f || size.y <= 0.0f ||
        size.x > (float)WindowManager::screenW || size.y > (float)WindowManager::screenH) {
        Mark("Window size repaired", "Window position reset");
    }
}

void ValidateAndRepairThemeState() {
    int preset = (int)Theme::GetPreset();
    if (preset < 0 || preset > 4) {
        Theme::SetPreset(ThemePreset::DefaultPurple);
        Mark("Theme preset repaired", "Theme reset to default");
    }
}

void ValidateAndRepairScreenState() {
    if (WindowManager::screenW <= 0 || WindowManager::screenH <= 0) {
        WindowManager::SetScreenSize(std::max(1, g_DisplayW), std::max(1, g_DisplayH));
        Mark("Screen state repaired");
    }
}

void ValidateAndRepairModuleState() {
    const FeatureManifest* manifest = g_ModuleManager.GetActiveManifest();
    if (!manifest) return;
    int totalTabs = (int)manifest->tabs.size() + 1;
    if (g_ModuleState.activeTab < 0 || g_ModuleState.activeTab >= totalTabs) {
        g_ModuleState.activeTab = 0;
        Mark("Active tab repaired", "Module state repaired");
    }

    bool repaired = false;
    for (const auto& tab : manifest->tabs) {
        for (const auto& section : tab.sections) {
            std::string key = tab.id + "." + section.id;
            if (g_ModuleState.sectionOpenValues.find(key) == g_ModuleState.sectionOpenValues.end()) {
                g_ModuleState.sectionOpenValues[key] = section.defaultOpen;
                repaired = true;
            }
        }
        for (const auto& component : tab.components) {
            if (component.type == ComponentType::Dropdown) {
                int& value = g_ModuleState.intValues[component.id];
                int clamped = component.options.empty() ? 0 : std::max(0, std::min(value, (int)component.options.size() - 1));
                if (value != clamped) {
                    value = clamped;
                    repaired = true;
                }
            }
        }
    }
    if (repaired) Mark("Module state repaired", "Module state repaired");
}

void ValidateAndRepairConfigState() {
    if (g_ActiveProfileId.empty()) {
        g_ActiveProfileId = "default";
        Mark("Profile state repaired", "Config repaired");
    }
}

void RunStartupRecovery() {
    ValidateAndRepairScreenState();
    ValidateAndRepairThemeState();
    ValidateAndRepairConfigState();
}

void RunFrameRecoveryIfNeeded() {
    if (!enabled) return;
    ValidateAndRepairWindowState();
    ValidateAndRepairConfigState();
    ValidateAndRepairModuleState();
}
}
