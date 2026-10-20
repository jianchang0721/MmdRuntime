// =====================================================================
// smr_bind.cpp
// =====================================================================
#include "smr_bind.h"

#include <android/log.h>
#include <cstdlib>
#include <cstring>

#include "bone_hook.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define GTAG "MmdGame"
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, GTAG, __VA_ARGS__)

namespace game {

namespace {

// native SkinnedMeshRenderer 字段偏移（libunity.so 反汇编实测，2018.4.13f1）
const int OFF_MESH    = 0x1d4;   // PPtr<Mesh>，4 字节 instanceID
const int OFF_BONE_D  = 0x1e0;   // dynamic_array.data
const int OFF_BONE_SZ = 0x1f0;   // dynamic_array.size   (int32)
const int OFF_BONE_CAP= 0x1f8;   // dynamic_array.capacity (int64)

}  // namespace

int expand_bones(Smr& smr, const int32_t* instanceIds, int count) {
    if (!smr.native || !instanceIds || count <= 0) {
        GLOG("[GAME] expand_bones bad args native=%p ids=%p count=%d",
             smr.native, (const void*)instanceIds, count);
        return -1;
    }
    if (!bonehook::installed()) {
        GLOG("[GAME] expand_bones refused: bone hook not installed "
             "(析构时会 free 我们的指针)");
        return -1;
    }

    uint8_t* base = (uint8_t*)smr.native;
    int32_t* oldData = *(int32_t**)(base + OFF_BONE_D);
    int32_t  oldSize = *(int32_t*)(base + OFF_BONE_SZ);
    int64_t  oldCap  = *(int64_t*)(base + OFF_BONE_CAP);

    if (!oldData || oldSize <= 0 || oldSize > 4096) {
        GLOG("[GAME] expand_bones bad current state data=%p size=%d", oldData, oldSize);
        return -1;
    }

    int32_t* newData = (int32_t*)malloc((size_t)count * sizeof(int32_t));
    if (!newData) {
        GLOG("[GAME] expand_bones malloc %d bytes failed", count * 4);
        return -1;
    }
    memcpy(newData, instanceIds, (size_t)count * sizeof(int32_t));

    // 先把「原值」登记给 hook，再改字段 —— 顺序不能反，
    // 否则中间崩掉的话 hook 拿不到原指针
    bonehook::set_ours(newData, oldData, oldSize, oldCap);

    *(int32_t**)(base + OFF_BONE_D)  = newData;
    *(int32_t*)(base + OFF_BONE_SZ)  = count;
    *(int64_t*)(base + OFF_BONE_CAP) = (int64_t)count;

    smr.boneSlots = count;

    GLOG("[GAME] expand bones %d -> %d  data=%p (orig %p) cap %lld -> %d",
         oldSize, count, (void*)newData, (void*)oldData,
         (long long)oldCap, count);
    return count;
}

bool set_mesh_instance(Smr& smr, int32_t meshInstanceId) {
    if (!smr.native || meshInstanceId == 0) return false;

    uint8_t* base = (uint8_t*)smr.native;
    int32_t old = *(int32_t*)(base + OFF_MESH);
    *(int32_t*)(base + OFF_MESH) = meshInstanceId;
    smr.meshId = meshInstanceId;

    GLOG("[GAME] set mesh instanceID %d -> %d", old, meshInstanceId);
    return true;
}

bool set_shared_materials(Smr& smr, void** materials, int count) {
    if (!smr.managed || !materials || count <= 0) {
        GLOG("[GAME] set_shared_materials bad args managed=%p mats=%p count=%d",
             smr.managed, (void*)materials, count);
        return false;
    }

    static void* s_setShared = nullptr;
    static void* s_matCls    = nullptr;
    static bool  s_tried     = false;

    if (!s_tried) {
        s_tried = true;
        void* rendererCls = uo::u_class("UnityEngine", "Renderer");
        if (rendererCls) {
            // 不同 Unity 版本命名不同，两个都试
            s_setShared = uo::u_method(rendererCls, "set_sharedMaterials", 1);
            if (!s_setShared) s_setShared = uo::u_method(rendererCls, "SetSharedMaterials", 1);
            if (!s_setShared) s_setShared = uo::u_method(rendererCls, "set_materials", 1);
        }
        s_matCls = uo::u_class("UnityEngine", "Material");
        GLOG("[GAME] sharedMaterials setter=%p matCls=%p", s_setShared, s_matCls);
    }

    if (!s_setShared || !s_matCls) {
        GLOG("[GAME] 找不到 set_sharedMaterials，材质只能靠 native 改（本版未做）");
        return false;
    }

    void* arr = uo::u_new_array(s_matCls, (size_t)count);
    if (!arr) { GLOG("[GAME] material array alloc failed"); return false; }

    void** dst = (void**)uo::u_array_data(arr);
    for (int i = 0; i < count; i++) dst[i] = materials[i];

    void* a[1] = { arr };
    uo::u_call(smr.managed, s_setShared, a);
    GLOG("[GAME] set sharedMaterials x%d", count);
    return true;
}

bool read_back(const Smr& smr, int32_t* boneSlots, int32_t* meshId) {
    if (!smr.native) return false;
    const uint8_t* base = (const uint8_t*)smr.native;
    if (boneSlots) *boneSlots = *(const int32_t*)(base + OFF_BONE_SZ);
    if (meshId)    *meshId    = *(const int32_t*)(base + OFF_MESH);
    return true;
}

}  // namespace game
