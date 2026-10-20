#include "DynamicMenuRenderer.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "AppState.h"
#include "ConfigStorage.h"
#include "FeatureFactory.h"
#include "Theme.h"
#include "ToastManager.h"
#include "UIComponents.h"
void DynamicMenuRenderer::Render(const FeatureManifest& manifest, ModuleState& state, IModule* module) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        float w = ImGui::GetWindowWidth();
        float h = ImGui::GetWindowHeight();
        float outerPad = ClampFloat(w * 0.035f, 24.0f, 36.0f);
        float topY = Theme::Sizes::TitleBarHeight;
        float sidebarW = ClampFloat(w * 0.22f, Theme::Sizes::SidebarMinWidth, Theme::Sizes::SidebarMaxWidth);
        float sidebarGap = ClampFloat(w * 0.035f, 24.0f, 38.0f);
        float contentX = outerPad + sidebarW + sidebarGap;
        float contentW = w - contentX - outerPad;
        float contentTop = topY + 6.0f;
        float titleH = 58.0f;
        float panelTop = contentTop + titleH;
        float panelH = h - panelTop - outerPad;

        ImVec2 sidebarMin(wp.x + outerPad, wp.y + topY);
        ImVec2 sidebarMax(sidebarMin.x + sidebarW, wp.y + h - outerPad);
        dl->AddRectFilled(sidebarMin, sidebarMax, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Background, 0.28f)), Theme::Sizes::ChildRounding);
        dl->AddLine(ImVec2(sidebarMax.x + sidebarGap * 0.5f, sidebarMin.y), ImVec2(sidebarMax.x + sidebarGap * 0.5f, sidebarMax.y),
                    ImGui::GetColorU32(Theme::Colors::Border), 1.0f);

        const int moduleTabCount = (int)manifest.tabs.size();
        const int totalTabCount = moduleTabCount + 1;
        FeatureTab settingsTab{"template_settings", "Settings", IconId::Settings, {}};

        if (manifest.tabs.empty()) {
            state.activeTab = 0;
        } else {
            state.activeTab = std::max(0, std::min(state.activeTab, totalTabCount - 1));
        }

        float tabY = sidebarMin.y + 18.0f;
        float tabH = 42.0f;
        float tabGap = 12.0f;
        for (int i = 0; i < totalTabCount; ++i) {
            const FeatureTab& tab = i < moduleTabCount ? manifest.tabs[i] : settingsTab;
            ImVec2 min(sidebarMin.x + Theme::Spacing::SidebarPadding, tabY);
            ImVec2 max(sidebarMax.x - Theme::Spacing::SidebarPadding, tabY + tabH);
            UI::SidebarTab(tab, i, &state.activeTab, min, max);
            tabY += tabH + tabGap;
        }

        ImVec2 attachedMin(sidebarMin.x + 12.0f, sidebarMax.y - 54.0f);
        dl->AddRect(attachedMin, ImVec2(sidebarMax.x - 12.0f, sidebarMax.y - 10.0f), ImGui::GetColorU32(Theme::Colors::Border), 8.0f, 0, 1.0f);
        UI::StatusDot(dl, ImVec2(attachedMin.x + 22.0f, attachedMin.y + 22.0f), g_State.attached);
        dl->AddText(ImVec2(attachedMin.x + 42.0f, attachedMin.y + 13.0f), ImGui::GetColorU32(Theme::Colors::TextSecondary), "Attached");

        bool showingSettings = state.activeTab == moduleTabCount;
        const FeatureTab* activeTab = (!showingSettings && !manifest.tabs.empty()) ? &manifest.tabs[state.activeTab] : nullptr;
        ImGui::SetCursorPos(ImVec2(contentX, contentTop));
        std::string subtitle = manifest.moduleName.empty()
            ? "Return to Games and select a game."
            : manifest.moduleName + " v" + manifest.version;
        UI::Section(showingSettings ? "Settings" : (activeTab ? activeTab->title.c_str() : "No module loaded"), subtitle.c_str());

        ImGui::SetCursorPos(ImVec2(contentX, panelTop));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Theme::Spacing::ComponentPadding, Theme::Spacing::ComponentPadding));
        ImGui::BeginChild("##component_panel", ImVec2(contentW, panelH), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (showingSettings) {
            RenderTemplateSettings(manifest, state);
        } else if (!activeTab) {
            DrawEmptyState();
        } else if (activeTab->sections.empty() && activeTab->components.empty()) {
            ImGui::SetCursorPos(ImVec2(20.0f, 24.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
            ImGui::TextUnformatted("This tab is empty.");
            ImGui::PopStyleColor();
        } else {
            RenderTab(*activeTab, state, module);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
void DynamicMenuRenderer::RenderTemplateSettings(const FeatureManifest& manifest, ModuleState& state) {
        float rowGap = Theme::Spacing::RowGap;
        float rowH = 74.0f;
        float rowInnerW = ImGui::GetContentRegionAvail().x;
        float controlW = ClampFloat(rowInnerW * 0.38f, 180.0f, 260.0f);
        float controlX = ImGui::GetCursorScreenPos().x + rowInnerW - controlW - 18.0f;

        FeatureComponent themeComponent = MakeDropdown(
            "template_theme", "Theme Preset",
            {"Default Purple", "Blue", "Red", "Green", "Monochrome"},
            (int)Theme::GetPreset(), "Switches the local template accent colors");
        ImVec2 row = UI::Card(themeComponent.label.c_str(), themeComponent.description.c_str(), rowH);
        int preset = (int)Theme::GetPreset();
        ImGui::SetCursorScreenPos(ImVec2(controlX, row.y + (rowH - 38.0f) * 0.5f));
        if (UI::Dropdown("##theme_preset", &preset, themeComponent.options, controlW)) {
            Theme::SetPreset((ThemePreset)std::max(0, std::min(4, preset)));
            Toast::Info("Theme changed");
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowH + rowGap));

        std::vector<ProfileManager::ModuleProfile> profiles = ProfileManager::GetProfiles(manifest.moduleId);
        std::vector<std::string> profileNames;
        int activeProfileIndex = 0;
        for (int i = 0; i < (int)profiles.size(); ++i) {
            profileNames.push_back(profiles[i].displayName);
            if (profiles[i].profileId == g_ActiveProfileId) activeProfileIndex = i;
        }
        FeatureComponent profileComponent = MakeDropdown(
            "template_profile", "Profile",
            profileNames, activeProfileIndex, "Local preset profile for this module");
        row = UI::Card(profileComponent.label.c_str(), profileComponent.description.c_str(), rowH);
        ImGui::SetCursorScreenPos(ImVec2(controlX, row.y + (rowH - 38.0f) * 0.5f));
        if (UI::Dropdown("##module_profile", &activeProfileIndex, profileComponent.options, controlW)) {
            activeProfileIndex = std::max(0, std::min(activeProfileIndex, (int)profiles.size() - 1));
            g_ActiveProfileId = profiles[activeProfileIndex].profileId;
            InitializeModuleState(manifest, state);
            if (ProfileManager::LoadProfile(manifest.moduleId, g_ActiveProfileId, state, &manifest)) Toast::Success("Profile loaded");
            else Toast::Warning("Profile reset to defaults");
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowH + rowGap));

        DrawConfigAction("Save Config", "Persist this module's placeholder values", IconId::Save, [&]() {
            if (Config::SaveModuleConfig(manifest.moduleId, state, &manifest)) Toast::Success("Config saved");
            else Toast::Error("Config failed to save");
        });
        DrawConfigAction("Load Config", "Reload saved placeholder values", IconId::Configs, [&]() {
            if (Config::LoadModuleConfig(manifest.moduleId, state, &manifest)) Toast::Success("Config loaded");
            else Toast::Warning("Config failed to load");
        });
        if (g_BuildConfig.allowConfigReset) {
            DrawConfigAction("Reset Config", "Remove saved values for this module", IconId::Reset, [&]() {
                Config::ResetModuleConfig(manifest.moduleId);
                InitializeModuleState(manifest, state);
                Toast::Success("Config reset");
            });
        }
        DrawConfigAction("Save Profile", "Persist the active local profile", IconId::Save, [&]() {
            if (g_ActiveProfileId == "default") {
                if (Config::SaveModuleConfig(manifest.moduleId, state, &manifest)) Toast::Success("Profile saved");
                else Toast::Error("Profile failed to save");
            } else if (ProfileManager::SaveProfile(manifest.moduleId, g_ActiveProfileId, state, &manifest)) {
                Toast::Success("Profile saved");
            } else {
                Toast::Error("Profile failed to save");
            }
        });
        if (g_BuildConfig.allowConfigReset) {
            DrawConfigAction("Reset Profile", "Reset the active profile to manifest defaults", IconId::Reset, [&]() {
                ProfileManager::ResetProfile(manifest.moduleId, g_ActiveProfileId);
                InitializeModuleState(manifest, state);
                Toast::Success("Profile reset");
            });
        }
        DrawConfigAction("New Profile", "Create or overwrite Custom 1 from current values", IconId::Module, [&]() {
            g_ActiveProfileId = "custom_1";
            if (ProfileManager::SaveProfile(manifest.moduleId, g_ActiveProfileId, state, &manifest)) Toast::Success("Profile created");
            else Toast::Error("Profile failed to create");
        });
        if (g_BuildConfig.allowConfigReset) {
            DrawConfigAction("Delete Profile", "Delete the active non-default profile", IconId::Reset, [&]() {
                if (g_ActiveProfileId == "default") {
                    Toast::Warning("Default profile cannot be deleted");
                } else if (ProfileManager::DeleteProfile(manifest.moduleId, g_ActiveProfileId)) {
                    g_ActiveProfileId = "default";
                    InitializeModuleState(manifest, state);
                    ProfileManager::LoadProfile(manifest.moduleId, g_ActiveProfileId, state, &manifest);
                    Toast::Success("Profile deleted");
                } else {
                    Toast::Error("Profile failed to delete");
                }
            });
        }
    }

void DynamicMenuRenderer::DrawConfigAction(const char* label, const char* desc, IconId icon, const std::function<void()>& action) {
        float rowH = 70.0f;
        float rowInnerW = ImGui::GetContentRegionAvail().x;
        float controlW = ClampFloat(rowInnerW * 0.34f, 160.0f, 230.0f);
        float controlX = ImGui::GetCursorScreenPos().x + rowInnerW - controlW - 18.0f;
        ImVec2 row = UI::Card(label, desc, rowH);
        ImGui::SetCursorScreenPos(ImVec2(controlX, row.y + (rowH - Theme::Sizes::ButtonHeight) * 0.5f));
        if (UI::IconButton(icon, label)) action();
        ImGui::SameLine(0.0f, 8.0f);
        if (UI::Button(label, ImVec2(controlW - Theme::Sizes::ButtonHeight - 8.0f, Theme::Sizes::ButtonHeight))) action();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowH + Theme::Spacing::RowGap));
    }

void DynamicMenuRenderer::DrawEmptyState() {
        ImGui::SetCursorPos(ImVec2(20.0f, 24.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("No module loaded");
        ImGui::TextUnformatted("Return to Games and select a game.");
        ImGui::PopStyleColor();
    }

bool DynamicMenuRenderer::DependencyMatches(const ComponentDependency& dependency, ModuleState& state) {
        if (dependency.mode == DependencyMode::None || dependency.targetComponentId.empty()) return true;
        auto boolIt = state.boolValues.find(dependency.targetComponentId);
        if (boolIt != state.boolValues.end()) return boolIt->second == dependency.expectedBool;
        auto intIt = state.intValues.find(dependency.targetComponentId);
        if (intIt != state.intValues.end()) return intIt->second == dependency.expectedInt;
        auto stringIt = state.stringValues.find(dependency.targetComponentId);
        if (stringIt != state.stringValues.end()) return stringIt->second == dependency.expectedString;
        return true;
    }

bool DynamicMenuRenderer::ShouldRender(const ComponentDependency& dependency, ModuleState& state) {
        bool matches = DependencyMatches(dependency, state);
        if (dependency.mode == DependencyMode::ShowWhen) return matches;
        if (dependency.mode == DependencyMode::HideWhen) return !matches;
        return true;
    }

bool DynamicMenuRenderer::IsEnabled(const ComponentDependency& dependency, ModuleState& state) {
        bool matches = DependencyMatches(dependency, state);
        if (dependency.mode == DependencyMode::EnableWhen) return matches;
        if (dependency.mode == DependencyMode::DisableWhen) return !matches;
        return true;
    }

void DynamicMenuRenderer::RenderTab(const FeatureTab& tab, ModuleState& state, IModule* module) {
        if (!tab.sections.empty()) {
            for (const auto& section : tab.sections) RenderSection(tab, section, state, module);
        }
        if (!tab.components.empty()) {
            FeatureSection legacy = MakeSection("legacy", "General", "", true, tab.components);
            RenderSection(tab, legacy, state, module);
        }
    }

void DynamicMenuRenderer::RenderSection(const FeatureTab& tab, const FeatureSection& section, ModuleState& state, IModule* module) {
        if (!ShouldRender(section.dependency, state)) return;
        std::string sectionKey = tab.id + "." + section.id;
        if (state.sectionOpenValues.find(sectionKey) == state.sectionOpenValues.end()) {
            state.sectionOpenValues[sectionKey] = section.defaultOpen;
        }

        float headerH = 52.0f;
        ImVec2 header = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(header, ImVec2(header.x + width, header.y + headerH),
                          ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Background, 0.28f)), 10.0f);
        dl->AddRect(header, ImVec2(header.x + width, header.y + headerH),
                    ImGui::GetColorU32(Theme::Colors::Border), 10.0f, 0, 1.0f);
        dl->AddText(ImVec2(header.x + 18.0f, header.y + 10.0f), ImGui::GetColorU32(Theme::Colors::TextPrimary), section.title.c_str());
        if (!section.description.empty()) {
            dl->AddText(ImVec2(header.x + 18.0f, header.y + 29.0f),
                        ImGui::GetColorU32(Theme::Colors::TextSecondary), section.description.c_str());
        }
        const char* arrow = state.sectionOpenValues[sectionKey] ? "-" : "+";
        ImVec2 arrowSize = ImGui::CalcTextSize(arrow);
        dl->AddText(ImVec2(header.x + width - arrowSize.x - 20.0f, header.y + 17.0f),
                    ImGui::GetColorU32(Theme::Colors::Accent), arrow);
        ImGui::InvisibleButton(("##section_" + sectionKey).c_str(), ImVec2(width, headerH));
        if (ImGui::IsItemClicked()) state.sectionOpenValues[sectionKey] = !state.sectionOpenValues[sectionKey];
        ImGui::SetCursorScreenPos(ImVec2(header.x, header.y + headerH + Theme::Spacing::RowGap));
        if (!state.sectionOpenValues[sectionKey]) {
            ImGui::Dummy(ImVec2(width, 2.0f));
            return;
        }

        float rowGap = section.layout == SectionLayout::Compact ? 8.0f : Theme::Spacing::RowGap;
        float baseRowH = section.layout == SectionLayout::Compact ? Theme::Sizes::RowHeightMin : Theme::Sizes::RowHeightMax;
        if (section.layout == SectionLayout::Card) baseRowH += 8.0f;
        float rowInnerW = ImGui::GetContentRegionAvail().x;
        bool twoColumn = section.layout == SectionLayout::TwoColumn && rowInnerW > 560.0f;
        float columnGap = Theme::Spacing::RowGap;
        float columnW = twoColumn ? (rowInnerW - columnGap) * 0.5f : rowInnerW;
        ImVec2 sectionStart = ImGui::GetCursorScreenPos();
        int visibleIndex = 0;

        for (const auto& component : section.components) {
            if (!ShouldRender(component.dependency, state)) continue;
            bool enabled = IsEnabled(component.dependency, state);
            float rowH = baseRowH;
            if (component.layout == ComponentLayout::CompactRow) rowH = Theme::Sizes::RowHeightMin;
            if (component.layout == ComponentLayout::Card) rowH = Theme::Sizes::RowHeightMax + 8.0f;
            if (component.layout == ComponentLayout::Inline) rowH = Theme::Sizes::RowHeightMin;
            if (component.layout == ComponentLayout::FullWidth) rowH = Theme::Sizes::RowHeightMax;

            float cardW = columnW;
            if (twoColumn) {
                int col = visibleIndex % 2;
                int rowIndex = visibleIndex / 2;
                ImGui::SetCursorScreenPos(ImVec2(sectionStart.x + (float)col * (columnW + columnGap),
                                                 sectionStart.y + (float)rowIndex * (rowH + rowGap)));
            }
            ImVec2 row = twoColumn
                ? UI::CardSized(component.label.c_str(), component.description.c_str(), cardW, rowH)
                : UI::Card(component.label.c_str(), component.description.c_str(), rowH);
            float controlW = component.layout == ComponentLayout::FullWidth
                ? cardW - 36.0f
                : ClampFloat(cardW * 0.36f, 150.0f, std::min(260.0f, cardW - 44.0f));
            float controlX = component.layout == ComponentLayout::FullWidth
                ? row.x + 18.0f
                : row.x + cardW - controlW - 18.0f;
            if (!enabled) ImGui::BeginDisabled();
            RenderComponentControl(component, state, module, row, rowH, controlX, controlW);
            if (!enabled) ImGui::EndDisabled();
            if (!twoColumn) ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowH + rowGap));
            ++visibleIndex;
        }
        if (twoColumn && visibleIndex > 0) {
            int rows = (visibleIndex + 1) / 2;
            ImGui::SetCursorScreenPos(ImVec2(sectionStart.x, sectionStart.y + (float)rows * (baseRowH + rowGap)));
        }
        ImGui::Dummy(ImVec2(width, 4.0f));
    }

void DynamicMenuRenderer::NotifyChanged(IModule* module, const std::string& componentId) {
        if (module) module->OnComponentChanged(componentId);
    }

void DynamicMenuRenderer::RenderComponentControl(const FeatureComponent& component, ModuleState& state, IModule* module,
                                ImVec2 row, float rowH, float controlX, float controlW) {
        switch (component.type) {
            case ComponentType::Switch: {
                bool& value = state.boolValues[component.id];
                float y = row.y + (rowH - Theme::Sizes::SwitchSize.y) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX + controlW - Theme::Sizes::SwitchSize.x, y));
                if (UI::Toggle(("##" + component.id).c_str(), &value)) NotifyChanged(module, component.id);
                break;
            }
            case ComponentType::Slider: {
                float& value = state.floatValues[component.id];
                float y = row.y + (rowH - 34.0f) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX, y));
                ImGui::SetNextItemWidth(controlW);
                if (ImGui::SliderFloat(("##" + component.id).c_str(), &value, component.minValue, component.maxValue, "%.0f%%")) {
                    NotifyChanged(module, component.id);
                }
                break;
            }
            case ComponentType::Dropdown: {
                int& value = state.intValues[component.id];
                float y = row.y + (rowH - 38.0f) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX, y));
                if (UI::Dropdown(("##" + component.id).c_str(), &value, component.options, controlW)) NotifyChanged(module, component.id);
                break;
            }
            case ComponentType::Button: {
                float y = row.y + (rowH - Theme::Sizes::ButtonHeight) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX, y));
                if (UI::Button(component.label.c_str(), ImVec2(controlW, Theme::Sizes::ButtonHeight))) NotifyChanged(module, component.id);
                break;
            }
            case ComponentType::Input: {
                std::string& value = state.stringValues[component.id];
                char buffer[128];
                snprintf(buffer, sizeof(buffer), "%s", value.c_str());
                float y = row.y + (rowH - Theme::Sizes::InputHeight) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX, y));
                if (UI::InputText(("##" + component.id).c_str(), buffer, sizeof(buffer), "Type something...", ImVec2(controlW, Theme::Sizes::InputHeight))) {
                    value = buffer;
                    NotifyChanged(module, component.id);
                }
                break;
            }
            case ComponentType::Keybind: {
                std::string& value = state.stringValues[component.id];
                if (value.empty()) value = component.defaultString.empty() ? "F1" : component.defaultString;
                float y = row.y + (rowH - 38.0f) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX + controlW - 124.0f, y));
                if (UI::Keybind(value.c_str(), ImVec2(124.0f, 38.0f))) NotifyChanged(module, component.id);
                break;
            }
            case ComponentType::Badge: {
                std::string& value = state.stringValues[component.id];
                if (value.empty()) value = component.defaultString.empty() ? "TEST" : component.defaultString;
                float y = row.y + (rowH - 32.0f) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX + controlW - 84.0f, y));
                UI::Badge(value.c_str());
                break;
            }
            default: {
                float y = row.y + (rowH - Theme::Sizes::ButtonHeight) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(controlX, y));
                ImGui::BeginDisabled();
                UI::Button("Unsupported", ImVec2(controlW, Theme::Sizes::ButtonHeight));
                ImGui::EndDisabled();
                break;
            }
        }
    }
