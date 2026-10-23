// =====================================================================
// camera_ctrl.h —— 自由视角摄像机（灵魂出窍）
//
// 目标机位由 freecam 新建的相机承担（见 freecam.h），这里只负责
// 「每帧算该摆哪儿、朝哪边」，以及吃掉属于自己的触摸。
//
// 控制方式（照用户 C# 参考实现 AndroidFreeCamera）：
//   · 左半屏 = 虚拟摇杆：按下的点当圆心，拖多远就是推多少（半径 joyRadius 满推）
//   · 右半屏（上半 1/4 以外）= 拖视角，yaw/pitch 增量
//   · 上升 / 下降 / 加速：三个开关，UI 面板里点，按住式后面再说
//
// 为什么要放 eglSwapBuffers 里更新：游戏如果在 Update 里写相机，
// 我们的 tick 不一定在它之后；渲染前最后一写才保险。
// （现在相机是我们自己的，这条其实已经不致命，但保持原样。）
// =====================================================================
#pragma once

namespace game {

struct CameraCtrl {
    void* camera    = nullptr;   // Il2CppObject* (UnityEngine.Camera)
    void* transform = nullptr;   // 它的 Transform
    bool  enabled   = false;
    bool  scanned   = false;

    // ---- 灵魂出窍模式（用户 C# 参考实现）----
    // 本体不动，相机脱离角色自己飞；触摸我们自己吃，不指望游戏 Input Manager。
    bool  freeFly   = true;
    float flySpeed  = 12.0f;     // 米/秒（摇杆满推）
    float boostMul  = 3.0f;      // 加速倍率
    float lookSens  = 0.30f;     // 度/像素
    float joyRadius = 80.0f;     // 摇杆半径（像素）＝ 满推距离
    // 上面三个会被 /data/data/com.hermes.h1game/files/mmd_cam.cfg 覆盖（如果文件在）

    bool  ascend  = false;
    bool  descend = false;
    bool  boost   = false;

    float camPos[3] = {0.0f, 0.0f, 0.0f};   // freeFly 下的世界坐标
    bool  needSync  = true;      // 下次 update 把 camPos 对齐到相机真实位置
    bool  cmdReset  = false;     // UI 点了复位，update 里消费（UnityMain 线程）
    bool  syncFailed = false;    // 对齐失败过：别让相机瞬移

    // 自由相机自己的触摸状态（左半屏摇杆 + 右半屏转视角）
    int   moveFinger  = -1;
    float moveOriginX = 0.0f;
    float moveOriginY = 0.0f;
    float moveInputX  = 0.0f;
    float moveInputY  = 0.0f;
    int   lookFinger  = -1;
    float lookLastX   = 0.0f;
    float lookLastY   = 0.0f;

    // 球坐标（绕 target 公转，freeFly=false 时用）
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

// 锚定的角色 Transform（module.cpp 在 census 之后塞进来；复位时拉回这里）
void camera_set_follow(void* charTransform);

// 喂一个指针的触摸事件。返回 true = 这个触摸归摄像机，别给游戏。
// action: 0=DOWN 1=UP 2=MOVE 3=CANCEL
bool camera_touch(int action, int pointerId, float x, float y, float viewW, float viewH);

// 摄像机模式开关（UI 调用）。只记请求，真正执行在 camera_pump()。
void camera_set_enabled(int on);
int  camera_is_enabled();

// UnityMain 每帧调（tick_body 里）：消费开关请求。
void camera_pump();

// 上升 / 下降 / 加速（UI 调用）
void camera_set_motion(int ascend, int descend, int boost);

// 速度 / 灵敏度 / 摇杆半径（UI 调用）
void camera_set_config(float speed, float sens, float radius);
// 读当前值：out[0]=速度 out[1]=灵敏度 out[2]=摇杆半径
void camera_get_config(float* out);

// 摇杆状态（UI 画可视化圆）。返回 1 = 正在推。
int camera_stick(float* out);

}  // namespace game
