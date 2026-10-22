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
    void*   mf     = nullptr;   // Il2CppObject* MeshFilter   (D 方案)
    void*   mr     = nullptr;   // Il2CppObject* MeshRenderer (D 方案)
    int32_t meshId = 0;
    int     bones  = 0;
    int     mode   = 0;         // 0 = 自建 SMR，1 = MeshFilter+MeshRenderer
    int     layer  = -1;        // 模型 GO 实际生效的 layer（读回值）
};

// modelTransform：已经建好的模型根节点（挂在游戏角色下、带缩放的那一个）。
//                 SMR 组件加到它所在的 GameObject 上，不再自己建节点。
// sourceSmr：目标角色的 SMR（Il2CppObject*）。只用来拿 layer ——
//            不拷 layer 的话，游戏相机 cullingMask 里没有 Default 层，模型画了也看不见。
// boneIds：每根骨骼 Transform 的 instanceID 数组。
//          build-103 实测 dump：游戏原 SMR 的 m_Bones.data 前 8 个 4 字节全是
//          0x0004xxxx 小整数 -> native m_Bones 就是 4 字节 instanceID 数组，
//          不是 8 字节指针数组（8 字节写法崩在同一地址，证明宽度不是根因）。
bool create_self_smr(void* modelTransform,
                     void* sourceSmr,
                     void* meshObj,
                     const int32_t* boneIds, int boneCount,
                     void** materials, int matCount,
                     SelfSmr& out);

// =====================================================================
// D 方案：MeshFilter + MeshRenderer
//
// 为什么要这条路（2026-10-22 实测）：自建 SMR 的 m_Mesh / m_Bones / 材质
// 全部写对、get_sharedMesh 读回非空、isVisible=1，但 +0x1d8 建不出来、
// Renderer bounds 恒 0 —— Unity 的 SMR 蒙皮缓存只走它自己的 SetMesh 才建，
// 直写字段绕过了那一步。MeshRenderer 没有蒙皮，顶点缓冲由 Unity 标准路径
// 上传，用来把「模型能不能画出来」和「蒙皮」解耦。
//
// 返回 false 时调用方要回退 create_self_smr()。
// =====================================================================
bool create_self_mesh_renderer(void* modelTransform,
                               void* sourceSmr,
                               void* meshObj,
                               void** materials, int matCount,
                               SelfSmr& out);

// 每 60 帧读一次自建渲染器的 isVisible / bounds（渲染系统跨帧更新的缓存）
void probe_self_tick();

// 销毁（调试用；不调用也不会立刻析构）
void destroy_self_smr(SelfSmr& s);

}  // namespace game
