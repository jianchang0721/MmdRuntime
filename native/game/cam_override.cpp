// =====================================================================
// cam_override.cpp —— 被动捕获相机 + hook 写入侧
// =====================================================================
#include "cam_override.h"

#include <cmath>
#include <cstring>
#include <cstdint>

#include "logsink.h"
#include "dobby/dobby.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define CTAG "MmdCamOv"
#define CLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {
namespace cam_override {

namespace {

void*  g_cam = nullptr;      // UnityEngine.Camera（hook get_main 抓到）
void*  g_camTr = nullptr;    // 它的 Transform（hook get_transform 抓到）
bool   g_active = false;     // 玩家开了自由相机才改参数
bool   g_installed = false;

// 我们想要的 pose
float  g_pos[3] = {0, 0, 0};
float  g_quat[4] = {0, 0, 0, 1};
bool   g_haveDesired = false;

int    g_hookCount = 0;      // 成功装了几个

// ---- 原函数指针 ----
typedef void* (*FnGetMain)(void* mtd);
typedef void* (*FnCompGetTr)(void* self, void* mtd);
typedef void  (*FnSetPosInj)(void* self, float* p);
typedef void  (*FnSetRotInj)(void* self, float* q);
typedef void  (*FnSetLocalPosInj)(void* self, float* p);
typedef void  (*FnSetPosRotInj)(void* self, float* p, float* q);
typedef void  (*FnSetLocalRotInj)(void* self, float* q);

FnGetMain        o_getMain = nullptr;
FnCompGetTr      o_compGetTr = nullptr;
FnSetPosInj      o_setPosInj = nullptr;
FnSetRotInj      o_setRotInj = nullptr;
FnSetLocalPosInj o_setLocalPosInj = nullptr;
FnSetPosRotInj   o_setPosRotInj = nullptr;
FnSetLocalRotInj o_setLocalRotInj = nullptr;

inline bool finite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline bool finite4(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) &&
           std::isfinite(v[2]) && std::isfinite(v[3]);
}

// 这个 self 是不是我们捕获到的相机 Transform？
inline bool is_cam_transform(void* self) {
    return g_camTr && self == g_camTr;
}

// 改参数：位置/旋转覆盖成我们想要的
inline void override_if_cam(void* self, float* p, float* q) {
    if (!g_active || !g_haveDesired) return;
    if (!is_cam_transform(self)) return;
    if (p && finite3(g_pos)) { p[0] = g_pos[0]; p[1] = g_pos[1]; p[2] = g_pos[2]; }
    if (q && finite4(g_quat)) { q[0] = g_quat[0]; q[1] = g_quat[1]; q[2] = g_quat[2]; q[3] = g_quat[3]; }
}

// ---- hook 实现 ----
void* h_getMain(void* mtd) {
    void* r = o_getMain ? o_getMain(mtd) : nullptr;
    if (r && r != g_cam) {
        g_cam = r;
        g_camTr = nullptr;      // 相机换了，Transform 重新捕获
        CLOG("[CAMOV] main camera -> %p", r);
    }
    return r;
}

void* h_compGetTr(void* self, void* mtd) {
    void* r = o_compGetTr ? o_compGetTr(self, mtd) : nullptr;
    if (r && g_cam && self == g_cam && !g_camTr) {
        g_camTr = r;
        CLOG("[CAMOV] camera transform -> %p", r);
    }
    return r;
}

void h_setPosInj(void* self, float* p) {
    override_if_cam(self, p, nullptr);
    if (o_setPosInj) o_setPosInj(self, p);
}

void h_setRotInj(void* self, float* q) {
    override_if_cam(self, nullptr, q);
    if (o_setRotInj) o_setRotInj(self, q);
}

void h_setLocalPosInj(void* self, float* p) {
    override_if_cam(self, p, nullptr);
    if (o_setLocalPosInj) o_setLocalPosInj(self, p);
}

void h_setLocalRotInj(void* self, float* q) {
    override_if_cam(self, nullptr, q);
    if (o_setLocalRotInj) o_setLocalRotInj(self, q);
}

void h_setPosRotInj(void* self, float* p, float* q) {
    override_if_cam(self, p, q);
    if (o_setPosRotInj) o_setPosRotInj(self, p, q);
}

// ---- 方法定位 ----
// *_Injected 是 il2cpp icall，MethodInfo->methodPointer 就是 native 入口。
void* method_ptr(void* klass, const char* name, int argc) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m = nullptr;
    void* found = nullptr;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != argc) continue;
        found = *(void**)m;      // MethodInfo 第一个字段是 methodPointer
        break;
    }
    return found;
}

void* method_ptr_typed(void* klass, const char* name, int argc, const char* p0) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m = nullptr;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != argc) continue;
        const Il2CppType* t = g_api.method_get_param(m, 0);
        Il2CppClass* k = t ? g_api.class_from_type(t) : nullptr;
        const char* cn = k ? g_api.class_get_name(k) : nullptr;
        if (cn && strcmp(cn, p0) == 0) return *(void**)m;
    }
    return nullptr;
}

}  // namespace

bool captured() { return g_camTr != nullptr; }
void* camera() { return g_cam; }
void* transform() { return g_camTr; }

void set_desired_pose(const float pos[3], const float quat[4]) {
    if (pos) { memcpy(g_pos, pos, sizeof(g_pos)); }
    if (quat) { memcpy(g_quat, quat, sizeof(g_quat)); }
    g_haveDesired = true;
}

bool install() {
    if (g_installed) return true;
    if (!uo::ready() && !uo::init()) { CLOG("[CAMOV] uo::init 失败"); return false; }

    Il2CppClass* camCls   = (Il2CppClass*)uo::u_class("UnityEngine", "Camera");
    Il2CppClass* compCls  = (Il2CppClass*)uo::u_class("UnityEngine", "Component");
    Il2CppClass* trCls    = (Il2CppClass*)uo::u_class("UnityEngine", "Transform");
    if (!camCls || !compCls || !trCls) { CLOG("[CAMOV] class missing"); return false; }

    // ① 被动捕获：hook get_main / get_transform 的 methodPointer
    void* pGetMain = method_ptr(camCls, "get_main", 0);
    void* pCompTr  = method_ptr(compCls, "get_transform", 0);

        // ② 写入侧：*_Injected
    // ⚠️ 实测（build-155）：这些 icall 钩子装了但一次都不命中——Unity 内部
    // C++ 调 Transform 走原生直路，不经过 managed icall 层。留着做兜底，
    // 真正的接管在 cam_write_hook.cpp（hook libunity 原生 Transform 函数）。
    void* pSetPos    = method_ptr(trCls, "set_position_Injected", 1);
    void* pSetRot    = method_ptr(trCls, "set_rotation_Injected", 1);
    void* pSetLPos   = method_ptr(trCls, "set_localPosition_Injected", 1);
    void* pSetLRot   = method_ptr(trCls, "set_localRotation_Injected", 1);
    void* pSetPosRot = method_ptr(trCls, "SetPositionAndRotation_Injected", 2);

    CLOG("[CAMOV] targets: main=%p cTr=%p pos=%p rot=%p lpos=%p lrot=%p posrot=%p",
         pGetMain, pCompTr, pSetPos, pSetRot, pSetLPos, pSetLRot, pSetPosRot);

    auto hook = [&](void* target, void* replace, void** orig) -> bool {
        if (!target) return false;
        if (DobbyHook(target, replace, orig) != 0 || !*orig) {
            CLOG("[CAMOV] DobbyHook %p 失败", target);
            return false;
        }
        g_hookCount++;
        return true;
    };

    // 注意：set_position_Injected 的真实签名是 (ref Vector3)，
    // 但 il2cpp icall 里 Vector3& 就是一个 float*，直接改内容即可。
    if (!hook(pGetMain, (void*)h_getMain, (void**)&o_getMain)) return false;
    if (!hook(pCompTr, (void*)h_compGetTr, (void**)&o_compGetTr)) return false;
    if (pSetPos)    hook(pSetPos, (void*)h_setPosInj, (void**)&o_setPosInj);
    if (pSetRot)    hook(pSetRot, (void*)h_setRotInj, (void**)&o_setRotInj);
    if (pSetLPos)   hook(pSetLPos, (void*)h_setLocalPosInj, (void**)&o_setLocalPosInj);
    if (pSetLRot)   hook(pSetLRot, (void*)h_setLocalRotInj, (void**)&o_setLocalRotInj);
    if (pSetPosRot) hook(pSetPosRot, (void*)h_setPosRotInj, (void**)&o_setPosRotInj);

    g_installed = true;
    CLOG("[CAMOV] installed, %d hooks", g_hookCount);
    return true;
}

}  // namespace cam_override
}  // namespace game
