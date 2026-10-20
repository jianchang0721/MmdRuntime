#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BridgeConfig.h"

// =====================================================================
// UI 侧的 UI/Game 桥
//
// 设计要点（血泪史）：
//   1. 所有 set 都先落在内存结构体 Shared() 上，再统一 Flush 到文件。
//      绝不一改就写一次盘 —— 拖滑条时会几十次重写入。
//   2. 写盘用 rename 原子替换，读侧永远不会看到半截文件。
//   3. 命令（rescan / reapply）走 cmd.xxx 通道，Game 读完自行清除。
// =====================================================================

namespace Bridge {

enum class GameState {
    Unknown,
    Detached,
    Attached,
    Loading,
    Active,
    ErrorState
};

inline const char* GameStateName(GameState state) {
    switch (state) {
        case GameState::Unknown:    return "unknown";
        case GameState::Detached:   return "detached";
        case GameState::Attached:   return "attached";
        case GameState::Loading:    return "loading";
        case GameState::Active:     return "active";
        case GameState::ErrorState: return "error";
    }
    return "unknown";
}

inline GameState ParseGameState(const std::string& raw) {
    if (raw == "attached") return GameState::Attached;
    if (raw == "loading")  return GameState::Loading;
    if (raw == "active")   return GameState::Active;
    if (raw == "error")    return GameState::ErrorState;
    if (raw == "detached") return GameState::Detached;
    return GameState::Unknown;
}

// =====================================================================
// UI 侧的唯一数据源
// =====================================================================
struct BridgeModel {
    // ---- UI -> Game ----
    bool  enableReplace = false;
    float modelScale = 1.0f;
    std::string modelPath;
    std::string storageDir;
    bool  showBounds = false;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float offsetZ = 0.0f;

    // ---- Game -> UI ----
    GameState state = GameState::Unknown;
    std::string statusMessage = "等待启动…";
    std::string gameVersion = "unknown";
    std::string activeModel = "无";
    int lastResult = 0;
    int lastErrorCode = 0;
    std::string lastErrorMessage;
    int smrTotal = 0;
    int smrReplaced = 0;
    int boneCount = 0;
    int vertexCount = 0;
    float replaceProgress = 0.0f;

    // ---- 本地 ----
    std::vector<std::string> models;
    int selectedModel = 0;
    std::string pendingCommand;
    int64_t lastWriteMs = 0;
    int64_t lastReadMs = 0;
};

BridgeModel& Shared();

void EnsureDirectory();
void FlushUiConfig();
void PollStatus();
void RefreshModelList();

bool SetStorageDir(const std::string& path);
void SetEnableReplace(bool value);
void SetModelScale(float value);
void SetModelPath(const std::string& value);
void SetShowBounds(bool value);
void SetManualOffset(float x, float y, float z);
void SelectModel(const std::string& name);
void RequestRescan();
void RequestReapply();
void ImportModelsFromConfig();

}  // namespace Bridge
