#pragma once

#include <cstdint>
#include <string>
#include <vector>

// =====================================================================
// UI 侧的数据源（原来叫 Bridge，是读文件；现在直连 mmd_get_api）
//
// 名字保留是因为 Screens.cpp 一大堆地方调 Bridge::xxx，接口形状不动，
// 只把内部实现换成「同进程直调 libmmdruntime.so 导出的函数表」。
//
// 好处（对比之前的文件通道）：
//   · 零延迟：setter 直接改游戏侧内存里的命令位
//   · 没有 SELinux / 目录权限 / stopped state 这些幺蛾子
//   · 不需要 root 建 /data/local/tmp/mmdrt
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
        case GameState::Unknown:    return "等待连接";
        case GameState::Detached:   return "未就绪";
        case GameState::Attached:   return "已连接";
        case GameState::Loading:    return "加载中";
        case GameState::Active:     return "生效中";
        case GameState::ErrorState: return "出错";
    }
    return "未知";
}

// UI 侧唯一数据源。字段语义跟旧版一致，Screens.cpp 直接读。
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
    std::string statusMessage = "等待游戏侧 so…";
    std::string gameVersion = "libmmdruntime";
    std::string activeModel = "无";
    int lastResult = -1;
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

// 绑定 libmmdruntime.so 的 mmd_get_api()。幂等，内部限速每秒重试一次。
bool Bind();
bool Connected();

// 从 mmd_get_api 拉一次状态填进 Shared()。每帧调都行。
void PollStatus();

// 扫本地模型目录（/sdcard 下），填 models 列表
void RefreshModelList();

void SetStorageDir(const std::string& path);
void SetEnableReplace(bool value);
void SetModelScale(float value);
void SetModelPath(const std::string& value);
void SetShowBounds(bool value);
void SetManualOffset(float x, float y, float z);
void SelectModel(const std::string& name);
void RequestInit();    // 初始化：绑 il2cpp + 扫 SMR，不建模型
void RequestRescan();
void RequestReapply();
void ImportModelsFromConfig();

// ---- 自由视角摄像机（透传到 mmd_get_api）----
int  CameraEnable(int on);      // 返回 1 = 开启成功
int  CameraEnabled();
void CameraReset();
void CameraZoom(float delta);   // 相对量，正数拉远
void CameraMotion(int ascend, int descend, int boost);   // 灵魂出窍：上升/下降/加速
void CameraConfig(float speed, float sens, float radius); // 速度 / 灵敏度 / 摇杆半径

// 让游戏侧在 UnityMain 线程跑一帧（UI 的 eglSwapBuffers 里每帧调）
void RenderTick();

}  // namespace Bridge
