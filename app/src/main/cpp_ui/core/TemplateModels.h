#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct TouchInputEvent { int action; float x; float y; };
struct KeyInputEvent { int unicodeChar; int keyCode; bool isDown; };

enum class Screen {
    Login,
    Games,
    Menu
};

enum class IconId {
    None,
    Logo,
    Game,
    Search,
    Refresh,
    Close,
    Main,
    Visuals,
    Players,
    World,
    Misc,
    Configs,
    Settings,
    Key,
    Shield,
    Play,
    Status,
    Info,
    Warning,
    Error,
    Success,
    Save,
    Reset,
    Back,
    Module
};

enum class ToastType {
    Info,
    Success,
    Warning,
    Error
};

enum class ThemePreset {
    DefaultPurple,
    Blue,
    Red,
    Green,
    Monochrome
};

enum class BuildMode {
    Debug,
    Development,
    Demo,
    Release
};

enum class ComponentType {
    Switch,
    Slider,
    Dropdown,
    Button,
    Input,
    Keybind,
    Badge
};

enum class DependencyMode {
    None,
    ShowWhen,
    HideWhen,
    EnableWhen,
    DisableWhen
};

enum class ComponentLayout {
    NormalRow,
    CompactRow,
    Card,
    Inline,
    FullWidth,
    TwoColumn
};

enum class SectionLayout {
    Vertical,
    Compact,
    Card,
    TwoColumn
};

struct RuntimeBuildConfig {
    BuildMode mode = BuildMode::Development;
    bool allowDebugPanel = true;
    bool allowConfigReset = true;
    bool allowModuleReload = true;
    bool allowVerboseLogs = true;
    bool allowDemoModules = true;
    bool strictManifestValidation = true;
    bool showTemplateWatermark = false;
};

struct OverlayRuntimeState {
    bool overlayPermissionGranted = false;
    bool drawOverAppsEnabled = false;
    bool touchPassThroughEnabled = false;
    bool rendererReady = false;
    bool surfaceActive = false;
    bool imguiReady = false;
    int screenWidth = 0;
    int screenHeight = 0;
    bool landscape = false;
    bool appInForeground = true;
    std::string lastOverlayError;
    std::string lastRendererError;
};

struct UIState {
    char licenseKey[96] = "";
    char searchText[96] = "";
    bool attached = true;
};

struct GameProfile {
    std::string gameId;
    std::string displayName;
    bool online;
    std::string moduleId;
};

struct FeatureComponent {
    std::string id;
    std::string label;
    ComponentType type;
    bool defaultBool = false;
    float defaultFloat = 0.0f;
    int defaultInt = 0;
    std::string defaultString;
    float minValue = 0.0f;
    float maxValue = 100.0f;
    std::vector<std::string> options;
    std::string description;
    ComponentLayout layout = ComponentLayout::NormalRow;
    struct ComponentDependency {
        DependencyMode mode = DependencyMode::None;
        std::string targetComponentId;
        bool expectedBool = false;
        int expectedInt = 0;
        std::string expectedString;
    } dependency;
};

using ComponentDependency = FeatureComponent::ComponentDependency;

struct FeatureSection {
    std::string id;
    std::string title;
    std::string description;
    bool defaultOpen = true;
    std::vector<FeatureComponent> components;
    ComponentDependency dependency;
    SectionLayout layout = SectionLayout::Vertical;
};

struct FeatureTab {
    std::string id;
    std::string title;
    IconId icon = IconId::None;
    std::vector<FeatureSection> sections;
    std::vector<FeatureComponent> components;
};

struct FeatureManifest {
    int manifestVersion = 1;
    int minTemplateVersion = 1;
    std::string moduleId;
    std::string gameId;
    std::string moduleName;
    std::string version;
    std::vector<FeatureTab> tabs;
};

struct ModuleState {
    int activeTab = 0;
    std::unordered_map<std::string, bool> boolValues;
    std::unordered_map<std::string, float> floatValues;
    std::unordered_map<std::string, int> intValues;
    std::unordered_map<std::string, std::string> stringValues;
    std::unordered_map<std::string, bool> sectionOpenValues;
};

struct ManifestValidationResult {
    bool valid = false;
    std::string error;
};

static constexpr int TEMPLATE_MANIFEST_VERSION = 1;

inline float ClampFloat(float value, float minValue, float maxValue) {
    return value < minValue ? minValue : (value > maxValue ? maxValue : value);
}
