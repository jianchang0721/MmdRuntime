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

// modelTransform：已经建好的模型根节点（挂在游戏角色下、带缩放的那一个）。
//                 SMR 组件加到它所在的 GameObject 上，不再自己建节点。
// sourceSmr：目标角色的 SMR（Il2CppObject*）。只用来拿 layer ——
//            不拷 layer 的话，游戏相机 cullingMask 里没有 Default 层，模型画了也看不见。
// bonePtrs：每根骨骼的 native Transform*（managed 对象 +16 就是 m_CachedPtr）。
//           native m_Bones 是 8 字节/元素的指针数组，不是 4 字节 instanceID 数组
//           （build-102 实测 fault addr 0x0006800600000001 = 两个 id 被拼成指针）
bool create_self_smr(void* modelTransform,
                     void* sourceSmr,
                     void* meshObj,
                     void** bonePtrs, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out);

// 销毁（调试用；不调用也不会立刻析构）
void destroy_self_smr(SelfSmr& s);

}  // namespace game
