#include "Modules.h"

#include <algorithm>
#include <utility>

#include "AppState.h"
#include "ConfigStorage.h"
#include "FeatureFactory.h"

namespace TemplateData {
const std::vector<GameProfile>& Games() {
    static const std::vector<GameProfile> games = {
        {"test_game_01", "Test Game 01", true, "test_module"},
        {"test_game_02", "Test Game 02", true, "demo_module"},
        {"test_game_03", "Test Game 03", true, "basic_module"},
        {"test_game_04", "Test Game 04", false, "offline_module"},
        {"test_game_05", "Test Game 05", false, "offline_module"},
    };
    return games;
}
}

class TestModule final : public IModule {
public:
    const char* GetModuleId() const override { return "test_module"; }
    const char* GetGameId() const override { return "test_game_01"; }
    const char* GetDisplayName() const override { return "Test Module"; }
    const char* GetVersion() const override { return "1.0.0"; }
    FeatureManifest GetManifest() const override {
        FeatureManifest m;
        m.manifestVersion = TEMPLATE_MANIFEST_VERSION;
        m.minTemplateVersion = 1;
        m.moduleId = GetModuleId();
        m.gameId = GetGameId();
        m.moduleName = GetDisplayName();
        m.version = GetVersion();
        FeatureTab mainTab{"main", "Main", IconId::Main};
        mainTab.sections.push_back(MakeSection("general", "General", "Primary placeholder controls", true, {
            MakeSwitch("test_switch", "Test Switch", true, "This is a test switch"),
            WithBoolDependency(MakeSlider("test_slider", "Test Slider", 50.0f, 0.0f, 100.0f, "Visible when Test Switch is enabled"),
                               DependencyMode::ShowWhen, "test_switch", true),
        }));
        mainTab.sections.push_back(WithLayout(MakeSection("controls", "Controls", "Simple interactive examples", true, {
            MakeDropdown("test_dropdown", "Test Dropdown", {"Option 1", "Option 2", "Option 3"}, 0, "This is a test dropdown"),
            WithBoolDependency(MakeButton("test_button", "Test Button", "Disabled when Test Switch is off"),
                               DependencyMode::EnableWhen, "test_switch", true),
            MakeKeybind("test_keybind", "Test Keybind", "F1", "This is a test keybind"),
        }), SectionLayout::Compact));
        mainTab.sections.push_back(WithLayout(MakeSection("info", "Info", "Text and badge examples", true, {
            MakeInput("test_input", "Test Input", "", "This is a test input"),
            MakeBadge("test_badge", "Test Badge", "TEST", "This is a test status badge"),
            WithLayout(MakeButton("test_full_width", "Full Width Button", "Full-width placeholder action"), ComponentLayout::FullWidth),
        }), SectionLayout::Card));
        m.tabs.push_back(mainTab);
        return m;
    }
};

class DemoModule final : public IModule {
public:
    const char* GetModuleId() const override { return "demo_module"; }
    const char* GetGameId() const override { return "test_game_02"; }
    const char* GetDisplayName() const override { return "Demo Module"; }
    const char* GetVersion() const override { return "1.0.0"; }
    FeatureManifest GetManifest() const override {
        FeatureManifest m;
        m.manifestVersion = TEMPLATE_MANIFEST_VERSION;
        m.minTemplateVersion = 1;
        m.moduleId = GetModuleId();
        m.gameId = GetGameId();
        m.moduleName = GetDisplayName();
        m.version = GetVersion();
        FeatureTab mainTab{"main", "Main", IconId::Main};
        mainTab.sections.push_back(WithLayout(MakeSection("general", "General", "Demo module controls", true, {
            MakeSwitch("demo_switch", "Demo Switch", true, "This is a demo switch"),
            WithBoolDependency(MakeSlider("demo_slider", "Demo Slider", 35.0f, 0.0f, 100.0f, "Visible when Demo Switch is enabled"),
                               DependencyMode::ShowWhen, "demo_switch", true),
            WithLayout(MakeButton("demo_button", "Demo Button", "This is a demo button"), ComponentLayout::Inline),
        }), SectionLayout::TwoColumn));
        m.tabs.push_back(mainTab);
        FeatureTab settingsTab{"settings", "Settings", IconId::Settings};
        settingsTab.sections.push_back(WithLayout(MakeSection("module_settings", "Module Settings", "Placeholder module values", true, {
            MakeDropdown("demo_dropdown", "Demo Dropdown", {"Option 1", "Option 2", "Option 3"}, 1, "This is a demo dropdown"),
            MakeInput("demo_input", "Demo Input", "", "This is a demo input"),
            MakeBadge("demo_badge", "Demo Badge", "DEMO", "This is a demo badge"),
        }), SectionLayout::Compact));
        m.tabs.push_back(settingsTab);
        return m;
    }
};

class BasicModule final : public IModule {
public:
    const char* GetModuleId() const override { return "basic_module"; }
    const char* GetGameId() const override { return "test_game_03"; }
    const char* GetDisplayName() const override { return "Basic Module"; }
    const char* GetVersion() const override { return "1.0.0"; }
    FeatureManifest GetManifest() const override {
        FeatureManifest m;
        m.manifestVersion = TEMPLATE_MANIFEST_VERSION;
        m.minTemplateVersion = 1;
        m.moduleId = GetModuleId();
        m.gameId = GetGameId();
        m.moduleName = GetDisplayName();
        m.version = GetVersion();
        FeatureTab mainTab{"main", "Main", IconId::Main};
        mainTab.sections.push_back(MakeSection("general", "General", "Basic module examples", true, {
            MakeSwitch("basic_switch", "Basic Switch", false, "This is a basic switch"),
            WithBoolDependency(MakeButton("basic_button", "Basic Button", "Enabled when Basic Switch is on"),
                               DependencyMode::EnableWhen, "basic_switch", true),
            MakeBadge("basic_badge", "Basic Badge", "BASIC", "This is a basic badge"),
        }));
        m.tabs.push_back(mainTab);
        return m;
    }
};

std::unique_ptr<IModule> CreateTestModule() { return std::make_unique<TestModule>(); }
std::unique_ptr<IModule> CreateDemoModule() { return std::make_unique<DemoModule>(); }
std::unique_ptr<IModule> CreateBasicModule() { return std::make_unique<BasicModule>(); }

void ModuleManager::RegisterModule(std::unique_ptr<IModule> module) {
    if (!module) return;
    modules.push_back(std::move(module));
}

bool ModuleManager::LoadModuleForGame(const std::string& gameId) {
    for (const auto& profile : TemplateData::Games()) {
        if (profile.gameId == gameId) {
            lastSelectedGameId = profile.gameId;
            lastSelectedGameName = profile.displayName;
            if (!profile.online) {
                lastError = "Game offline";
                return false;
            }
            return LoadModuleById(profile.moduleId);
        }
    }
    lastError = "Game profile not found";
    return false;
}

bool ModuleManager::LoadModuleById(const std::string& moduleId) {
    for (auto& module : modules) {
        if (std::string(module->GetModuleId()) != moduleId) continue;
        FeatureManifest manifest = module->GetManifest();
        ManifestValidationResult validation = ValidateManifest(manifest);
        if (!validation.valid) {
            lastError = validation.error;
            lastValidationError = validation.error;
            UnloadCurrentModule();
            return false;
        }
        UnloadCurrentModule();
        activeModule = module.get();
        activeManifest = manifest;
        activeModuleId = activeManifest.moduleId;
        InitializeModuleState(activeManifest, g_ModuleState);
        Config::LoadModuleConfig(activeManifest.moduleId, g_ModuleState, &activeManifest);
        activeModule->OnLoad();
        lastError.clear();
        lastValidationError.clear();
        return true;
    }
    lastError = "Module not found";
    return false;
}

void ModuleManager::UnloadCurrentModule() {
    if (activeModule) {
        Config::SaveModuleConfig(activeManifest.moduleId, g_ModuleState, &activeManifest);
        activeModule->OnUnload();
    }
    activeModule = nullptr;
    activeManifest = FeatureManifest();
    activeModuleId.clear();
    g_ModuleState = ModuleState();
}

bool ModuleManager::ReloadActiveModule() {
    std::string moduleId = activeModule ? activeManifest.moduleId : activeModuleId;
    if (moduleId.empty() && !lastSelectedGameId.empty()) return LoadModuleForGame(lastSelectedGameId);
    if (moduleId.empty()) {
        lastError = "No active module to reload";
        return false;
    }
    return LoadModuleById(moduleId);
}

void ModuleManager::ResetActiveModuleState() {
    if (!IsModuleLoaded()) return;
    InitializeModuleState(activeManifest, g_ModuleState);
}

bool ModuleManager::IsModuleLoaded() const { return activeModule != nullptr && !activeManifest.tabs.empty(); }
bool ModuleManager::HasActiveModule() const { return IsModuleLoaded(); }
IModule* ModuleManager::GetActiveModule() { return activeModule; }
const FeatureManifest* ModuleManager::GetActiveManifest() const { return IsModuleLoaded() ? &activeManifest : nullptr; }
std::string ModuleManager::GetLastError() const { return lastError; }
std::string ModuleManager::GetLastValidationError() const { return lastValidationError; }
std::string ModuleManager::GetLastSelectedGameId() const { return lastSelectedGameId; }
std::string ModuleManager::GetLastSelectedGameName() const { return lastSelectedGameName; }

bool ModuleManager::ValidateComponent(const FeatureComponent& component, std::unordered_set<std::string>& componentIds) const {
    if (component.id.empty()) { lastMutableValidationError = "Manifest has component without id"; return false; }
    if (component.label.empty()) { lastMutableValidationError = "Manifest has component without label"; return false; }
    if (!componentIds.insert(component.id).second) { lastMutableValidationError = "Manifest has duplicate component id"; return false; }
    if (component.type == ComponentType::Slider && component.minValue >= component.maxValue) {
        lastMutableValidationError = "Slider min must be less than max";
        return false;
    }
    if (component.type == ComponentType::Dropdown && component.options.empty()) {
        lastMutableValidationError = "Dropdown needs options";
        return false;
    }
    return true;
}

ManifestValidationResult ModuleManager::ValidateManifest(const FeatureManifest& manifest) const {
    if (manifest.manifestVersion <= 0 || manifest.manifestVersion > TEMPLATE_MANIFEST_VERSION) return {false, "Unsupported manifest version"};
    if (manifest.minTemplateVersion > TEMPLATE_MANIFEST_VERSION) return {false, "Module requires a newer template"};
    if (manifest.moduleId.empty()) return {false, "Manifest missing module id"};
    if (manifest.gameId.empty()) return {false, "Manifest missing game id"};
    if (manifest.moduleName.empty()) return {false, "Manifest missing module name"};
    if (manifest.version.empty()) return {false, "Manifest missing version"};
    if (manifest.tabs.empty()) return {false, "Manifest has no tabs"};

    std::unordered_set<std::string> componentIds;
    std::unordered_set<std::string> tabIds;
    for (const auto& tab : manifest.tabs) {
        if (tab.id.empty()) return {false, "Manifest has tab without id"};
        if (tab.title.empty()) return {false, "Manifest has tab without title"};
        if (!tabIds.insert(tab.id).second) return {false, "Manifest has duplicate tab id"};
        std::unordered_set<std::string> sectionIds;
        for (const auto& section : tab.sections) {
            if (section.id.empty()) return {false, "Manifest has section without id"};
            if (section.title.empty()) return {false, "Manifest has section without title"};
            if (!sectionIds.insert(section.id).second) return {false, "Manifest has duplicate section id"};
            for (const auto& component : section.components) {
                if (!ValidateComponent(component, componentIds)) return {false, lastMutableValidationError};
            }
        }
        for (const auto& component : tab.components) {
            if (!ValidateComponent(component, componentIds)) return {false, lastMutableValidationError};
        }
    }
    return {true, ""};
}
