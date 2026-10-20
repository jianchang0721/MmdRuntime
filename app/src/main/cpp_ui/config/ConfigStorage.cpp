#include "ConfigStorage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

#include "AppState.h"
#include "Theme.h"

namespace Config {
std::string PathForModule(const std::string& moduleId) {
    std::string base = g_ConfigDirectory.empty() ? "configs" : g_ConfigDirectory;
    return base + "/" + moduleId + ".cfg";
}

void EnsureDirectory() {
    std::string base = g_ConfigDirectory.empty() ? "configs" : g_ConfigDirectory;
    mkdir(base.c_str(), 0700);
}

std::string Escape(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\t') out += "\\t";
        else out += ch;
    }
    return out;
}

std::string Unescape(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char next = value[++i];
            if (next == 'n') out += '\n';
            else if (next == 't') out += '\t';
            else out += next;
        } else {
            out += value[i];
        }
    }
    return out;
}

namespace StateSerializer {
static std::unordered_map<std::string, const FeatureComponent*> CollectComponents(const FeatureManifest* manifest) {
    std::unordered_map<std::string, const FeatureComponent*> components;
    if (!manifest) return components;
    for (const auto& tab : manifest->tabs) {
        for (const auto& section : tab.sections) {
            for (const auto& component : section.components) components[component.id] = &component;
        }
        for (const auto& component : tab.components) components[component.id] = &component;
    }
    return components;
}

void Save(std::ostream& file, const std::string& moduleId, const ModuleState& state,
          const FeatureManifest* manifest, const std::string& profileId) {
    if (!profileId.empty()) file << "profileId\t" << Escape(profileId) << "\n";
    file << "moduleId\t" << Escape(moduleId) << "\n";
    file << "moduleVersion\t" << Escape(manifest ? manifest->version : "") << "\n";
    file << "themePreset\t" << (int)Theme::GetPreset() << "\n";
    file << "activeTab\t" << state.activeTab << "\n";
    for (const auto& it : state.boolValues) file << "bool\t" << Escape(it.first) << "\t" << (it.second ? 1 : 0) << "\n";
    for (const auto& it : state.floatValues) file << "float\t" << Escape(it.first) << "\t" << it.second << "\n";
    for (const auto& it : state.intValues) file << "int\t" << Escape(it.first) << "\t" << it.second << "\n";
    for (const auto& it : state.stringValues) file << "string\t" << Escape(it.first) << "\t" << Escape(it.second) << "\n";
    for (const auto& it : state.sectionOpenValues) file << "section\t" << Escape(it.first) << "\t" << (it.second ? 1 : 0) << "\n";
}

bool Load(std::istream& file, ModuleState& state, const FeatureManifest* manifest) {
    std::unordered_map<std::string, const FeatureComponent*> components = CollectComponents(manifest);
    std::string line;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string kind, id, value;
        std::getline(ss, kind, '\t');
        std::getline(ss, id, '\t');
        std::getline(ss, value);
        id = Unescape(id);
        value = Unescape(value);

        if (kind == "themePreset") {
            int preset = std::max(0, std::min(4, atoi(id.c_str())));
            Theme::SetPreset((ThemePreset)preset);
        } else if (kind == "activeTab") {
            state.activeTab = std::max(0, atoi(id.c_str()));
        } else if (kind == "bool" && state.boolValues.find(id) != state.boolValues.end()) {
            state.boolValues[id] = atoi(value.c_str()) != 0;
        } else if (kind == "float" && state.floatValues.find(id) != state.floatValues.end()) {
            float parsed = (float)atof(value.c_str());
            auto found = components.find(id);
            if (found != components.end()) parsed = ClampFloat(parsed, found->second->minValue, found->second->maxValue);
            state.floatValues[id] = parsed;
        } else if (kind == "int" && state.intValues.find(id) != state.intValues.end()) {
            int parsed = atoi(value.c_str());
            auto found = components.find(id);
            if (found != components.end() && !found->second->options.empty()) {
                parsed = std::max(0, std::min(parsed, (int)found->second->options.size() - 1));
            }
            state.intValues[id] = parsed;
        } else if (kind == "string" && state.stringValues.find(id) != state.stringValues.end()) {
            state.stringValues[id] = value;
        } else if (kind == "section" && state.sectionOpenValues.find(id) != state.sectionOpenValues.end()) {
            state.sectionOpenValues[id] = atoi(value.c_str()) != 0;
        }
    }
    return true;
}
}

bool SaveConfig(const UIState&) {
    return true;
}

bool LoadConfig(UIState&) {
    return true;
}

void ResetConfig(UIState& state) {
    state = UIState();
}

bool SaveModuleConfig(const std::string& moduleId, const ModuleState& state, const FeatureManifest* manifest) {
    if (moduleId.empty()) return false;
    EnsureDirectory();
    std::ofstream file(PathForModule(moduleId), std::ios::trunc);
    if (!file.is_open()) {
        g_LastConfigStatus = "Save failed";
        return false;
    }
    StateSerializer::Save(file, moduleId, state, manifest);
    g_LastConfigStatus = "Saved " + moduleId;
    return true;
}

bool LoadModuleConfig(const std::string& moduleId, ModuleState& state, const FeatureManifest* manifest) {
    if (moduleId.empty()) return false;
    std::ifstream file(PathForModule(moduleId));
    if (!file.is_open()) {
        g_LastConfigStatus = "Missing config";
        return false;
    }
    StateSerializer::Load(file, state, manifest);
    g_LastConfigStatus = "Loaded " + moduleId;
    return true;
}

bool LoadModuleConfig(const std::string& moduleId, ModuleState& state) {
    return LoadModuleConfig(moduleId, state, nullptr);
}

bool ResetModuleConfig(const std::string& moduleId) {
    if (moduleId.empty()) return false;
    std::remove(PathForModule(moduleId).c_str());
    g_LastConfigStatus = "Reset " + moduleId;
    return true;
}

std::string GetModuleConfigPath(const std::string& moduleId) {
    return PathForModule(moduleId);
}
}

namespace ProfileManager {
std::string SanitizeId(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (std::isalnum((unsigned char)ch)) out += (char)std::tolower((unsigned char)ch);
        else if (ch == '_' || ch == '-' || ch == ' ') out += '_';
    }
    if (out.empty()) out = "profile";
    return out;
}

static std::string PathForProfile(const std::string& moduleId, const std::string& profileId) {
    std::string base = g_ConfigDirectory.empty() ? "configs" : g_ConfigDirectory;
    return base + "/" + moduleId + "_" + profileId + ".profile";
}

std::vector<ModuleProfile> GetProfiles(const std::string& moduleId) {
    return {
        {"default", "Default", moduleId, ModuleState()},
        {"testing", "Testing", moduleId, ModuleState()},
        {"custom_1", "Custom 1", moduleId, ModuleState()},
        {"custom_2", "Custom 2", moduleId, ModuleState()},
    };
}

bool SaveProfile(const std::string& moduleId, const std::string& profileId,
                 const ModuleState& state, const FeatureManifest* manifest) {
    if (moduleId.empty() || profileId.empty()) return false;
    Config::EnsureDirectory();
    std::ofstream file(PathForProfile(moduleId, profileId), std::ios::trunc);
    if (!file.is_open()) return false;
    Config::StateSerializer::Save(file, moduleId, state, manifest, profileId);
    return true;
}

bool LoadProfile(const std::string& moduleId, const std::string& profileId,
                 ModuleState& state, const FeatureManifest* manifest) {
    if (profileId == "default") return Config::LoadModuleConfig(moduleId, state, manifest);
    std::ifstream file(PathForProfile(moduleId, profileId));
    if (!file.is_open()) return false;
    return Config::StateSerializer::Load(file, state, manifest);
}

bool ResetProfile(const std::string& moduleId, const std::string& profileId) {
    if (moduleId.empty() || profileId.empty()) return false;
    std::remove(PathForProfile(moduleId, profileId).c_str());
    return true;
}

bool CreateProfile(const std::string& moduleId, const std::string& profileName) {
    std::string profileId = SanitizeId(profileName);
    ModuleState empty;
    return SaveProfile(moduleId, profileId, empty, nullptr);
}

bool DeleteProfile(const std::string& moduleId, const std::string& profileId) {
    if (profileId == "default") return false;
    return ResetProfile(moduleId, profileId);
}

bool RenameProfile(const std::string&, const std::string&, const std::string&) {
    return true;
}
}
