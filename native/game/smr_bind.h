// =====================================================================
// smr_bind.h —— 对游戏里 SkinnedMeshRenderer 的写入层
//
// 只碰 native 内存的三个字段（偏移全是实测出来的，见 mmdruntime 记忆）：
//   +0x1d4  m_Mesh      int32 instanceID
//   +0x1e0  m_Bones.data
//   +0x1f0  m_Bones.size     int32
//   +0x1f8  m_Bones.capacity int64
//
// 扩容走 malloc + 改三字段，析构安全由 bone_hook 负责（析构前还原原值）。
// =====================================================================
#pragma once
#include <cstdint>

namespace game {

struct Smr {
    void*   managed = nullptr;   // Il2CppObject* (SkinnedMeshRenderer)
    void*   native  = nullptr;   // C++ SkinnedMeshRenderer*
    int32_t boneSlots = 0;       // 当前 m_Bones.size
    int32_t meshId = 0;          // 当前 m_Mesh 的 instanceID
};

// 把 m_Bones 换成我们给的 instanceID 数组（数量任意，会 malloc 新内存）。
// 返回写入的数量，<0 失败。要求 bonehook 已 install()，否则拒绝执行。
int expand_bones(Smr& smr, const int32_t* instanceIds, int count);

// 把 m_Mesh 换成指定 mesh 的 instanceID（4 字节写入）
bool set_mesh_instance(Smr& smr, int32_t meshInstanceId);

// 只读：从 native 指针读回当前 boneSlots / meshId，用于校验
bool read_back(const Smr& smr, int32_t* boneSlots, int32_t* meshId);

// 把材质数组设给 SMR。
//
// 走 managed 的 Renderer.set_sharedMaterials（SMR 自己的 setter 被 strip 了，
// 但基类 Renderer 上那个通常还在）。数量应当等于 mesh 的子网格数，少的那几个
// 子网格会用 UnityEngine 默认材质（粉红），不会崩。
bool set_shared_materials(Smr& smr, void** materials, int count);

}  // namespace game
