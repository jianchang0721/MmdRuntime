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
bool build_mesh(const Model& model, const Skeleton& skel, BuiltMesh& out);

}  // namespace mmd
