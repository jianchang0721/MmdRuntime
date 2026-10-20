#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "TemplateModels.h"

namespace Config {
std::string PathForModule(const std::string& moduleId);
void EnsureDirectory();
std::string Escape(const std::string& value);
std::string Unescape(const std::string& value);
bool SaveConfig(const UIState& state);
bool LoadConfig(UIState& state);
void ResetConfig(UIState& state);
bool SaveModuleConfig(const std::string& moduleId, const ModuleState& state, const FeatureManifest* manifest = nullptr);
bool LoadModuleConfig(const std::string& moduleId, ModuleState& state, const FeatureManifest* manifest = nullptr);
bool LoadModuleConfig(const std::string& moduleId, ModuleState& state);
bool ResetModuleConfig(const std::string& moduleId);
std::string GetModuleConfigPath(const std::string& moduleId);

namespace StateSerializer {
void Save(std::ostream& file, const std::string& moduleId, const ModuleState& state,
          const FeatureManifest* manifest, const std::string& profileId = "");
bool Load(std::istream& file, ModuleState& state, const FeatureManifest* manifest);
}
}

namespace ProfileManager {
struct ModuleProfile {
    std::string profileId;
    std::string displayName;
    std::string moduleId;
    ModuleState state;
};

std::string SanitizeId(const std::string& value);
std::vector<ModuleProfile> GetProfiles(const std::string& moduleId);
bool SaveProfile(const std::string& moduleId, const std::string& profileId,
                 const ModuleState& state, const FeatureManifest* manifest = nullptr);
bool LoadProfile(const std::string& moduleId, const std::string& profileId,
                 ModuleState& state, const FeatureManifest* manifest = nullptr);
bool ResetProfile(const std::string& moduleId, const std::string& profileId);
bool CreateProfile(const std::string& moduleId, const std::string& profileName);
bool DeleteProfile(const std::string& moduleId, const std::string& profileId);
bool RenameProfile(const std::string& moduleId, const std::string& profileId, const std::string& newName);
}
