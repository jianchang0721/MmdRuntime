// =====================================================================
// camera_ctrl.cpp
//
// 复用 scan_smrs 那套：FindObjectsOfTypeAll(Camera) 拿所有摄像机，
// 挑 enabled 且 depth 最大的当主摄像机。
//
// 方法查找踩过的坑（build-48 用血换来的）：
//   set_position(Vector3) / set_rotation(Quaternion) 这些有重载，
//   必须按「第一个参数的类名」筛，只按参数个数筛会撞错，
//   而撞错的表现是 runtime_invoke 不抛异常、安安静静返回 nullptr。
// =====================================================================
#include "camera_ctrl.h"

#include <android/log.h>
#include <cmath>
#include <cstring>

#include "unity/il2cpp_api.h"
#include "logsink.h"
#include "unity/unity_objects.h"

#define CTAG "MmdCam"
#define CLOG(...) logsink::Write(__VA_ARGS__)

namespace game {

namespace {

// ---- il2cpp 引用 ----
void* g_camClass = nullptr;
void* g_findAll = nullptr;
void* g_getTr = nullptr;
void* g_setPos = nullptr;
void* g_setRot = nullptr;
void* g_setFov = nullptr;
void* g_getDepth = nullptr;
void* g_getEnabled = nullptr;

bool g_refsReady = false;

bool bind_refs() {
    if (g_refsReady) return true;

    g_camClass = uo::u_class("UnityEngine", "Camera");
    void* resClass = uo::u_class("UnityEngine", "Resources");
    if (!g_camClass || !resClass) {
        CLOG("[CAM] class missing cam=%p res=%p", (void*)g_camClass, (void*)resClass);
        return false;
    }

    g_findAll = uo::u_method(resClass, "FindObjectsOfTypeAll", 1);

    void* compClass = uo::u_class("UnityEngine", "Component");
    if (compClass) g_getTr = uo::u_method(compClass, "get_transform", 0);
    if (!g_getTr) g_getTr = uo::u_method(g_camClass, "get_transform", 0);

    void* trClass = uo::u_class("UnityEngine", "Transform");
    if (trClass) {
        g_setPos = uo::u_method_typed(trClass, "set_position", 1, "Vector3");
        g_setRot = uo::u_method_typed(trClass, "set_rotation", 1, "Quaternion");
    }
    g_setFov    = uo::u_method(g_camClass, "set_fieldOfView", 1);
    g_getDepth  = uo::u_method(g_camClass, "get_depth", 0);
    g_getEnabled = uo::u_method(g_camClass, "get_enabled", 0);

    CLOG("[CAM] find=%p getTr=%p setPos=%p setRot=%p setFov=%p depth=%p",
         (void*)g_findAll, (void*)g_getTr, (void*)g_setPos,
         (void*)g_setRot, (void*)g_setFov, (void*)g_getDepth);

    if (!g_findAll || !g_getTr || !g_setPos || !g_setRot) {
        CLOG("[CAM] 关键方法缺失，自由视角不可用");
        return false;
    }
    g_refsReady = true;
    return true;
}

// ---- 触摸状态 ----
struct TouchSlot {
    bool  active = false;
    float startX = 0.0f;
    float startY = 0.0f;
    float lastX  = 0.0f;
    float lastY  = 0.0f;
    int   side   = 0;   // 0 = 左（平移） 1 = 右（旋转）
};

const int kMaxPointers = 6;
TouchSlot g_slots[kMaxPointers];

int find_free_slot() {
    for (int i = 0; i < kMaxPointers; i++) if (!g_slots[i].active) return i;
    return -1;
}

int find_slot_by_id(int id) {
    return (id >= 0 && id < kMaxPointers) ? id : -1;
}

}  // namespace

CameraCtrl g_camera;

// =====================================================================
// 扫描
// =====================================================================
int CameraCtrl::scan() {
    if (!bind_refs()) return 0;

    Il2CppReflectionType* t = g_api.type_get_object(g_api.class_get_type((Il2CppClass*)g_camClass));
    if (!t) { CLOG("[CAM] type_get_object 失败"); return 0; }

    void* args[1] = { t };
    Il2CppObject* arr = (Il2CppObject*)uo::u_call(nullptr, g_findAll, args);
    if (!arr) { CLOG("[CAM] FindObjectsOfTypeAll 返回 null"); return 0; }

    size_t n = ((Il2CppArray*)arr)->max_length;
    CLOG("[CAM] 场景里有 %zu 个 Camera", n);

    float bestDepth = -1e9f;
    void* bestCam = nullptr;
    int valid = 0;

    for (size_t i = 0; i < n; i++) {
        void* cam = *(void**)uo::u_array_data_from(arr, i);
        if (!cam) continue;
        valid++;

        // get_enabled() 返回 boxed bool，值在 +0x10
        bool en = true;
        if (g_getEnabled) {
            Il2CppObject* boxed = (Il2CppObject*)uo::u_call(cam, g_getEnabled, nullptr);
            if (boxed) en = (*(uint8_t*)((char*)boxed + 16)) != 0;
        }
        if (!en) continue;

        float depth = 0.0f;
        if (g_getDepth) {
            Il2CppObject* boxed = (Il2CppObject*)uo::u_call(cam, g_getDepth, nullptr);
            if (boxed) depth = *(float*)((char*)boxed + 16);
        }

        if (depth > bestDepth) { bestDepth = depth; bestCam = cam; }
    }

    if (!bestCam) {
        CLOG("[CAM] 没有 enabled 的摄像机（valid=%d）", valid);
        return 0;
    }

    Il2CppObject* tr = (Il2CppObject*)uo::u_call(bestCam, g_getTr, nullptr);
    if (!tr) { CLOG("[CAM] get_transform 失败"); return 0; }

    camera = bestCam;
    transform = tr;
    scanned = true;
    CLOG("[CAM] 主摄像机 cam=%p tr=%p depth=%.2f", camera, transform, bestDepth);
    return valid;
}

// =====================================================================
// 每帧更新
// =====================================================================
void CameraCtrl::update() {
    if (!enabled || !transform || !g_refsReady) return;

    // 目标点：先读当前摄像机对着的位置（首次），之后可跟随角色
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;

    if (followTarget) {
        // TODO: 读玩家角色位置填 target[]。先保持手动设的值。
    }

    // 球坐标 → 世界坐标（Unity 左手系，Z 向前）
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cyw = cosf(yaw),  syw = sinf(yaw);

    cx = target[0] + dist * cp * syw;
    cy = target[1] + dist * sp;
    cz = target[2] + dist * cp * cyw;

    struct { float x, y, z; } pos = { cx, cy, cz };
    void* a1[1] = { &pos };
    uo::u_call(transform, g_setPos, a1);

    // 摄像机要看向 target：pitch 取负，因为「摄像机抬头」= 欧拉角减小
    // Unity 的 Quaternion 顺序是 (x, y, z, w)
    float halfP = (pitch) * 0.5f;
    float halfY = (yaw) * 0.5f;
    float q0 = sinf(halfP) * cosf(halfY);
    float q1 = sinf(halfP) * sinf(halfY);
    float q2 = cosf(halfP) * sinf(halfY);
    float q3 = cosf(halfP) * cosf(halfY);
    float q[4] = { q0, q1, q2, q3 };

    void* a2[1] = { q };
    uo::u_call(transform, g_setRot, a2);

    if (g_setFov) {
        float fov = 55.0f;
        void* a3[1] = { &fov };
        uo::u_call(camera, g_setFov, a3);
    }
}

void CameraCtrl::reset() {
    dist = 4.0f;
    yaw = 0.0f;
    pitch = 0.25f;
    target[0] = 0.0f;
    target[1] = 1.0f;
    target[2] = 0.0f;
}

void CameraCtrl::orbit(float dYaw, float dPitch) {
    yaw += dYaw;
    pitch += dPitch;
    // 夹住，别翻过头
    const float lim = 1.45f;   // ~83°
    if (pitch > lim) pitch = lim;
    if (pitch < -lim) pitch = -lim;
    if (yaw > 3.14159f * 2.0f) yaw -= 3.14159f * 2.0f;
    if (yaw < 0.0f) yaw += 3.14159f * 2.0f;
}

void CameraCtrl::zoom(float d) {
    dist += d;
    if (dist < 0.5f) dist = 0.5f;
    if (dist > 30.0f) dist = 30.0f;
}

void CameraCtrl::pan(float fwd, float right, float up) {
    const float cyw = cosf(yaw), syw = sinf(yaw);
    // 前后：沿摄像机朝向的水平投影
    target[0] += fwd * syw + right * cyw;
    target[2] += fwd * cyw - right * syw;
    target[1] += up;
    if (target[1] < -5.0f) target[1] = -5.0f;
    if (target[1] > 30.0f) target[1] = 30.0f;
}

// =====================================================================
// 触摸：左半屏平移，右半屏旋转
// =====================================================================
bool camera_touch(int action, int pointerId, float x, float y,
                  float viewW, float viewH) {
    if (!g_camera.enabled) return false;

    const int slot = find_slot_by_id(pointerId);
    if (slot < 0) return false;
    TouchSlot& s = g_slots[slot];

    const float halfW = viewW * 0.5f;
    const float kRotatePerPixel = 0.004f;   // 弧度/像素
    const float kPanPerPixel    = 0.010f;   // 世界单位/像素

    switch (action) {
        case 0: {  // DOWN
            if (s.active) return false;
            s.active = true;
            s.startX = s.lastX = x;
            s.startY = s.lastY = y;
            s.side = (x < halfW) ? 0 : 1;
            return true;
        }

        case 2: {  // MOVE
            if (!s.active) return false;
            const float dx = x - s.lastX;
            const float dy = y - s.lastY;
            s.lastX = x;
            s.lastY = y;

            if (s.side == 1) {
                g_camera.orbit(dx * kRotatePerPixel, -dy * kRotatePerPixel);
            } else {
                // 左半屏：上下 = 前后推拉，左右 = 平移
                g_camera.pan(-dy * kPanPerPixel, dx * kPanPerPixel, 0.0f);
            }
            return true;
        }

        case 1:   // UP
        case 3: { // CANCEL
            if (!s.active) return false;
            s.active = false;
            return true;
        }

        default:
            return false;
    }
}

void camera_set_enabled(int on) {
    const bool want = (on != 0);
    if (want && !g_camera.scanned) {
        // 第一次开：先扫
        int n = g_camera.scan();
        if (n <= 0) {
            CLOG("[CAM] 开不起来：场景里没有可用摄像机");
            return;
        }
    }
    g_camera.enabled = want;
    CLOG("[CAM] %s", want ? "自由视角开" : "自由视角关");
}

int camera_is_enabled() { return g_camera.enabled ? 1 : 0; }

}  // namespace game
