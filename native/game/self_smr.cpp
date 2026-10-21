// =====================================================================
// self_smr.cpp —— 自建 SkinnedMeshRenderer（C 方案）
// =====================================================================
#include "self_smr.h"

#include <cstdlib>
#include <cstring>

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

bool create_self_smr(const char* name,
                     void* parentTransform,
                     void* meshObj,
                     const int32_t* boneIds, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out) {
    out = SelfSmr{};
    if (!meshObj || !boneIds || boneCount <= 0) {
        GLOG("[SELF] bad args mesh=%p ids=%p n=%d", meshObj, (void*)boneIds, boneCount);
        return false;
    }

    Il2CppClass* smrCls = (Il2CppClass*)uo::u_class("UnityEngine", "SkinnedMeshRenderer");
    if (!smrCls) { GLOG("[SELF] SMR class missing"); return false; }

    // ---- 1) GameObject + Transform ----
    void* tr = uo::create_node(name, parentTransform, 0.0f, 0.0f, 0.0f);
    if (!tr) { GLOG("[SELF] create_node failed"); return false; }
    out.tr = tr;

    Il2CppClass* trCls = (Il2CppClass*)uo::u_class("UnityEngine", "Transform");
    const MethodInfo* mGetGO = (const MethodInfo*)uo::u_method(trCls, "get_gameObject", 0);
    void* go = mGetGO ? uo::u_call(tr, (void*)mGetGO, nullptr) : nullptr;
    if (!go) { GLOG("[SELF] get_gameObject failed"); return false; }
    out.go = go;
    GLOG("[SELF] GameObject=%p Transform=%p", go, tr);

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

    // 登记给析构 hook（自建 SMR 销毁时把字段还原，避免 Unity free 我们的内存）
    bonehook::set_ours(newIds, oldData, oldSize, oldCap);

    *(void**)(base + L.offBoneData) = newIds;
    *(int32_t*)(base + L.offBoneSize) = boneCount;
    *(int64_t*)(base + L.offBoneCap) = (int64_t)boneCount;
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

    GLOG("[SELF] done: meshId=%d bones=%d mats=%d", meshId, boneCount, matCount);
    return true;
}

void destroy_self_smr(SelfSmr& s) {
    // 目前不主动销毁：Unity 会在场景销毁时处理，
    // 骨骼容器析构由 bone_hook 兜底还原。
    s = SelfSmr{};
}

}  // namespace game
