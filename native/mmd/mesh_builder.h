// =====================================================================
// mesh_builder.h —— PMX → UnityEngine.Mesh
//
// 一次性把所有顶点数据灌进去：vertices / normals / uv / triangles /
// boneWeights / bindposes。之后的每帧蒙皮由 Unity 自己做（GPU），
// 我们只管摆骨骼 Transform。
// =====================================================================
#pragma once
#include <cstdint>

#include "pmx.h"
#include "skeleton.h"

namespace mmd {

struct BuiltMesh {
    void* mesh        = nullptr;   // Il2CppObject* (UnityEngine.Mesh)
    int   vertexCount = 0;
    int   indexCount  = 0;
    int   boneCount   = 0;
    int   subMeshes   = 0;         // 按材质切的子网格数（现在是 1，材质分离后面做）
    bool  ok          = false;
};

// skel 必须已经 build 好（需要每根骨骼的绝对坐标来算 bindpose）
//
// templateMesh：从目标 SMR 拿到的 sharedMesh。非空时用它克隆出一个新 Mesh，
// 而不是 object_new + Mesh..ctor() —— 后者在 Unity 2018 IL2CPP 下会崩
// （cto r里要创建 native Mesh，实测静默退出且不留 tombstone）。
bool build_mesh(const Model& model, const Skeleton& skel,
                void* templateMesh, BuiltMesh& out);

}  // namespace mmd
