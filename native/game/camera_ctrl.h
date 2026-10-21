// =====================================================================
// camera_ctrl.h —— 自由视角摄像机
//
// 思路：不创建任何对象，只扫描场景里现成的 Camera 并每帧改它的
// position / rotation / fieldOfView。
//
// 控制方式（复用游戏原生操作区，玩家不用学新布局）：
//   · 左半屏拖动 = 平移（前后左右 + 上下）
//   · 右半屏拖动 = 旋转（yaw / pitch）
//   · 双指同时 = 平移 + 旋转一起做
//   · 摄像机默认绕当前目标角色公转，角色动它跟着动
//
// 为什么放 eglSwapBuffers 里更新：游戏如果在 Update 里写摄像机，
// 我们的 16ms tick 不一定在它之后；渲染前最后一写才保险。
// =====================================================================
#pragma once

namespace game {

struct CameraCtrl {
    void* camera    = nullptr;   // Il2CppObject* (UnityEngine.Camera)
    void* transform = nullptr;   // 它的 Transform
    bool  enabled   = false;
    bool  scanned   = false;

    // ---- 灵魂出窍模式（用户需求）----
    // 本体不动，摄像机脱离角色，用游戏场景的摇杆推着飞。
    // freeFly=false 时退回旧的「绕 target 公转」。
    bool  freeFly   = true;      // 默认就是灵魂出窍
    float flySpeed  = 6.0f;      // 米/秒（摇杆满推）
    float camPos[3] = {0, 0, 0}; // freeFly 下的世界坐标（首次启用时取当前相机位置）

    // 球坐标（绕 target 公转）
    float dist  = 4.0f;
    float yaw   = 0.0f;
    float pitch = 0.25f;
    float target[3] = {0.0f, 1.0f, 0.0f};
    bool  followTarget = true;   // true = 每帧读目标角色位置

    int  scan();                 // 找主摄像机，返回找到几个
    void update();               // 每帧写摄像机
    void reset();
    void orbit(float dYaw, float dPitch);
    void zoom(float d);
    void pan(float fwd, float right, float up);
};

extern CameraCtrl g_camera;

// 喂一个指针的触摸事件。返回 true = 这个触摸归摄像机，别给游戏。
// action: 0=DOWN 1=UP 2=MOVE 3=CANCEL
bool camera_touch(int action, int pointerId, float x, float y, float viewW, float viewH);

// 摄像机模式开关（UI 调用）
void camera_set_enabled(int on);
int  camera_is_enabled();

}  // namespace game
