#pragma once

#include <functional>
#include <string>

#include "Modules.h"
#include "TemplateModels.h"
#include "imgui.h"

class DynamicMenuRenderer {
public:
    void Render(const FeatureManifest& manifest, ModuleState& state, IModule* module);

private:
    void RenderTemplateSettings(const FeatureManifest& manifest, ModuleState& state);
    void DrawConfigAction(const char* label, const char* desc, IconId icon,
                          const std::function<void()>& action);
    void DrawEmptyState();
    bool DependencyMatches(const ComponentDependency& dependency, ModuleState& state);
    bool ShouldRender(const ComponentDependency& dependency, ModuleState& state);
    bool IsEnabled(const ComponentDependency& dependency, ModuleState& state);
    void RenderTab(const FeatureTab& tab, ModuleState& state, IModule* module);
    void RenderSection(const FeatureTab& tab, const FeatureSection& section, ModuleState& state, IModule* module);
    void NotifyChanged(IModule* module, const std::string& componentId);
    void RenderComponentControl(const FeatureComponent& component, ModuleState& state, IModule* module,
                                ImVec2 row, float rowH, float controlX, float controlW);
};
