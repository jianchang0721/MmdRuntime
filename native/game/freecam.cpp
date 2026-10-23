// =====================================================================
// freecam.cpp —— 新建相机接管画面
// =====================================================================
#include "freecam.h"

#include <cstring>
#include <cstdio>

#include "logsink.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define FTAG "MmdFreeCam"
#define FLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {
namespace freecam {

namespace {

void* g_origCam = nullptr;      // 游戏原相机（enabled 被我们关了）
void* g_cam     = nullptr;      // 我们的相机
void* g_tr      = nullptr;      // 我们的相机 Transform
void* g_go      = nullptr;      // 我们的 GameObject
float g_depth   = 0.0f;

Il2CppClass* g_camCls = nullptr;

// 带类型筛选的方法查找（重载多的必须筛，别只按参数个数）
void* find_method_typed(void* klass, const char* name, int nparams, const char* p0) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m = nullptr;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != nparams) continue;
        if (!p0) return (void*)m;
        const Il2CppType* t = g_api.method_get_param(m, 0);
        Il2CppClass* k = t ? g_api.class_from_type(t) : nullptr;
        const char* cn = k ? g_api.class_get_name(k) : nullptr;
        if (cn && strcmp(cn, p0) == 0) return (void*)m;
    }
    return nullptr;
}

float cam_depth(void* cam) {
    static void* m = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; m = uo::u_method(g_camCls, "get_depth", 0); }
    if (!m || !cam) return 0.0f;
    void* r = uo::u_call(cam, m, nullptr);
    return r ? *(float*)((char*)r + 16) : 0.0f;
}

bool cam_set_enabled(void* cam, bool on) {
    static void* m = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; m = uo::u_method(g_camCls, "set_enabled", 1); }
    if (!m || !cam) return false;
    uint8_t v = on ? 1 : 0;
    void* a[1] = { &v };
    return uo::u_call_void(cam, m, a);
}

}  // namespace

bool  active() { return g_cam != nullptr; }
void* camera() { return g_cam; }
void* transform() { return g_tr; }
int   depth() { return (int)g_depth; }

void* enable(void* srcCam) {
    if (active()) return g_tr;
    if (!srcCam) { FLOG("[FREECAM] srcCam is null"); return nullptr; }

    // 依赖 Unity 对象层（幂等）
    if (!uo::ready() && !uo::init()) { FLOG("[FREECAM] uo::init failed"); return nullptr; }

    g_camCls = (Il2CppClass*)uo::u_class("UnityEngine", "Camera");
    Il2CppClass* goCls = (Il2CppClass*)uo::u_class("UnityEngine", "GameObject");
    if (!g_camCls || !goCls) { FLOG("[FREECAM] Camera/GameObject class missing"); return nullptr; }

    // ---- 1) new GameObject（parent=null，就是脱离层级）----
    void* tr = uo::create_node("ModelMod_FreeCamera", nullptr, 0.0f, 0.0f, 0.0f);
    if (!tr) { FLOG("[FREECAM] create_node failed"); return nullptr; }

    // GameObject 从 Transform 反查
    void* go = nullptr;
    {
        void* compCls = uo::u_class("UnityEngine", "Component");
        void* mGetGO = compCls ? uo::u_method(compCls, "get_gameObject", 0) : nullptr;
        go = mGetGO ? uo::u_call(tr, mGetGO, nullptr) : nullptr;
    }
    if (!go) { FLOG("[FREECAM] get_gameObject failed"); return nullptr; }
    g_go = go;

    // ---- 2) AddComponent(Camera) ----
    const MethodInfo* mAdd = (const MethodInfo*)find_method_typed(goCls, "AddComponent", 1, "Type");
    if (!mAdd) mAdd = (const MethodInfo*)uo::u_method(goCls, "AddComponent", 1);
    if (!mAdd) { FLOG("[FREECAM] AddComponent missing"); return nullptr; }

    Il2CppReflectionType* ty = g_api.type_get_object(g_api.class_get_type(g_camCls));
    if (!ty) { FLOG("[FREECAM] type_get_object failed"); return nullptr; }
    void* a1[1] = { ty };
    void* cam = uo::u_call(go, (void*)mAdd, a1);
    if (!cam) { FLOG("[FREECAM] AddComponent(Camera) returned null"); return nullptr; }

    // ---- 3) CopyFrom(原相机)：拷 FOV/clearFlags/cullingMask/投影等全部参数 ----
    // 这一步最关键：不拷 cullingMask 的话场景会空。
    {
        void* mCopy = find_method_typed(g_camCls, "CopyFrom", 1, "Camera");
        if (mCopy) {
            void* a[1] = { srcCam };
            if (uo::u_call_void(cam, mCopy, a)) FLOG("[FREECAM] CopyFrom OK");
            else FLOG("[FREECAM] CopyFrom 抛异常");
        } else {
            // 退化路径：至少把 near/far/fov/cullingMask 挨个拷过去
            FLOG("[FREECAM] CopyFrom 缺失，逐项拷关键参数");
            struct { const char* get; const char* set; } props[] = {
                {"get_fieldOfView","set_fieldOfView"},
                {"get_nearClipPlane","set_nearClipPlane"},
                {"get_farClipPlane","set_farClipPlane"},
                {"get_cullingMask","set_cullingMask"},
                {"get_depth","set_depth"},
                {"get_orthographic","set_orthographic"},
                {"get_orthographicSize","set_orthographicSize"},
            };
            for (auto& p : props) {
                void* g = uo::u_method(g_camCls, p.get, 0);
                void* s = uo::u_method(g_camCls, p.set, 1);
                if (!g || !s) continue;
                void* v = uo::u_call(srcCam, g, nullptr);
                if (!v) continue;
                // 值类型：把 +0x10 起的 4 字节原样搬过去
                // (float / int / bool 都是 4 字节或更小，这里按 4 字节拷足够)
                uint32_t raw = *(uint32_t*)((char*)v + 16);
                void* a[1] = { &raw };
                uo::u_call_void(cam, s, a);
            }
        }
    }

    // ---- 4) 摆到原相机的位置/朝向 ----
    {
        void* trCls = uo::u_class("UnityEngine", "Transform");
        void* srcTr = nullptr;
        {
            void* compCls = uo::u_class("UnityEngine", "Component");
            void* mGetTr = compCls ? uo::u_method(compCls, "get_transform", 0) : nullptr;
            srcTr = mGetTr ? uo::u_call(srcCam, mGetTr, nullptr) : nullptr;
        }
        void* mGetPos = trCls ? uo::u_method(trCls, "get_position", 0) : nullptr;
        void* mSetPos = trCls ? uo::u_method(trCls, "set_position", 1) : nullptr;
        void* mGetRot = trCls ? uo::u_method(trCls, "get_rotation", 0) : nullptr;
        void* mSetRot = trCls ? uo::u_method(trCls, "set_rotation", 1) : nullptr;
        if (srcTr && mGetPos && mSetPos) {
            void* p = uo::u_call(srcTr, mGetPos, nullptr);
            if (p) {
                float v[3];
                memcpy(v, (char*)p + 16, sizeof(v));
                void* a[1] = { v };
                uo::u_call_void(tr, mSetPos, a);
                FLOG("[FREECAM] pos <- (%.2f, %.2f, %.2f)", v[0], v[1], v[2]);
            }
        }
        if (srcTr && mGetRot && mSetRot) {
            void* q = uo::u_call(srcTr, mGetRot, nullptr);
            if (q) {
                float v[4];
                memcpy(v, (char*)q + 16, sizeof(v));
                void* a[1] = { v };
                uo::u_call_void(tr, mSetRot, a);
            }
        }
    }

    // ---- 5) depth = 原相机 + 1，排在它后面渲染 ----
    g_depth = cam_depth(srcCam) + 1.0f;
    {
        void* mSetDepth = uo::u_method(g_camCls, "set_depth", 1);
        if (mSetDepth) {
            void* a[1] = { &g_depth };
            uo::u_call_void(cam, mSetDepth, a);
        }
    }

    // ---- 6) 打开我们的相机，关掉游戏相机 ----
    cam_set_enabled(cam, true);
    cam_set_enabled(srcCam, false);

    g_origCam = srcCam;
    g_cam = cam;
    g_tr = tr;

    FLOG("[FREECAM] enabled: go=%p cam=%p tr=%p depth=%.2f (orig disabled, depth %.2f)",
         go, cam, tr, g_depth, g_depth - 1.0f);
    return tr;
}

void disable() {
    if (!g_cam && !g_origCam) return;

    if (g_cam) cam_set_enabled(g_cam, false);
    if (g_origCam) cam_set_enabled(g_origCam, true);

    // 销毁 GameObject（Destroy 会连带 Camera 组件）
    if (g_go) {
        Il2CppClass* objCls = (Il2CppClass*)uo::u_class("UnityEngine", "Object");
        void* mDestroy = objCls ? uo::u_method(objCls, "Destroy", 1) : nullptr;
        if (mDestroy) {
            void* a[1] = { g_go };
            uo::u_call_void(nullptr, mDestroy, a);
            FLOG("[FREECAM] destroy GameObject %p", g_go);
        } else {
            FLOG("[FREECAM] Object.Destroy 缺失，只能禁用相机");
        }
    }

    FLOG("[FREECAM] disabled, original camera restored");
    g_origCam = nullptr;
    g_cam = nullptr;
    g_tr = nullptr;
    g_go = nullptr;
    g_depth = 0.0f;
}

}  // namespace freecam
}  // namespace game
