#include "FeatureFactory.h"

FeatureComponent MakeSwitch(const char* id, const char* label, bool value, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Switch;
    c.defaultBool = value;
    c.description = desc;
    return c;
}

FeatureComponent MakeSlider(const char* id, const char* label, float value, float minValue, float maxValue, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Slider;
    c.defaultFloat = value;
    c.minValue = minValue;
    c.maxValue = maxValue;
    c.description = desc;
    return c;
}

FeatureComponent MakeDropdown(const char* id, const char* label, const std::vector<std::string>& options, int defaultIndex, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Dropdown;
    c.options = options;
    c.defaultInt = defaultIndex;
    c.description = desc;
    return c;
}

FeatureComponent MakeButton(const char* id, const char* label, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Button;
    c.description = desc;
    return c;
}

FeatureComponent MakeInput(const char* id, const char* label, const char* value, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Input;
    c.defaultString = value;
    c.description = desc;
    return c;
}

FeatureComponent MakeKeybind(const char* id, const char* label, const char* value, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Keybind;
    c.defaultString = value;
    c.description = desc;
    return c;
}

FeatureComponent MakeBadge(const char* id, const char* label, const char* value, const char* desc) {
    FeatureComponent c;
    c.id = id;
    c.label = label;
    c.type = ComponentType::Badge;
    c.defaultString = value;
    c.description = desc;
    return c;
}

FeatureComponent WithBoolDependency(FeatureComponent component, DependencyMode mode, const char* targetId, bool expected) {
    component.dependency.mode = mode;
    component.dependency.targetComponentId = targetId ? targetId : "";
    component.dependency.expectedBool = expected;
    return component;
}

FeatureComponent WithLayout(FeatureComponent component, ComponentLayout layout) {
    component.layout = layout;
    return component;
}

FeatureSection MakeSection(const char* id, const char* title, const char* description,
                           bool defaultOpen, const std::vector<FeatureComponent>& components) {
    FeatureSection section;
    section.id = id;
    section.title = title;
    section.description = description ? description : "";
    section.defaultOpen = defaultOpen;
    section.components = components;
    return section;
}

FeatureSection WithLayout(FeatureSection section, SectionLayout layout) {
    section.layout = layout;
    return section;
}

void InitializeModuleState(const FeatureManifest& manifest, ModuleState& state) {
    state = ModuleState();
    auto initComponent = [&](const FeatureComponent& component) {
        switch (component.type) {
            case ComponentType::Switch: state.boolValues[component.id] = component.defaultBool; break;
            case ComponentType::Slider: state.floatValues[component.id] = component.defaultFloat; break;
            case ComponentType::Dropdown: state.intValues[component.id] = component.defaultInt; break;
            case ComponentType::Input:
            case ComponentType::Keybind:
            case ComponentType::Badge: state.stringValues[component.id] = component.defaultString; break;
            case ComponentType::Button: break;
        }
    };
    for (const auto& tab : manifest.tabs) {
        for (const auto& section : tab.sections) {
            state.sectionOpenValues[tab.id + "." + section.id] = section.defaultOpen;
            for (const auto& component : section.components) initComponent(component);
        }
        for (const auto& component : tab.components) initComponent(component);
    }
}
