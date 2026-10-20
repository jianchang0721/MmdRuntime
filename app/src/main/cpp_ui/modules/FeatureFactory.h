#pragma once

#include <string>
#include <vector>

#include "TemplateModels.h"

FeatureComponent MakeSwitch(const char* id, const char* label, bool value, const char* desc);
FeatureComponent MakeSlider(const char* id, const char* label, float value, float minValue, float maxValue, const char* desc);
FeatureComponent MakeDropdown(const char* id, const char* label, const std::vector<std::string>& options, int defaultIndex, const char* desc);
FeatureComponent MakeButton(const char* id, const char* label, const char* desc);
FeatureComponent MakeInput(const char* id, const char* label, const char* value, const char* desc);
FeatureComponent MakeKeybind(const char* id, const char* label, const char* value, const char* desc);
FeatureComponent MakeBadge(const char* id, const char* label, const char* value, const char* desc);
FeatureComponent WithBoolDependency(FeatureComponent component, DependencyMode mode, const char* targetId, bool expected);
FeatureComponent WithLayout(FeatureComponent component, ComponentLayout layout);
FeatureSection MakeSection(const char* id, const char* title, const char* description,
                           bool defaultOpen, const std::vector<FeatureComponent>& components);
FeatureSection WithLayout(FeatureSection section, SectionLayout layout);
void InitializeModuleState(const FeatureManifest& manifest, ModuleState& state);
