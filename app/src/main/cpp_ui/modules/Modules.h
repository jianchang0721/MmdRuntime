#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "TemplateModels.h"

class IModule {
public:
    virtual ~IModule() = default;
    virtual const char* GetModuleId() const = 0;
    virtual const char* GetGameId() const = 0;
    virtual const char* GetDisplayName() const = 0;
    virtual const char* GetVersion() const = 0;
    virtual FeatureManifest GetManifest() const = 0;
    virtual void OnLoad() {}
    virtual void OnUnload() {}
    virtual void OnComponentChanged(const std::string&) {}
};

namespace TemplateData {
const std::vector<GameProfile>& Games();
}

class ModuleManager {
public:
    void RegisterModule(std::unique_ptr<IModule> module);
    bool LoadModuleForGame(const std::string& gameId);
    bool LoadModuleById(const std::string& moduleId);
    void UnloadCurrentModule();
    bool ReloadActiveModule();
    void ResetActiveModuleState();
    bool IsModuleLoaded() const;
    bool HasActiveModule() const;
    IModule* GetActiveModule();
    const FeatureManifest* GetActiveManifest() const;
    std::string GetLastError() const;
    std::string GetLastValidationError() const;
    std::string GetLastSelectedGameId() const;
    std::string GetLastSelectedGameName() const;

private:
    bool ValidateComponent(const FeatureComponent& component, std::unordered_set<std::string>& componentIds) const;
    ManifestValidationResult ValidateManifest(const FeatureManifest& manifest) const;

    std::vector<std::unique_ptr<IModule>> modules;
    IModule* activeModule = nullptr;
    FeatureManifest activeManifest;
    std::string lastError;
    std::string lastValidationError;
    std::string lastSelectedGameId;
    std::string lastSelectedGameName;
    std::string activeModuleId;
    mutable std::string lastMutableValidationError;
};

std::unique_ptr<IModule> CreateTestModule();
std::unique_ptr<IModule> CreateDemoModule();
std::unique_ptr<IModule> CreateBasicModule();
