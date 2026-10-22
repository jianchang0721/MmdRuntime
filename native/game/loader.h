// =====================================================================
// loader.h —— 一个模型的完整加载流程
//
//   ① 装 bone hook（析构保护，必须先于扩容）
//   ② PMX 224 根骨骼 → Transform 层级树
//   ③ PMX → Unity Mesh（顶点/法线/UV/索引/权重/bindposes）
//   ④ m_Bones 扩容到 224 并写入我们骨骼的 instanceID
//   ⑤ m_Mesh 换成我们的 mesh
//   ⑥ 读回校验
//
// 任何一步失败都停在那一步并写日志，不做半吊子状态。
// =====================================================================
#pragma once
#include <cstring>
#include <vector>

#include "game/smr_bind.h"
#include "mmd/mesh_builder.h"
#include "mmd/pmx.h"
#include "mmd/skeleton.h"

namespace game {

struct LoadResult {
    bool ok = false;
    int  bonesBuilt = 0;
    int  verts = 0;
    int  tris = 0;
    int  boneSlots = 0;     // 扩容后 SMR 认为的骨骼数
    int  subMeshes = 0;     // mesh 的子网格数（= 材质数）
    int  materials = 0;     // 成功做出的材质数
    int  textures  = 0;     // 成功解码的贴图数
    int32_t meshId = 0;
    void* mesh = nullptr;
    void* modelRoot = nullptr;   // Transform of our MMDModel node (scene root)
    char  error[256] = {0};
    // 钉住 mesh / 材质的 gchandle（C++ 裸指针引用，GC 不认，必须 pin）
    std::vector<uint32_t> pinHandles;
    mmd::Skeleton skeleton;   // 持有骨骼树（Transform 引用）
};

// roleRoot：把骨骼树挂到哪个 Transform 下（游戏角色的节点）。
// 传 nullptr 就挂场景根，先跑通再说。
// pmxPath：PMX 绝对路径，贴图按它推目录
bool load_model_into_smr(const mmd::Model& model,
                         const char* pmxPath,
                         Smr& smr,
                         void* roleRoot,
                         LoadResult& out);

}  // namespace game
