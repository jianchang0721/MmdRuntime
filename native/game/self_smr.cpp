// =====================================================================
// self_smr.cpp —— 自建 SkinnedMeshRenderer（C 方案）
// =====================================================================
#include "self_smr.h"

#include <cstdlib>
#include <cstring>

#include "logsink.h"
#include "smr_bind.h"
#include "smr_layout.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define GTAG "MmdSelfSmr"
#define GLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {

namespace {

// boxed Vector3 的 value 在 +0x10（il2cpp 装箱布局）
struct Vec3 { float x, y, z; };

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

bool create_self_smr(void* modelTransform,
                     void* sourceSmr,
                     void* meshObj,
                     const int32_t* boneIds, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out) {
    out = SelfSmr{};
    if (!modelTransform || !meshObj || !boneIds || boneCount <= 0) {
        GLOG("[SELF] bad args tr=%p mesh=%p ids=%p n=%d",
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
        GLOG("[SELF] layer copied: %d (setLayer=%p)", layer, (void*)mSetLayer);
    }

    // ---- 拷贝 layer 后：朝向 + 父级缩放反算（ModelMod 2265~2312 行原样搬）----
    // 两条都是「看不见」的真凶，ModelMod 里踩过：
    //   ① MMD/glTF 多数面朝 -Z，Unity 是 +Z，不转 180 就背对镜头
    //   ② 「角色节点自带的缩放不是 1（很多游戏模型单位是厘米，节点缩放 0.01），
    //       直接写 localScale=20 会被父级再乘一次，直接缩到看不见」
    // 复用上面 layer 那段已经拿到的 Transform 类指针
    Il2CppClass* trCls2 = (Il2CppClass*)uo::u_class("UnityEngine", "Transform");
    if (trCls2) {
        // 面朝 +Z（绕 Y 转 180）
        const MethodInfo* mSetRot = (const MethodInfo*)uo::u_method(trCls2, "set_localRotation", 1);
        if (mSetRot) {
            struct { float x, y, z, w; } q = { 0.0f, 1.0f, 0.0f, 0.0f };
            void* a[1] = { &q };
            uo::u_call_void(tr, (void*)mSetRot, a);
            GLOG("[SELF] rotated 180 on Y (MMD faces -Z, Unity faces +Z)");
        }

        // 读父级 lossyScale，用来把「绝对缩放」换算成「localScale」
        // 不然父级一乘，0.08 直接变 0.0008，缩到看不见
        const MethodInfo* mLossy = (const MethodInfo*)uo::u_method(trCls2, "get_lossyScale", 0);
        float psc = 1.0f;
        if (modelTransform && mLossy) {
            // modelTransform 就是挂载点，直接读它自己的 lossyScale
            void* ls = uo::u_call(modelTransform, (void*)mLossy, nullptr);
            if (ls) {
                Vec3 v = *(Vec3*)((char*)ls + 16);
                GLOG("[SELF] parent lossyScale = %.6f %.6f %.6f", v.x, v.y, v.z);
                if (v.x > 0.00001f) psc = v.x;
            }
        }
        GLOG("[SELF] parent scale factor psc=%.6f", psc);
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

    // ---- 3) 写 native m_Bones（新对象 size=0，随便给）----
    uint8_t* base = (uint8_t*)out.native;
    const auto& L = layout();

    int32_t* newIds = (int32_t*)malloc((size_t)boneCount * sizeof(int32_t));
    if (!newIds) { GLOG("[SELF] malloc bone ids failed"); return false; }
    memcpy(newIds, boneIds, (size_t)boneCount * sizeof(int32_t));

    int32_t oldSize = *(int32_t*)(base + L.offBoneSize);
    void*   oldData = *(void**)(base + L.offBoneData);
    int64_t oldCap  = *(int64_t*)(base + L.offBoneCap);
    GLOG("[SELF] old bones data=%p size=%d cap=%lld", oldData, oldSize, (long long)oldCap);

    // 不装 hook：capacity 最低位置 1 就够 Unity 跳过 Deallocate（见下面）。
    // build-88 实测 DobbyHook 在 libunity 上 abort，这条路彻底不走了。

    *(void**)(base + L.offBoneData) = newIds;
    *(int32_t*)(base + L.offBoneSize) = boneCount;
    // capacity 最低位置 1 = 「这块内存不归 Unity 管，析构时跳过 Deallocate」。
    // 这是 ModelMod 阶段挖出来的容器析构行为（读 m_Bones+0x18 的 bit0），
    // 实测 Unity 自扩容的 capacity 恒偶数，bit0 正是留给这个标志。
    // 用它可以完全不要 bone_hook，避开 build-88 那个 DobbyHook abort。
    *(int64_t*)(base + L.offBoneCap) = (int64_t)boneCount | 1;
    out.bones = boneCount;
    GLOG("[SELF] bones: data=%p size=%d cap(odd)=%lld —— 析构不释放我们的内存",
         (void*)newIds, boneCount, (long long)((int64_t)boneCount | 1));

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

    GLOG("[SELF] done: meshId=%d bones=%d mats=%d", meshId, boneCount, matCount);
    return true;
}

void destroy_self_smr(SelfSmr& s) {
    // 目前不主动销毁：Unity 会在场景销毁时处理，
    // 骨骼容器析构由 bone_hook 兜底还原。
    s = SelfSmr{};
}

}  // namespace game
