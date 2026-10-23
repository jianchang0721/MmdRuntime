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
//
// 2026-10-23 灵魂出窍重写（照用户 C# 参考实现 AndroidFreeCamera）：
//   · 输入换成自建虚拟摇杆（左半屏）+ 右半屏拖视角，不再指望 Input.GetAxisRaw
//   · 移动方向含 pitch（抬头前飞会爬升）、归一化（斜向不加速）、真实 dt
//   · 补上 上升/下降/加速
//   · 写入改 managed setter 优先 —— 目标是我们自己新建的相机，没人抢，
//     managed 写一定生效且会正确标 dirty；native 直写降级成兜底
//   · 每帧查相机存活，切场景被 Unity 回收后自动重建
// =====================================================================
#include "camera_ctrl.h"

#include <android/log.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <time.h>

#include "unity/il2cpp_api.h"
#include "cam_override.h"
#include "cam_native.h"
#include "freecam.h"
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
void* g_getPos = nullptr;      // Transform.get_position
void* g_getRot = nullptr;      // Transform.get_rotation
void* g_getAxisRaw = nullptr;  // UnityEngine.Input.GetAxisRaw(string)，静态
void* g_inputClass = nullptr;

// 重建时用来跳过我们自己的相机：gchandle 钉住过的 managed 对象地址不会被复用，
// 所以直接比指针是安全的。
void* g_skipCam = nullptr;

// 锚定的角色 Transform（module.cpp 塞进来）
void* g_followTr = nullptr;

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
        g_getPos = uo::u_method_typed(trClass, "get_position", 0, nullptr);
        g_getRot = uo::u_method_typed(trClass, "get_rotation", 0, nullptr);
    }
    if (!g_getPos && trClass) g_getPos = uo::u_method(trClass, "get_position", 0);

    // 摇杆：Unity 的 Input Manager 轴。游戏的虚拟摇杆通常映射到这两个轴。
    // 2026-10-23 起只用来打诊断（灵魂出窍改自己读触摸了）。
    g_inputClass = uo::u_class("UnityEngine", "Input");
    if (g_inputClass) {
        g_getAxisRaw = uo::u_method_typed(g_inputClass, "GetAxisRaw", 1, "String");
        if (!g_getAxisRaw) g_getAxisRaw = uo::u_method(g_inputClass, "GetAxisRaw", 1);
    }

    g_setFov    = uo::u_method(g_camClass, "set_fieldOfView", 1);
    g_getDepth  = uo::u_method(g_camClass, "get_depth", 0);
    g_getEnabled = uo::u_method(g_camClass, "get_enabled", 0);

    CLOG("[CAM] find=%p getTr=%p setPos=%p setRot=%p getPos=%p axis=%p input=%p",
         (void*)g_findAll, (void*)g_getTr, (void*)g_setPos, (void*)g_setRot,
         (void*)g_getPos, (void*)g_getAxisRaw, (void*)g_inputClass);

    if (!g_findAll || !g_getTr || !g_setPos || !g_setRot) {
        CLOG("[CAM] 关键方法缺失，自由视角不可用");
        return false;
    }
    g_refsReady = true;
    return true;
}

// ---- 触摸状态（旧公转模式用）----
struct TouchSlot {
    bool  active = false;
    float startX = 0.0f;
    float startY = 0.0f;
    float lastX  = 0.0f;
    float lastY  = 0.0f;
    int   id     = -1;
    int   side   = 0;   // 0 = 左（平移） 1 = 右（旋转）
};

const int kMaxPointers = 8;
TouchSlot g_slots[kMaxPointers];

int find_free_slot() {
    for (int i = 0; i < kMaxPointers; i++) if (!g_slots[i].active) return i;
    return -1;
}

// 按真实 pointerId 线性找（以前直接拿 id 当下标，>6 的手指整体丢事件）
int find_slot_by_id(int id) {
    for (int i = 0; i < kMaxPointers; i++)
        if (g_slots[i].active && g_slots[i].id == id) return i;
    return -1;
}

const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
const float kPitchLimit = 85.0f * kDeg2Rad;   // 照 C# 的 ±85°

// 真实帧间隔（eglSwapBuffers 频率 = 屏幕刷新率；掉帧时 dt 会变大）
float frame_dt() {
    static long long lastNs = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long now = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    float dt = lastNs ? (float)((double)(now - lastNs) / 1e9) : (1.0f / 60.0f);
    lastNs = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.100f) dt = 0.100f;   // 卡顿/切后台回来别让相机瞬移几百米
    return dt;
}

// Unity 左手系，前进 +z。
//
// Quaternion.Euler(pitch, yaw, 0) 把 forward 转成：
//   forward = (sin y * cos p,  -sin p,  cos y * cos p)
// 注意 y 分量是 **-sin p** —— pitch 为正时相机会低头。
// （build-159 之前这里写成 +sin p，再加上四元数 y/z 分量搞反，
//   结果「手指往上划屏幕往下看、人还往上飞」，完全反人类）
void cam_basis(float yaw, float pitch, float fwd[3], float right[3]) {
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw),   sy = sinf(yaw);
    fwd[0] = sy * cp;  fwd[1] = -sp;  fwd[2] = cy * cp;
    right[0] = cy;     right[1] = 0.0f; right[2] = -sy;
}

// Quaternion.Euler(pitch, yaw, 0)（Unity 欧拉顺序 ZXY → R = Ry * Rx）
//
// qY * qX 展开式（y=sin(yaw/2) 记作 sy，p=sin(pitch/2) 记作 sp）：
//   x = cy*sp
//   y = cp*sy      ← 不是 sp*sy
//   z = -sp*sy     ← 不是 cp*sy
//   w = cp*cy
// build-159 把 y/z 分量写反了，旋转整个乱掉。
void euler_to_quat(float yaw, float pitch, float q[4]) {
    const float halfP = pitch * 0.5f;
    const float halfY = yaw * 0.5f;
    const float sp = sinf(halfP), cp = cosf(halfP);
    const float sy = sinf(halfY), cy = cosf(halfY);
    q[0] = cy * sp;
    q[1] = cp * sy;
    q[2] = -sp * sy;
    q[3] = cp * cy;
}

void normalize_yaw_pitch() {
    const float twoPi = 6.28318530717958647692f;
    while (g_camera.yaw >  twoPi) g_camera.yaw -= twoPi;
    while (g_camera.yaw < -twoPi) g_camera.yaw += twoPi;
    if (g_camera.pitch >  kPitchLimit) g_camera.pitch =  kPitchLimit;
    if (g_camera.pitch < -kPitchLimit) g_camera.pitch = -kPitchLimit;
}

// ---- 相机接管：写入 ----
// managed setter 优先：
//   目标是我们自己新建的相机，没人跟它抢，managed 写一定生效，
//   而且会走 Unity 的正确路径标 dirty / 更新 localToWorld。
//   直接怼 native Transform 的 TRS 字段有「值对了但画面不重算」的风险，
//   所以降级成兜底 + 诊断。
void write_cam_pos(void* tr, const float p[3]) {
    if (g_setPos) {
        float v[3] = { p[0], p[1], p[2] };
        void* a[1] = { v };
        if (uo::u_call_void(tr, g_setPos, a)) return;
    }
    if (!cam_native::set_world_pos(tr, p)) {
        static int fails = 0;
        if (fails < 5) { fails++; CLOG("[CAM] write pos 全路径失败"); }
    }
}

void write_cam_rot(void* tr, const float q[4]) {
    if (g_setRot) {
        float v[4] = { q[0], q[1], q[2], q[3] };
        void* a[1] = { v };
        if (uo::u_call_void(tr, g_setRot, a)) return;
    }
    if (!cam_native::set_world_rot(tr, q)) {
        static int fails = 0;
        if (fails < 5) { fails++; CLOG("[CAM] write rot 全路径失败"); }
    }
}

// 读当前相机的世界位置/朝向。
//
// managed 优先（build-155 的教训是反的：当时写的是游戏相机，native 直读才准）；
// 现在是自建相机，managed getter 走 Unity 正常路径，不会误读。
// native 直读留作兜底 —— 但它的偏移是启发性探测的，猜错就读到垃圾。
bool read_cam_pos(void* tr, float out[3]) {
    if (g_getPos && tr) {
        void* r = uo::u_call(tr, g_getPos, nullptr);
        if (r) {
            float v[3];
            memcpy(v, (char*)r + 16, sizeof(v));
            // 全 0 当失败（相机真在原点时退回到 native 也没损失）
            if (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f) {
                out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
                return true;
            }
        }
    }
    return cam_native::get_world_pos(tr, out);
}

bool read_cam_rot(void* tr, float out[4]) {
    if (g_getRot && tr) {
        void* r = uo::u_call(tr, g_getRot, nullptr);
        if (r) {
            float v[4];
            memcpy(v, (char*)r + 16, sizeof(v));
            const float n = v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3];
            if (n > 0.5f && n < 2.0f) {   // 单位四元数才算有效
                out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = v[3];
                return true;
            }
        }
    }
    return cam_native::get_world_rot(tr, out);
}

// 从四元数反解 yaw/pitch（接管时对齐朝向，别一开就跳视角）
void quat_to_yaw_pitch(const float q[4], float* yaw, float* pitch) {
    // forward = q * (0,0,1)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = 2.0f * (x * z + w * y);
    const float fy = 2.0f * (y * z - w * x);
    const float fz = 1.0f - 2.0f * (x * x + y * y);
    *yaw = atan2f(fx, fz);
    // forward.y = -sin(pitch) → pitch = -asin(forward.y)
    const float clamped = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    *pitch = -asinf(clamped);
}

void camera_rebuild();

}  // namespace

CameraCtrl g_camera;

void camera_set_follow(void* charTransform) {
    g_followTr = charTransform;
}

// =====================================================================
// 扫描
// =====================================================================
int CameraCtrl::scan() {
    if (!bind_refs()) return 0;

    // ---- 1) Camera.main 优先 ----
    // 直接按 depth 挑会挑错：手游的 UI 相机（Screen Space - Camera 的 Canvas）
    // depth 往往比主相机高，CopyFrom 它会把 cullingMask 抄成「只含 UI 层」，
    // 场景全空。Camera.main 找的是 tag = MainCamera 的那个，才是主相机。
    {
        void* mMain = uo::u_method(g_camClass, "get_main", 0);
        if (mMain) {
            void* m = uo::u_call(nullptr, mMain, nullptr);
            if (m && m != g_skipCam) {
                bool en = true;
                if (g_getEnabled) {
                    Il2CppObject* boxed = (Il2CppObject*)uo::u_call(m, g_getEnabled, nullptr);
                    if (boxed) en = (*(uint8_t*)((char*)boxed + 16)) != 0;
                }
                if (en) {
                    Il2CppObject* mtr = (Il2CppObject*)uo::u_call(m, g_getTr, nullptr);
                    if (mtr) {
                        camera = m;
                        transform = mtr;
                        scanned = true;
                        CLOG("[CAM] 主摄像机（Camera.main）cam=%p tr=%p", m, mtr);
                        return 1;
                    }
                }
            }
        }
        CLOG("[CAM] Camera.main 不可用，回退按 depth 挑");
    }

    Il2CppReflectionType* t = g_api.type_get_object(g_api.class_get_type((Il2CppClass*)g_camClass));
    if (!t) { CLOG("[CAM] type_get_object 失败"); return 0; }

    void* args[1] = { t };
    Il2CppObject* arr = (Il2CppObject*)uo::u_call(nullptr, g_findAll, args);
    if (!arr) { CLOG("[CAM] FindObjectsOfTypeAll 返回 null"); return 0; }

    size_t n = ((Il2CppArray*)arr)->max_length;
    CLOG("[CAM] 场景里有 %zu 个 Camera", n);

    float bestDepth = -1e9f;
    void* bestCam = nullptr;
    void* bestTr = nullptr;
    int valid = 0;

    for (size_t i = 0; i < n; i++) {
        void* cam = *(void**)uo::u_array_data_from(arr, i);
        if (!cam) continue;
        valid++;

        // 跳过我们自己的相机 —— 否则重建时会把「自己」当成游戏相机，
        // CopyFrom 自己 + depth 再 +1，无限套娃。
        if (cam == g_skipCam) continue;

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

        if (depth > bestDepth) {
            bestDepth = depth;
            bestCam = cam;
        }
    }

    if (!bestCam) {
        CLOG("[CAM] 没有 enabled 的摄像机（valid=%d）", valid);
        return 0;
    }

    Il2CppObject* tr = (Il2CppObject*)uo::u_call(bestCam, g_getTr, nullptr);
    if (!tr) { CLOG("[CAM] get_transform 失败"); return 0; }
    bestTr = tr;

    camera = bestCam;
    transform = bestTr;
    scanned = true;
    CLOG("[CAM] 主摄像机 cam=%p tr=%p depth=%.2f", camera, transform, bestDepth);
    return valid;
}

// =====================================================================
// 每帧更新
// =====================================================================
void CameraCtrl::update() {
    if (!enabled) return;
    if (!g_refsReady && !bind_refs()) return;

    // 相机被 Unity 回收了（切场景 / 游戏重建 Camera）→ 我们的指针就是野指针，
    // 再往下写就是踩内存。检测到就整体重建。
    if (!transform || !freecam::alive(transform)) {
        CLOG("[CAM] 我们的相机对象已失效（切场景？）→ 重建");
        camera_rebuild();
        return;
    }

    // 游戏原相机被销毁也要重建：我们的相机还在渲染，但位置/剔除参数
    // 全是旧场景的，画面会对着空气。
    {
        void* orig = freecam::gameCamera();
        if (orig && !freecam::alive(orig)) {
            CLOG("[CAM] 游戏原相机被销毁（切场景）→ 重建");
            camera_rebuild();
            return;
        }
    }

    const float dt = frame_dt();

    // ---- 复位请求（UI 线程发的，这里消费；Unity 调用必须在 UnityMain）----
    if (cmdReset) {
        cmdReset = false;
        float p[3] = {0.0f, 0.0f, 0.0f};
        bool got = false;
        if (g_followTr && freecam::alive(g_followTr)) {
            got = read_cam_pos(g_followTr, p);
            if (got) p[1] += 1.6f;   // 抬到角色胸口/头顶高度
        }
        if (!got) {
            got = read_cam_pos(transform, p);
            p[1] += 1.6f;
        }
        if (got) {
            camPos[0] = p[0]; camPos[1] = p[1]; camPos[2] = p[2];
            needSync = false;
            write_cam_pos(transform, camPos);
            CLOG("[CAM] reset -> (%.2f, %.2f, %.2f)", camPos[0], camPos[1], camPos[2]);
        } else {
            CLOG("[CAM] reset 失败：读不到参照位置");
        }
    }

    // ---- 灵魂出窍：本体不动，摇杆推着相机飞 ----
    if (freeFly) {
        // 首次启用：把 camPos 对齐到相机当前世界坐标，避免瞬移
        if (needSync) {
            float p[3] = {0.0f, 0.0f, 0.0f};
            if (read_cam_pos(transform, p)) {
                camPos[0] = p[0]; camPos[1] = p[1]; camPos[2] = p[2];
                needSync = false;
                syncFailed = false;
                CLOG("[CAM] freeFly sync pos=(%.2f, %.2f, %.2f)", camPos[0], camPos[1], camPos[2]);
            } else if (!syncFailed) {
                syncFailed = true;
                CLOG("[CAM] freeFly sync 失败（读不到相机位置）—— 本帧不动，等下次");
            }
        }

        const float h = moveInputX;
        const float v = moveInputY;   // camera_touch 里已经翻好符号：+1 = 向前

        float fwd[3], right[3];
        cam_basis(yaw, pitch, fwd, right);

        // 照 C#：所有方向先加完再归一化，斜向/上升时不会白嫖速度
        float dx = right[0] * h + fwd[0] * v;
        float dy = right[1] * h + fwd[1] * v;
        float dz = right[2] * h + fwd[2] * v;
        if (ascend)  dy += 1.0f;
        if (descend) dy -= 1.0f;

        const float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len > 1e-4f) {
            const float k = flySpeed * (boost ? boostMul : 1.0f) * dt / len;
            camPos[0] += dx * k;
            camPos[1] += dy * k;
            camPos[2] += dz * k;
            write_cam_pos(transform, camPos);
        }

        // 朝向：yaw/pitch（右半屏拖动改这两个值）
        float q[4];
        euler_to_quat(yaw, pitch, q);
        write_cam_rot(transform, q);

        // 每 60 帧打一次，实测时能直接看出「输入有没有进来、位置有没有变」
        static int flyBeat = 0;
        if (++flyBeat % 60 == 1) {
            CLOG("[CAM] fly h=%.2f v=%.2f up=%d dn=%d bst=%d yaw=%.1f pitch=%.1f pos=(%.1f, %.1f, %.1f)",
                 h, v, (int)ascend, (int)descend, (int)boost,
                 yaw / kDeg2Rad, pitch / kDeg2Rad,
                 camPos[0], camPos[1], camPos[2]);
        }
        return;
    }

    // ---- 旧公转模式（freeFly=false）----
    if (followTarget) {
        // 暂时保持手动设的值
    }

    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cyw = cosf(yaw),  syw = sinf(yaw);

    const float px = target[0] + dist * cp * syw;
    const float py = target[1] + dist * sp;
    const float pz = target[2] + dist * cp * cyw;

    const float pos[3] = { px, py, pz };
    write_cam_pos(transform, pos);

    float q[4];
    euler_to_quat(yaw, pitch, q);
    write_cam_rot(transform, q);
}

void CameraCtrl::reset() {
    // 灵魂出窍：只“回到角色身边”，朝向保持不动（复位就转头很烦）
    if (freeFly) {
        cmdReset = true;   // 真正的复位在 UnityMain 的 update 里做
        return;
    }
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
    normalize_yaw_pitch();
}

void CameraCtrl::zoom(float d) {
    dist += d;
    if (dist < 0.5f) dist = 0.5f;
    if (dist > 30.0f) dist = 30.0f;
}

void CameraCtrl::pan(float fwd, float right, float up) {
    const float cyw = cosf(yaw), syw = sinf(yaw);
    target[0] += fwd * syw + right * cyw;
    target[2] += fwd * cyw - right * syw;
    target[1] += up;
    if (target[1] < -5.0f) target[1] = -5.0f;
    if (target[1] > 300.0f) target[1] = 300.0f;
}

// =====================================================================
// 触摸
// =====================================================================
namespace {

// 灵魂出窍的触摸：左半屏虚拟摇杆 + 右半屏拖视角（照 C# ReadTouches）
bool freefly_touch(int action, int pointerId, float x, float y,
                   float viewW, float viewH) {
    CameraCtrl& c = g_camera;
    const float halfW = viewW * 0.5f;

    switch (action) {
        case 0: {   // DOWN
            if (x < halfW) {
                if (c.moveFinger >= 0) return true;   // 已经有移动指，吃掉但不接管
                c.moveFinger  = pointerId;
                c.moveOriginX = x;
                c.moveOriginY = y;
                c.moveInputX  = 0.0f;
                c.moveInputY  = 0.0f;
                return true;
            }
            // 右半屏整块都给视角（面板自己会先吃掉自己的矩形，不用我们让）
            if (c.lookFinger >= 0) return true;
            c.lookFinger = pointerId;
            c.lookLastX  = x;
            c.lookLastY  = y;
            return true;
        }

        case 2: {   // MOVE
            if (pointerId == c.moveFinger) {
                float dx = (x - c.moveOriginX) / (c.joyRadius > 1.0f ? c.joyRadius : 1.0f);
                float dy = (y - c.moveOriginY) / (c.joyRadius > 1.0f ? c.joyRadius : 1.0f);
                const float l = sqrtf(dx * dx + dy * dy);
                if (l > 1.0f) { dx /= l; dy /= l; }
                c.moveInputX = dx;
                c.moveInputY = -dy;   // 屏幕 y 向下增大；向上推 = 向前
                return true;
            }
            if (pointerId == c.lookFinger) {
                const float dx = x - c.lookLastX;
                const float dy = y - c.lookLastY;
                c.lookLastX = x;
                c.lookLastY = y;
                c.yaw   += dx * c.lookSens * kDeg2Rad;
                // 手指往上划（dy<0）→ pitch 变小 → 抬头（forward.y 变正）
                c.pitch += dy * c.lookSens * kDeg2Rad;
                normalize_yaw_pitch();
                return true;
            }
            return false;
        }

        case 1:     // UP
        case 3: {   // CANCEL
            if (pointerId == c.moveFinger) {
                c.moveFinger = -1;
                c.moveInputX = 0.0f;
                c.moveInputY = 0.0f;
                return true;
            }
            if (pointerId == c.lookFinger) {
                c.lookFinger = -1;
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

}  // namespace

bool camera_touch(int action, int pointerId, float x, float y,
                  float viewW, float viewH) {
    if (!g_camera.enabled) return false;

    if (g_camera.freeFly) return freefly_touch(action, pointerId, x, y, viewW, viewH);

    // ---- 旧公转模式：左半屏平移，右半屏旋转 ----
    const float halfW = viewW * 0.5f;
    const float kRotatePerPixel = 0.004f;   // 弧度/像素
    const float kPanPerPixel    = 0.010f;   // 世界单位/像素

    switch (action) {
        case 0: {  // DOWN
            const int slot = find_slot_by_id(pointerId);
            if (slot >= 0) return false;         // 这根手指已经在管了
            const int free = find_free_slot();
            if (free < 0) return false;
            TouchSlot& s = g_slots[free];
            s.active = true;
            s.id     = pointerId;
            s.side   = (x < halfW) ? 0 : 1;
            s.startX = s.lastX = x;
            s.startY = s.lastY = y;
            return true;
        }

        case 2: {  // MOVE
            const int slot = find_slot_by_id(pointerId);
            if (slot < 0) return false;
            TouchSlot& s = g_slots[slot];
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

        case 1:    // UP
        case 3: {  // CANCEL
            const int slot = find_slot_by_id(pointerId);
            if (slot < 0) return false;
            g_slots[slot].active = false;
            g_slots[slot].id = -1;
            return true;
        }

        default:
            return false;
    }
}

// =====================================================================
// 开关 / 重建
// =====================================================================
namespace {

// 游戏原相机（被 freecam 关掉的那个）。rebuild 后要跟着换，
// 否则下次关闭会把相机恢复到一个早就死了的指针上。
void* s_gameCam = nullptr;
void* s_gameTr  = nullptr;

int s_rebuildFails = 0;

void camera_rebuild() {
    CLOG("[CAM] rebuild: 重新扫描游戏相机");

    // 先把旧相机指针记下来让 scan 跳过，再 forget。
    // 顺序别反：forget 会 unpin，指针就没保障了。
    g_skipCam = freecam::camera();
    freecam::forget();          // 只清指针（gchandle / 成员），不碰可能已野掉的 Unity 对象
    g_camera.camera = nullptr;
    g_camera.transform = nullptr;
    g_camera.scanned = false;
    g_camera.needSync = true;
    g_camera.moveFinger = -1;
    g_camera.lookFinger = -1;

    if (g_camera.scan() <= 0) {
        if (++s_rebuildFails >= 3) {
            CLOG("[CAM] rebuild 连续失败 %d 次，关掉自由相机", s_rebuildFails);
            freecam::forget();
            g_camera.enabled = false;
        }
        return;
    }

    void* newTr = freecam::enable(g_camera.camera);
    if (!newTr) {
        if (++s_rebuildFails >= 3) {
            CLOG("[CAM] rebuild 连续失败 %d 次，关掉自由相机", s_rebuildFails);
            freecam::forget();
            g_camera.camera = nullptr;
            g_camera.transform = nullptr;
            g_camera.enabled = false;
        }
        return;
    }
    s_rebuildFails = 0;
    // 新 scan 到的就是新的游戏原相机（freecam::enable 已经把它关了）
    s_gameCam = g_camera.camera;
    s_gameTr  = g_camera.transform;
    g_camera.camera = freecam::camera();
    g_camera.transform = newTr;
    CLOG("[CAM] rebuild OK: cam=%p tr=%p depth=%d (gameCam=%p)",
         g_camera.camera, g_camera.transform, freecam::depth(), s_gameCam);
}

}  // namespace

namespace {
// UI 线程只写请求，真正建相机/销毁相机必须在 UnityMain 跑：
// AddComponent / Destroy 要建/析 native 对象，Java 主线程跑会静默失败
// （build-121 实测 Mesh..ctor 就这样，连 tombstone 都不留）。
int g_pendingEnable = -1;   // -1 = 没请求，0 = 关，1 = 开

void do_enable_internal(bool want) {

    if (want) {
        if (g_camera.enabled) return;

        // 记着的游戏相机如果已经被切场景带走了，重新扫。
        // （不判的话会对野指针 CopyFrom）
        if (s_gameCam && !freecam::alive(s_gameCam)) {
            CLOG("[CAM] 记着的游戏相机已失效，重新扫描");
            s_gameCam = nullptr;
            s_gameTr  = nullptr;
            g_camera.scanned = false;
        }

        if (!g_camera.scanned) {
            int n = g_camera.scan();
            if (n <= 0) {
                CLOG("[CAM] 开不起来：场景里没有可用摄像机");
                return;
            }
        }

        // ---- 新建一台相机接管画面（用户给的 C# 方案）----
        // 不跟游戏抢同一个 Transform：游戏爱怎么写它自己的相机就怎么写，
        // 反正它已经不渲染了。我们的相机没人碰，写多少就是多少。
        if (!s_gameCam) { s_gameCam = g_camera.camera; s_gameTr = g_camera.transform; }
        g_skipCam = s_gameCam;

        void* newTr = freecam::enable(s_gameCam);
        if (newTr) {
            g_camera.camera = freecam::camera();
            g_camera.transform = newTr;
            CLOG("[CAM] freecam active: cam=%p tr=%p depth=%d",
                 g_camera.camera, g_camera.transform, freecam::depth());
        } else {
            CLOG("[CAM] freecam 建相机失败，退回改原相机（会被游戏覆盖）");
            g_camera.camera = s_gameCam;
            g_camera.transform = s_gameTr;
        }

        // 相机换了：位置缓存重对齐，朝向按新相机当前朝向反解，避免一开就跳视角
        g_camera.needSync = true;
        g_camera.syncFailed = false;
        g_camera.moveFinger = -1;
        g_camera.lookFinger = -1;
        g_camera.moveInputX = g_camera.moveInputY = 0.0f;
        {
            float q[4] = {0, 0, 0, 1};
            if (read_cam_rot(g_camera.transform, q)) {
                quat_to_yaw_pitch(q, &g_camera.yaw, &g_camera.pitch);
                CLOG("[CAM] 朝向对齐 yaw=%.1f pitch=%.1f",
                     g_camera.yaw / kDeg2Rad, g_camera.pitch / kDeg2Rad);
            }
        }

        g_camera.enabled = true;
        CLOG("[CAM] 自由视角开");
        return;
    }

    // ---- 关 ----
    if (!g_camera.enabled) return;
    g_camera.enabled = false;
    freecam::disable();
    if (s_gameCam) { g_camera.camera = s_gameCam; g_camera.transform = s_gameTr; }
    g_camera.needSync = true;
    g_camera.moveInputX = g_camera.moveInputY = 0.0f;
    g_camera.moveFinger = -1;
    g_camera.lookFinger = -1;
    CLOG("[CAM] 自由视角关，原相机已恢复");
}

}  // namespace

void camera_set_enabled(int on) {
    g_pendingEnable = (on != 0) ? 1 : 0;
    CLOG("[CAM] 请求%s（等 UnityMain 执行）", on ? "开" : "关");
}

void camera_pump() {
    // build-161：自动接管已撤（用户要求不要默认开），只在面板里手动启。
    if (g_pendingEnable < 0) return;
    const bool want = (g_pendingEnable != 0);
    g_pendingEnable = -1;
    do_enable_internal(want);
}

int camera_is_enabled() {
    if (g_pendingEnable >= 0) return g_pendingEnable;   // UI 的 Toggle 要立即反映意图
    return g_camera.enabled ? 1 : 0;
}

void camera_set_motion(int ascend, int descend, int boost) {
    g_camera.ascend  = (ascend  != 0);
    g_camera.descend = (descend != 0);
    g_camera.boost   = (boost   != 0);
}

int camera_stick(float* out) {
    if (!g_camera.enabled || !g_camera.freeFly) return 0;
    // 只在真按着的时候才给，不然 UI 会画个残留的圆
    if (g_camera.moveFinger < 0) return 0;
    if (out) {
        out[0] = g_camera.moveOriginX;
        out[1] = g_camera.moveOriginY;
        out[2] = g_camera.moveInputX;
        out[3] = g_camera.moveInputY;
        out[4] = g_camera.joyRadius;
    }
    return 1;
}

void camera_set_config(float speed, float sens, float radius) {
    if (speed  > 0.0f)  { g_camera.flySpeed  = speed; }
    if (sens   > 0.0f)  { g_camera.lookSens  = sens; }
    if (radius > 0.0f)  { g_camera.joyRadius = radius; }
    // 夹一下，别让面板拖出离谱值
    if (g_camera.flySpeed  < 0.5f)   g_camera.flySpeed  = 0.5f;
    if (g_camera.flySpeed  > 50.0f)  g_camera.flySpeed  = 50.0f;
    if (g_camera.lookSens  < 0.01f)  g_camera.lookSens  = 0.01f;
    if (g_camera.lookSens  > 2.0f)   g_camera.lookSens  = 2.0f;
    if (g_camera.joyRadius < 20.0f)  g_camera.joyRadius = 20.0f;
    if (g_camera.joyRadius > 500.0f) g_camera.joyRadius = 500.0f;
}

}  // namespace game
