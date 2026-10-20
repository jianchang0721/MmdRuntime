#include "Bridge.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "AppState.h"

namespace Bridge {

static BridgeModel g_Model;
static BridgeConfig::KeyValueTable g_UiConfig;
static int64_t g_LastStatusRead = 0;

BridgeModel& Shared() { return g_Model; }

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void EnsureDirectory() {
    mkdir(BridgeConfig::BRIDGE_DIRECTORY, 0777);
    ::chmod(BridgeConfig::BRIDGE_DIRECTORY, 0777);
}

// ------------------------------------------------------------------
// UI -> Game：把内存里的最新值一次性刷进 ui.conf
// ------------------------------------------------------------------
void FlushUiConfig() {
    EnsureDirectory();
    BridgeModel& model = Shared();

    g_UiConfig.SetBool("enable_replace", model.enableReplace);
    g_UiConfig.SetFloat("model_scale", model.modelScale);
    g_UiConfig.Set("model_path", model.modelPath);
    g_UiConfig.SetBool("show_bounds", model.showBounds);
    g_UiConfig.SetFloat("offset_x", model.offsetX);
    g_UiConfig.SetFloat("offset_y", model.offsetY);
    g_UiConfig.SetFloat("offset_z", model.offsetZ);
    g_UiConfig.Set("active_model",
                   model.models.empty() ? "" : model.models[model.selectedModel]);

    // 命令：写完由 UI 侧清除标记，Game 下次轮询看到即为一次请求
    if (!model.pendingCommand.empty()) {
        g_UiConfig.Set("cmd." + model.pendingCommand, "1");
        model.pendingCommand.clear();
    }

    g_UiConfig.Set("version", std::to_string(g_UiConfig.GetBumpVersion()));
    g_UiConfig.SetInt("write_time_ms", (int)NowMs());

    const bool ok = g_UiConfig.SaveToFile(BridgeConfig::UiFilePath());
    ::chmod(BridgeConfig::UiFilePath().c_str(), 0666);
    model.lastWriteMs = NowMs();
    g_LastConfigStatus = ok ? "Bridge write ok" : "Bridge write failed";
}

// ------------------------------------------------------------------
// Game -> UI：mtime 变了才重新解析
// ------------------------------------------------------------------
void PollStatus() {
    EnsureDirectory();
    struct stat info {};
    if (::stat(BridgeConfig::StatusFilePath().c_str(), &info) != 0) return;
    const int64_t mtime = (int64_t)info.st_mtim.tv_sec * 1000 + info.st_mtim.tv_nsec / 1000000;
    if (mtime == g_LastStatusRead) return;
    g_LastStatusRead = mtime;

    BridgeConfig::KeyValueTable loaded;
    if (!loaded.LoadFromFile(BridgeConfig::StatusFilePath())) return;

    BridgeModel& model = Shared();
    model.state = ParseGameState(loaded.GetString("game_state", "unknown"));
    model.statusMessage = loaded.GetString("status_message", "等待启动…");
    model.gameVersion = loaded.GetString("game_version", "unknown");
    model.activeModel = loaded.GetString("active_model", "无");
    model.lastResult = loaded.GetInt("last_result", 0);
    model.lastErrorCode = loaded.GetInt("last_error_code", 0);
    model.lastErrorMessage = loaded.GetString("last_error_message");
    model.smrTotal = loaded.GetInt("smr_total", 0);
    model.smrReplaced = loaded.GetInt("smr_replaced", 0);
    model.boneCount = loaded.GetInt("bone_count", 0);
    model.vertexCount = loaded.GetInt("vertex_count", 0);
    model.replaceProgress = loaded.GetFloat("replace_progress", 0.0f);
    model.lastReadMs = NowMs();
}

// ------------------------------------------------------------------
// 模型列表：目录条目 + 单个 .pmx 文件都收
// ------------------------------------------------------------------
void RefreshModelList() {
    BridgeModel& model = Shared();
    if (model.storageDir.empty()) model.storageDir = "/sdcard/MT2/ModelMod/ngyu";
    model.models.clear();

    DIR* handle = opendir(model.storageDir.c_str());
    if (!handle) return;
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (entry->d_type == DT_DIR) {
            model.models.push_back(name);
        } else {
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos && name.substr(dot) == ".pmx") {
                model.models.push_back(name);
            }
        }
    }
    closedir(handle);
    std::sort(model.models.begin(), model.models.end());

    if (model.selectedModel >= (int)model.models.size()) model.selectedModel = 0;
    if (!model.models.empty() && model.modelPath.empty()) {
        model.modelPath = model.storageDir + "/" + model.models[model.selectedModel];
    }
}

// ------------------------------------------------------------------
// setters
// ------------------------------------------------------------------
bool SetStorageDir(const std::string& path) {
    Shared().storageDir = path;
    RefreshModelList();
    FlushUiConfig();
    return true;
}

void SetEnableReplace(bool value) { Shared().enableReplace = value; FlushUiConfig(); }
void SetModelScale(float value) { Shared().modelScale = value; FlushUiConfig(); }
void SetModelPath(const std::string& value) { Shared().modelPath = value; FlushUiConfig(); }
void SetShowBounds(bool value) { Shared().showBounds = value; FlushUiConfig(); }

void SetManualOffset(float x, float y, float z) {
    BridgeModel& model = Shared();
    model.offsetX = x;
    model.offsetY = y;
    model.offsetZ = z;
    FlushUiConfig();
}

void SelectModel(const std::string& name) {
    BridgeModel& model = Shared();
    for (size_t i = 0; i < model.models.size(); ++i) {
        if (model.models[i] == name) {
            model.selectedModel = (int)i;
            model.modelPath = model.storageDir + "/" + name;
        }
    }
    FlushUiConfig();
}

void RequestRescan() { Shared().pendingCommand = "rescan"; FlushUiConfig(); }
void RequestReapply() { Shared().pendingCommand = "reapply"; FlushUiConfig(); }

void ImportModelsFromConfig() {
    BridgeConfig::KeyValueTable loaded;
    if (loaded.LoadFromFile(BridgeConfig::UiFilePath())) {
        const std::string path = loaded.GetString("model_path", "");
        if (!path.empty()) Shared().modelPath = path;
    }
    RefreshModelList();
}

}  // namespace Bridge
