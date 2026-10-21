// =====================================================================
// self_smr.h —— 自建渲染对象（C 方案）
//
// 背景（build-79 实测）：给游戏的 SMR 换 m_Mesh 一律崩，
// 连 3 顶点 1 三角形的最小网格都崩。原因是 Unity 2018 的 SMR
// 内部 m_Skin 缓存按旧 mesh 建，换 mesh 不重建。
//
// C 方案：自己 new 一个 GameObject + AddComponent(SkinnedMeshRenderer)，
// 新对象没有历史缓存，第一次渲染时会按我们给的 mesh/bones 重新建 m_Skin。
//
// 游戏角色只当驱动源：骨骼 Transform 读游戏骨骼运动，不写游戏对象。
// =====================================================================
#pragma once
#include <cstdint>

namespace game {

struct SelfSmr {
    void*   go     = nullptr;   // Il2CppObject* GameObject
    void*   tr     = nullptr;   // Il2CppObject* Transform
    void*   smr    = nullptr;   // Il2CppObject* SkinnedMeshRenderer
    void*   native = nullptr;   // C++ SkinnedMeshRenderer*
    int32_t meshId = 0;
    int     bones  = 0;
};

// parentTransform：挂到哪个 Transform 下（传 nullptr = 场景根）
// boneIds：每个骨骼 Transform 的 instanceID 数组
bool create_self_smr(const char* name,
                     void* parentTransform,
                     void* meshObj,
                     const int32_t* boneIds, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out);

// 销毁（调试用；不调用也不会立刻析构）
void destroy_self_smr(SelfSmr& s);

}  // namespace game
