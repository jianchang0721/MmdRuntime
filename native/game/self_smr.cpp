// =====================================================================
// self_smr.cpp —— 自建 SkinnedMeshRenderer（C 方案）
// =====================================================================
#include "self_smr.h"

#include <cstdlib>
#include <cstring>
#include <cmath>

#include "logsink.h"
#include "bone_hook.h"
#include "smr_bind.h"
#include "smr_layout.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define GTAG "MmdSelfSmr"
#define GLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {

// 全局保存自建 SMR，给 tick 探针用
static void* g_selfSmrManaged = nullptr;
static void* g_selfSmrNative  = nullptr;

namespace {

// 按「名字 + 参数个数 + 第一个参数类名」筛重载（AddComponent / set_sharedMaterials 都有多个）
const MethodInfo* find_method_typed(void* klass, const char* name, int nparams, const char* p0) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != nparams) continue;
        if (!p0) return m;
        const Il2CppType* t = g_api.method_get_param(m, 0);
        Il2CppClass* k = t ? g_api.class_from_type(t) : nullptr;
        const char* cn = k ? g_api.class_get_name(k) : nullptr;
        if (cn && strcmp(cn, p0) == 0) return m;
    }
    return nullptr;
}

// managed Object → native 指针（m_CachedPtr 在 +16）
void* obj_native(void* o) {
    return o ? *(void**)((char*)o + 16) : nullptr;
}

}  // namespace

// =====================================================================
// 可见性诊断：managed 状态 + native 字段对比
// =====================================================================
static void dump_renderer_state(const char* tag, void* renderer) {
    if (!renderer) { GLOG("[VIS] %s renderer null", tag); return; }
    void* rCls = uo::u_class("UnityEngine", "Renderer");
    if (!rCls) { GLOG("[VIS] Renderer class missing"); return; }
    void* mEnabled = uo::u_method(rCls, "get_enabled", 0);
    void* mVisible = uo::u_method(rCls, "get_isVisible", 0);
    void* mBounds  = uo::u_method(rCls, "get_bounds", 0);

    int en = -1, vis = -1;
    if (mEnabled) {
        void* r = uo::u_call(renderer, mEnabled, nullptr);
        if (r) en = *(uint8_t*)((char*)r + 16) ? 1 : 0;
    }
    if (mVisible) {
        void* r = uo::u_call(renderer, mVisible, nullptr);
        if (r) vis = *(uint8_t*)((char*)r + 16) ? 1 : 0;
    }
    GLOG("[VIS] %s enabled=%d isVisible=%d", tag, en, vis);

    if (mBounds) {
        void* r = uo::u_call(renderer, mBounds, nullptr);
        if (r) {
            float* b = (float*)((char*)r + 16);   // Bounds: center(3f) + extents(3f)
            GLOG("[VIS] %s bounds c=(%.3f, %.3f, %.3f) e=(%.3f, %.3f, %.3f)",
                 tag, b[0], b[1], b[2], b[3], b[4], b[5]);
        }
    }
}

// 诊断：把模型根的世界坐标设到相机正前方 3 米。
// cube 可见/模型可见的二分都靠这个——模型出现在眼前就说明渲染链路通，
// 问题只在挂载位置/缩放；还是空白就是 SMR 数据（m_Skin 之类）。
static void place_model_in_front(void* modelRoot) {
    if (!modelRoot) return;
    void* camCls  = uo::u_class("UnityEngine", "Camera");
    void* trCls   = uo::u_class("UnityEngine", "Transform");
    void* compCls = uo::u_class("UnityEngine", "Component");
    if (!camCls || !trCls) return;

    void* mMain  = uo::u_method(camCls, "get_main", 0);
    void* mGetTr = compCls ? uo::u_method(compCls, "get_transform", 0) : nullptr;
    void* mGetPos = uo::u_method(trCls, "get_position", 0);
    void* mGetFwd = uo::u_method(trCls, "get_forward", 0);
    void* mSetPos = uo::u_method_typed(trCls, "set_position", 1, "Vector3");
    if (!mMain || !mGetTr || !mGetPos || !mGetFwd || !mSetPos) {
        GLOG("[VIS] move-to-front: 方法缺失 main=%p getTr=%p getPos=%p getFwd=%p setPos=%p",
             (void*)mMain, (void*)mGetTr, (void*)mGetPos, (void*)mGetFwd, (void*)mSetPos);
        return;
    }

    void* cam = uo::u_call(nullptr, mMain, nullptr);
    if (!cam) { GLOG("[VIS] move-to-front: main camera null"); return; }
    void* camTr = uo::u_call(cam, mGetTr, nullptr);
    void* rp = camTr ? uo::u_call(camTr, mGetPos, nullptr) : nullptr;
    void* rf = camTr ? uo::u_call(camTr, mGetFwd, nullptr) : nullptr;
    if (!rp || !rf) { GLOG("[VIS] move-to-front: pos/fwd null"); return; }

    float* p = (float*)((char*)rp + 16);
    float* f = (float*)((char*)rf + 16);
    const float kDist = 3.0f;
    struct { float x, y, z; } np = { p[0] + f[0]*kDist, p[1] + f[1]*kDist, p[2] + f[2]*kDist };
    void* a[1] = { &np };
    uo::u_call_void(modelRoot, mSetPos, a);
    GLOG("[VIS] model -> camera front: (%.2f, %.2f, %.2f)  cam=(%.2f, %.2f, %.2f) fwd=(%.2f, %.2f, %.2f)",
         np.x, np.y, np.z, p[0], p[1], p[2], f[0], f[1], f[2]);
}

static void dump_visibility(SelfSmr& s, void* sourceSmr, void* modelRoot) {
    GLOG("[VIS] ===== 可见性诊断 =====");
    place_model_in_front(modelRoot);
    dump_renderer_state("self", s.smr);
    dump_renderer_state("ref ", sourceSmr);

    // ===== 场景排查：activeInHierarchy / 父链 / 所有相机 cullingMask =====
    {
        void* goCls2 = uo::u_class("UnityEngine", "GameObject");
        void* objCls2 = uo::u_class("UnityEngine", "Object");
        void* compCls2 = uo::u_class("UnityEngine", "Component");
        void* trCls2 = uo::u_class("UnityEngine", "Transform");
        void* mGetAH  = goCls2 ? uo::u_method(goCls2, "get_activeInHierarchy", 0) : nullptr;
        void* mGetAS  = goCls2 ? uo::u_method(goCls2, "get_activeSelf", 0) : nullptr;
        void* mGetName = objCls2 ? uo::u_method(objCls2, "get_name", 0) : nullptr;
        void* mGetGO   = compCls2 ? uo::u_method(compCls2, "get_gameObject", 0) : nullptr;
        void* mGetParent = trCls2 ? uo::u_method(trCls2, "get_parent", 0) : nullptr;
        void* mGetLayer  = goCls2 ? uo::u_method(goCls2, "get_layer", 0) : nullptr;
        void* mGetTr2    = compCls2 ? uo::u_method(compCls2, "get_transform", 0) : nullptr;

        if (s.go && mGetAH) {
            void* r = uo::u_call(s.go, mGetAH, nullptr);
            int ah = r ? (*(uint8_t*)((char*)r + 16) ? 1 : 0) : -1;
            void* r2 = mGetAS ? uo::u_call(s.go, mGetAS, nullptr) : nullptr;
            int as = r2 ? (*(uint8_t*)((char*)r2 + 16) ? 1 : 0) : -1;
            GLOG("[SCENE] modelGO activeInHierarchy=%d activeSelf=%d", ah, as);
        }

        void* cur = modelRoot;
        for (int depth = 0; depth < 4 && cur; depth++) {
            void* go = mGetGO ? uo::u_call(cur, mGetGO, nullptr) : nullptr;
            const char* nm = (go && mGetName) ? (const char*)uo::u_call(go, mGetName, nullptr) : nullptr;
            int ah = -1, lay = -1;
            if (go && mGetAH) {
                void* r = uo::u_call(go, mGetAH, nullptr);
                if (r) ah = *(uint8_t*)((char*)r + 16) ? 1 : 0;
            }
            if (go && mGetLayer) {
                void* r = uo::u_call(go, mGetLayer, nullptr);
                if (r) lay = *(int32_t*)((char*)r + 16);
            }
            GLOG("[SCENE] parent[%d] name='%s' activeInHierarchy=%d layer=%d",
                 depth, nm ? nm : "?", ah, lay);
            cur = mGetParent ? uo::u_call(cur, mGetParent, nullptr) : nullptr;
        }

        void* camCls2 = uo::u_class("UnityEngine", "Camera");
        void* mAll  = camCls2 ? uo::u_method(camCls2, "get_allCameras", 0) : nullptr;
        void* mMask = camCls2 ? uo::u_method(camCls2, "get_cullingMask", 0) : nullptr;
        void* mPos  = trCls2 ? uo::u_method(trCls2, "get_position", 0) : nullptr;
        if (mAll) {
            void* arr = uo::u_call(nullptr, mAll, nullptr);
            if (arr) {
                int32_t n = *(int32_t*)((char*)arr + 24);
                GLOG("[SCENE] allCameras n=%d (只列前 8 个)", n);
                for (int i = 0; i < n && i < 8; i++) {
                    void* cam = *(void**)((char*)arr + 32 + (size_t)i * 8);
                    if (!cam) continue;
                    const char* nm = mGetName ? (const char*)uo::u_call(cam, mGetName, nullptr) : "?";
                    int mask = -1;
                    if (mMask) {
                        void* r = uo::u_call(cam, mMask, nullptr);
                        if (r) mask = *(int32_t*)((char*)r + 16);
                    }
                    float px = 0, py = 0, pz = 0;
                    void* tr = mGetTr2 ? uo::u_call(cam, mGetTr2, nullptr) : nullptr;
                    if (tr && mPos) {
                        void* r = uo::u_call(tr, mPos, nullptr);
                        if (r) { float* q = (float*)((char*)r + 16); px = q[0]; py = q[1]; pz = q[2]; }
                    }
                    GLOG("[SCENE] cam[%d] '%s' pos=(%.1f,%.1f,%.1f) mask=0x%08X layer10=%d",
                         i, nm ? nm : "?", px, py, pz, mask,
                         (mask >= 0 && (mask & (1 << 10))) ? 1 : 0);
                }
            }
        }
    }

    void* refNative = sourceSmr ? *(void**)((char*)sourceSmr + 16) : nullptr;

    // skin probe：+0x1d8 是全表唯一像对象指针的 ref-only 槽，怀疑是 m_Skin。
    // 只读，不写。
    if (refNative) {
        uint64_t* rslot = (uint64_t*)((char*)refNative + 0x1d8);
        uint64_t  sslot = s.native ? *(uint64_t*)((char*)s.native + 0x1d8) : 0;
        GLOG("[VIS] +0x1d8 self=0x%llx ref=0x%llx", (unsigned long long)sslot,
             (unsigned long long)*rslot);
        void* skin = (void*)(*rslot);
        if (skin && ((uintptr_t)skin > 0x100000000ULL)) {
            uint32_t* w = (uint32_t*)skin;
            for (int off = 0; off < 0x100; off += 32) {
                GLOG("[VIS] skin+0x%02x: %08x %08x %08x %08x %08x %08x %08x %08x",
                     off, w[off/4+0], w[off/4+1], w[off/4+2], w[off/4+3],
                     w[off/4+4], w[off/4+5], w[off/4+6], w[off/4+7]);
            }
        }
    }

    if (s.native && refNative) {
        GLOG("[VIS] native self=%p ref=%p (只列 ref 有 self 为 0 的槽)",
             s.native, refNative);
        int printed = 0;
        for (int off = 0; off < 0x300; off += 4) {
            uint32_t sv = *(uint32_t*)((char*)s.native + off);
            uint32_t rv = *(uint32_t*)((char*)refNative + off);
            if (rv != 0 && sv == 0) {
                GLOG("[VIS]   ref-only @+0x%x = 0x%08x", off, rv);
                if (++printed >= 40) { GLOG("[VIS]   ...(截断)"); break; }
            }
        }
        if (printed == 0) GLOG("[VIS]   ref-only 槽为 0");
    }

    void* camCls  = uo::u_class("UnityEngine", "Camera");
    void* trCls   = uo::u_class("UnityEngine", "Transform");
    void* compCls = uo::u_class("UnityEngine", "Component");
    void* mCamMain = camCls ? uo::u_method(camCls, "get_main", 0) : nullptr;
    void* mGetPos  = trCls ? uo::u_method(trCls, "get_position", 0) : nullptr;
    void* mGetTr   = compCls ? uo::u_method(compCls, "get_transform", 0) : nullptr;

    void* cam = mCamMain ? uo::u_call(nullptr, mCamMain, nullptr) : nullptr;
    if (cam && mGetTr && mGetPos) {
        void* camTr = uo::u_call(cam, mGetTr, nullptr);
        void* r1 = camTr ? uo::u_call(camTr, mGetPos, nullptr) : nullptr;
        void* r2 = modelRoot ? uo::u_call(modelRoot, mGetPos, nullptr) : nullptr;
        if (r1 && r2) {
            float* c = (float*)((char*)r1 + 16);
            float* m = (float*)((char*)r2 + 16);
            float dx = m[0]-c[0], dy = m[1]-c[1], dz = m[2]-c[2];
            GLOG("[VIS] cam=(%.2f, %.2f, %.2f) model=(%.2f, %.2f, %.2f) dist=%.2f",
                 c[0], c[1], c[2], m[0], m[1], m[2], sqrtf(dx*dx+dy*dy+dz*dz));
        }
    }
}

bool create_self_smr(void* modelTransform,
                     void* sourceSmr,
                     void* meshObj,
                     const int32_t* boneIds, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out) {
    out = SelfSmr{};
    if (!modelTransform || !meshObj || !boneIds || boneCount <= 0) {
        GLOG("[SELF] bad args tr=%p mesh=%p ptrs=%p n=%d",
             modelTransform, meshObj, (void*)boneIds, boneCount);
        return false;
    }

    Il2CppClass* smrCls = (Il2CppClass*)uo::u_class("UnityEngine", "SkinnedMeshRenderer");
    if (!smrCls) { GLOG("[SELF] SMR class missing"); return false; }

    // ---- 1) 用已有的模型根节点（已挂在角色下、已缩放）----
    void* tr = modelTransform;
    out.tr = tr;

    Il2CppClass* trCls = (Il2CppClass*)uo::u_class("UnityEngine", "Transform");
    const MethodInfo* mGetGO = (const MethodInfo*)uo::u_method(trCls, "get_gameObject", 0);
    void* go = mGetGO ? uo::u_call(tr, (void*)mGetGO, nullptr) : nullptr;
    if (!go) { GLOG("[SELF] get_gameObject failed"); return false; }
    out.go = go;
    GLOG("[SELF] GameObject=%p Transform=%p", go, tr);

    // ---- 拷贝 layer（看不见模型的头号真因）----
    // ~/ModelMod_repo 里踩过：不设 layer 的话相机 cullingMask 里没 Default 层，
    // 模型画了但不渲染。直接抬用目标角色的 layer 最稳。
    {
        Il2CppClass* compCls = (Il2CppClass*)uo::u_class("UnityEngine", "Component");
        Il2CppClass* goClsL  = (Il2CppClass*)uo::u_class("UnityEngine", "GameObject");
        int layer = 0;
        if (sourceSmr && compCls && goClsL) {
            const MethodInfo* mGetGO = (const MethodInfo*)uo::u_method(compCls, "get_gameObject", 0);
            void* sgo = mGetGO ? uo::u_call(sourceSmr, (void*)mGetGO, nullptr) : nullptr;
            const MethodInfo* mGetLayer = (const MethodInfo*)uo::u_method(goClsL, "get_layer", 0);
            if (sgo && mGetLayer) {
                void* lr = uo::u_call(sgo, (void*)mGetLayer, nullptr);
                if (lr) layer = *(int32_t*)((char*)lr + 16);
            }
        }
        const MethodInfo* mSetLayer = (const MethodInfo*)uo::u_method(goClsL, "set_layer", 1);
        if (mSetLayer) {
            void* a[1] = { &layer };
            uo::u_call_void(go, (void*)mSetLayer, a);
        }
        GLOG("[SELF] layer copied: %d (setLayer=%p)%s", layer, (void*)mSetLayer,
             layer == 0 ? "  WARNING: layer=0，可能被相机 cullingMask 剔除" : "");
    }

    // ---- 2) AddComponent(SkinnedMeshRenderer 的 Type) ----
    Il2CppClass* goCls = (Il2CppClass*)uo::u_class("UnityEngine", "GameObject");
    const MethodInfo* mAdd = find_method_typed(goCls, "AddComponent", 1, "Type");
    if (!mAdd) mAdd = (const MethodInfo*)uo::u_method(goCls, "AddComponent", 1);
    if (!mAdd) { GLOG("[SELF] AddComponent missing"); return false; }

    Il2CppReflectionType* ty = g_api.type_get_object(g_api.class_get_type(smrCls));
    if (!ty) { GLOG("[SELF] type_get_object failed"); return false; }
    void* a1[1] = { ty };
    void* comp = uo::u_call(go, (void*)mAdd, a1);
    if (!comp) { GLOG("[SELF] AddComponent(SMR) returned null"); return false; }

    out.smr = comp;
    out.native = obj_native(comp);
    if (!out.native) { GLOG("[SELF] SMR native is null"); return false; }
    GLOG("[SELF] SMR=%p native=%p", comp, out.native);

    // ---- 3) 写 native m_Bones（8 字节/元素的 Transform* 指针数组）----
    uint8_t* base = (uint8_t*)out.native;
    const auto& L = layout();

    // 对照：游戏原 SMR 的 m_Bones.data 前 8 个 4 字节。
    // 小整数 -> 4 字节 instanceID 数组；0x7xxxxxxxxx 堆指针 -> 8 字节指针数组。
    if (sourceSmr) {
        uint8_t* sb = (uint8_t*)obj_native(sourceSmr);
        if (sb) {
            void* sd = *(void**)(sb + L.offBoneData);
            int32_t ss = *(int32_t*)(sb + L.offBoneSize);
            GLOG("[SELF] ref bones data=%p size=%d", sd, ss);
            if (sd && ss >= 2) {
                uint32_t* w = (uint32_t*)sd;
                GLOG("[SELF]   [%08x %08x %08x %08x] [%08x %08x %08x %08x]",
                     w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }

    // build-103 实测 dump：游戏原 SMR 的 m_Bones.data 前 8 个 4 字节全是
    // 0x0004xxxx 这类小整数 -> 它就是 4 字节 instanceID 数组。
    // 8 字节指针写法（build-103）崩在同一地址，说明元素宽度不是根因。
    // 所以这里按 4 字节 id 写，id 从 managed Transform 的 GetInstanceID 拿。
    int32_t* newBones = (int32_t*)malloc((size_t)boneCount * sizeof(int32_t));
    if (!newBones) { GLOG("[SELF] malloc bone ids failed"); return false; }
    memcpy(newBones, boneIds, (size_t)boneCount * sizeof(int32_t));

    int32_t oldSize = *(int32_t*)(base + L.offBoneSize);
    void*   oldData = *(void**)(base + L.offBoneData);
    int64_t oldCap  = *(int64_t*)(base + L.offBoneCap);
    GLOG("[SELF] old bones data=%p size=%d cap=%lld", oldData, oldSize, (long long)oldCap);

    // 登记给析构 hook（只在 hook 真装上时才有意义；Dobby 走不通时是空操作）
    if (bonehook::installed()) {
        bonehook::set_ours(newBones, oldData, oldSize, oldCap);
    }

    // capacity 最低位设 1：Unity 的容器析构读 [容器+0x18] 的 byte0，
    // bit0==1 直接 return，不调 Deallocate —— 这样它不会 free 我们 malloc 的
    // 这块内存（否则 heap corruption 必崩）。
    const int64_t capOdd = (int64_t)((int64_t)boneCount | 1);

    *(void**)(base + L.offBoneData) = newBones;
    *(int32_t*)(base + L.offBoneSize) = boneCount;
    *(int64_t*)(base + L.offBoneCap) = capOdd;
    out.bones = boneCount;

    // ---- 4) 写 native m_Mesh ----
    int32_t meshId = uo::instance_id(meshObj);
    if (meshId == 0) { GLOG("[SELF] mesh instanceID is 0"); return false; }
    *(int32_t*)(base + L.offMesh) = meshId;
    out.meshId = meshId;

    // ---- 5) 材质 ----
    if (materials && matCount > 0) {
        Smr tmp;
        tmp.managed = comp;
        tmp.native = out.native;
        tmp.meshObj = meshObj;
        tmp.boneSlots = boneCount;
        tmp.meshId = meshId;
        if (!set_shared_materials(tmp, materials, matCount)) {
            GLOG("[SELF] set_shared_materials failed (材质会是默认)");
        }
    }

    // ---- 6) 补 m_LocalBounds.extents ----
    // build-105 实测：自建 SMR 的 bounds extents 全 0 -> isVisible=0 -> 被视锥剔除，
    // 这就是「日志全 SUCCESS、屏幕全空白」的直接原因。
    // native 对比把字段钉死在 +0xcc/+0xd0/+0xd4（ref 的三个 float 与
    // managed get_bounds() 的 extents 完全一致）；center 在 +0xc0..+0xc8。
    // build-106 实测：+0xcc 是 world bounds 的缓存，写它没用（get_bounds 仍返回零）。
    // 真正的 localBounds 在 +0xd8（center 3 float + extents 3 float），
    // 特征：center 接近 0、extents 非零；+0x270 有一份同样的拷贝，两处都写。
    {
        uint8_t* rn = sourceSmr ? (uint8_t*)obj_native(sourceSmr) : nullptr;
        uint8_t* sn = (uint8_t*)out.native;
        if (rn) {
            memcpy(sn + 0xd8, rn + 0xd8, 24);
            memcpy(sn + 0x270, rn + 0x270, 24);
            float* sc = (float*)(sn + 0xd8);
            float* se = (float*)(sn + 0xe4);
            if (se[0] <= 0.0f || se[1] <= 0.0f || se[2] <= 0.0f) {
                sc[0] = 0; sc[1] = 10; sc[2] = 0;
                se[0] = 10; se[1] = 12; se[2] = 10;
                memcpy(sn + 0x270, sn + 0xd8, 24);
            }
            float* c2 = (float*)(sn + 0xd8);
            float* e2 = (float*)(sn + 0xe4);
            GLOG("[SELF] localBounds <- c=(%.3f, %.3f, %.3f) e=(%.3f, %.3f, %.3f)",
                 c2[0], c2[1], c2[2], e2[0], e2[1], e2[2]);
        } else {
            float* c2 = (float*)(sn + 0xd8);
            float* e2 = (float*)(sn + 0xe4);
            c2[0] = 0; c2[1] = 10; c2[2] = 0;
            e2[0] = 10; e2[1] = 12; e2[2] = 10;
            memcpy(sn + 0x270, sn + 0xd8, 24);
            GLOG("[SELF] localBounds <- 默认 c=(0,10,0) e=(10,12,10)");
        }
    }

    // ---- 7) 试验：共享 ref 的 +0x1d8（疑似 m_Skin）----
    // build-108 实测：self 该槽为 0、ref 非零，且 self 永远 isVisible=0。
    // 先把 ref 的指针抄过来，看 isVisible 变不变、崩不崩：
    //   变 1 且出画面 -> 共享可行（先跑通链路）
    //   变 1 但乱    -> 骨骼数不匹配，需要自己构造 Skin
    //   崩           -> 不能共享
    {
        uint8_t* rn = sourceSmr ? (uint8_t*)obj_native(sourceSmr) : nullptr;
        uint8_t* sn = (uint8_t*)out.native;
        if (rn) {
            uint64_t rv = *(uint64_t*)(rn + 0x1d8);
            GLOG("[SELF] +0x1d8 ref=0x%llx（共享会崩：162 vs 224 骨骼，跳过写入）",
                 (unsigned long long)rv);
        }
        (void)sn;
    }

    // ===== SMR 方法表全扫（闸门：set_bones / set_sharedMesh 到底在不在）=====
    {
        static bool scanned = false;
        if (!scanned) {
            scanned = true;
            const char* keys = "bones,sharedMesh,rootBone,bound,material,bake,skin,update,enabled,weight";
            void* classes[2] = { uo::u_class("UnityEngine", "SkinnedMeshRenderer"),
                                 uo::u_class("UnityEngine", "Renderer") };
            const char* cnames[2] = { "SMR", "Renderer" };
            for (int c = 0; c < 2; c++) {
                if (!classes[c]) { GLOG("[SMRAPI] %s class missing", cnames[c]); continue; }
                void* iter = nullptr;
                const MethodInfo* m = nullptr;
                int total = 0, matched = 0;
                while ((m = g_api.class_get_methods((Il2CppClass*)classes[c], &iter)) != nullptr) {
                    total++;
                    const char* nm = g_api.method_get_name(m);
                    if (!nm) continue;
                    bool hit = false;
                    for (const char* p2 = keys; *p2; ) {
                        const char* comma = strchr(p2, ',');
                        size_t len = comma ? (size_t)(comma - p2) : strlen(p2);
                        if (len && strstr(nm, p2) && strstr(nm, p2) == nm) { hit = true; break; }
                        if (len && strstr(nm, p2)) { hit = true; break; }
                        if (!comma) break;
                        p2 = comma + 1;
                    }
                    if (!hit) continue;
                    matched++;
                    GLOG("[SMRAPI] %s.%s argc=%u", cnames[c], nm,
                         (unsigned)g_api.method_get_param_count(m));
                }
                GLOG("[SMRAPI] %s total=%d matched=%d", cnames[c], total, matched);
            }
        }
    }

    // get_sharedMesh 读回验证（mesh 有没有被 GC 收掉）
    if (out.smr) {
        static void* mGetMesh = nullptr;
        if (!mGetMesh) {
            void* smrCls = uo::u_class("UnityEngine", "SkinnedMeshRenderer");
            if (smrCls) mGetMesh = uo::u_method(smrCls, "get_sharedMesh", 0);
        }
        if (mGetMesh) {
            void* m = uo::u_call(out.smr, mGetMesh, nullptr);
            GLOG("[SELF] get_sharedMesh 读回 = %p%s", m, m ? "" : "  (NULL! mesh 丢了)");
            if (m) {
                void* meshCls = uo::u_class("UnityEngine", "Mesh");
                void* mGetVC = meshCls ? uo::u_method(meshCls, "get_vertexCount", 0) : nullptr;
                void* mGetB  = meshCls ? uo::u_method(meshCls, "get_bounds", 0) : nullptr;
                void* mGetV  = meshCls ? uo::u_method(meshCls, "get_vertices", 0) : nullptr;
                if (mGetVC) {
                    void* r = uo::u_call(m, mGetVC, nullptr);
                    int vc = r ? *(int32_t*)((char*)r + 16) : -1;
                    GLOG("[SELF] mesh.vertexCount = %d (expect 21804)%s", vc,
                         vc == 21804 ? "" : "  <-- 顶点没写进去！");
                }
                if (mGetB) {
                    void* r = uo::u_call(m, mGetB, nullptr);
                    if (r) {
                        float* b = (float*)((char*)r + 16);
                        GLOG("[SELF] mesh.bounds c=(%.2f, %.2f, %.2f) e=(%.2f, %.2f, %.2f)",
                             b[0], b[1], b[2], b[3], b[4], b[5]);
                    }
                }
                if (mGetV) {
                    void* arr = uo::u_call(m, mGetV, nullptr);
                    if (arr) {
                        int32_t n = *(int32_t*)((char*)arr + 24);
                        float* v = (float*)((char*)arr + 32);
                        GLOG("[SELF] vertices n=%d v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f)",
                             n, v[0], v[1], v[2], v[3], v[4], v[5]);
                    } else {
                        GLOG("[SELF] get_vertices 返回 null");
                    }
                }
            }
        } else {
            GLOG("[SELF] get_sharedMesh 方法缺失");
        }
    }

    g_selfSmrManaged = out.smr;
    g_selfSmrNative  = out.native;

    dump_visibility(out, sourceSmr, modelTransform);
    GLOG("[SELF] done: meshId=%d bones=%d mats=%d", meshId, boneCount, matCount);
    return true;
}

void probe_self_tick() {
    static int n = 0;
    if (++n % 60 != 1) return;
    if (!g_selfSmrManaged) return;

    void* rCls = uo::u_class("UnityEngine", "Renderer");
    if (!rCls) return;
    void* mVis = uo::u_method(rCls, "get_isVisible", 0);
    void* mB   = uo::u_method(rCls, "get_bounds", 0);

    int vis = -1;
    if (mVis) {
        void* r = uo::u_call(g_selfSmrManaged, mVis, nullptr);
        if (r) vis = *(uint8_t*)((char*)r + 16) ? 1 : 0;
    }
    float bx = 0, by = 0, bz = 0;
    if (mB) {
        void* r = uo::u_call(g_selfSmrManaged, mB, nullptr);
        if (r) {
            float* b = (float*)((char*)r + 16);
            bx = b[3]; by = b[4]; bz = b[5];
        }
    }
    GLOG("[VIS] tick#%d isVisible=%d ext=(%.3f, %.3f, %.3f)", n, vis, bx, by, bz);
}

void destroy_self_smr(SelfSmr& s) {
    // 目前不主动销毁：Unity 会在场景销毁时处理，
    // 骨骼容器析构由 bone_hook 兜底还原。
    s = SelfSmr{};
}

}  // namespace game
