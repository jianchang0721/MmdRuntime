// =====================================================================
// mesh_builder.cpp —— PMX → UnityEngine.Mesh
//
// 数据规模（目标模型实测）：21804 顶点 / 82965 索引 / 224 骨骼 / 25 材质
// 一次性分配的总量约 1.7MB managed 数组，分步创建、每步用完就丢引用。
//
// 关键点：
//   1. il2cpp_array_new 传的是「元素类」，不是数组类
//   2. 数组数据从 +32 开始（klass8 + monitor8 + bounds8 + max_length4 + pad4）
//   3. BoneWeight 是 32 字节：4 个 int 索引 + 4 个 float 权重
//   4. bindpose = 骨骼世界矩阵的逆。MMD 骨骼初始旋转为单位，所以就是平移取负
//   5. Unity 的 Matrix4x4 内存序是 m00,m10,m20,m30,m01,m11,... 平移在 12/13/14
// =====================================================================
#include "mesh_builder.h"

#include <android/log.h>
#include <cstring>
#include <vector>

#include "unity/unity_objects.h"

#define MTAG "MmdMesh"
#define MLOG(...) __android_log_print(ANDROID_LOG_INFO, MTAG, __VA_ARGS__)

namespace mmd {

namespace {

struct MeshClasses {
    void* meshCls = nullptr;
    void* mCtor = nullptr;
    void* mSetVertices = nullptr;
    void* mSetNormals = nullptr;
    void* mSetUV = nullptr;
    void* mSetTriangles = nullptr;
    void* mSetBoneWeights = nullptr;
    void* mSetBindposes = nullptr;
    void* mUpload = nullptr;

    void* elemVec3 = nullptr;
    void* elemVec2 = nullptr;
    void* elemInt = nullptr;
    void* elemBoneWeight = nullptr;
    void* elemMatrix4x4 = nullptr;
};

bool init_mesh_classes(MeshClasses& mc) {
    mc.meshCls = uo::u_class("UnityEngine", "Mesh");
    if (!mc.meshCls) { MLOG("[MESH] Mesh class not found"); return false; }

    mc.mCtor            = uo::u_method(mc.meshCls, ".ctor", 0);
    mc.mSetVertices     = uo::u_method(mc.meshCls, "set_vertices", 1);
    mc.mSetNormals      = uo::u_method(mc.meshCls, "set_normals", 1);
    mc.mSetUV           = uo::u_method(mc.meshCls, "set_uv", 1);
    mc.mSetTriangles    = uo::u_method(mc.meshCls, "set_triangles", 1);
    mc.mSetBoneWeights  = uo::u_method(mc.meshCls, "set_boneWeights", 1);
    mc.mSetBindposes    = uo::u_method(mc.meshCls, "set_bindposes", 1);
    mc.mUpload          = uo::u_method(mc.meshCls, "UploadMeshData", 1);

    mc.elemVec3        = uo::u_class("UnityEngine", "Vector3");
    mc.elemVec2        = uo::u_class("UnityEngine", "Vector2");
    mc.elemInt         = uo::u_class("System", "Int32");
    mc.elemBoneWeight  = uo::u_class("UnityEngine", "BoneWeight");
    mc.elemMatrix4x4   = uo::u_class("UnityEngine", "Matrix4x4");

    MLOG("[MESH] ctor=%p verts=%p norms=%p uv=%p tris=%p bw=%p bp=%p",
         mc.mCtor, mc.mSetVertices, mc.mSetNormals, mc.mSetUV,
         mc.mSetTriangles, mc.mSetBoneWeights, mc.mSetBindposes);
    MLOG("[MESH] elems vec3=%p vec2=%p int=%p bw=%p m4=%p",
         mc.elemVec3, mc.elemVec2, mc.elemInt, mc.elemBoneWeight, mc.elemMatrix4x4);

    if (!mc.mCtor || !mc.mSetVertices || !mc.mSetNormals || !mc.mSetUV || !mc.mSetTriangles) {
        MLOG("[MESH] essential setter missing");
        return false;
    }
    if (!mc.elemVec3 || !mc.elemVec2 || !mc.elemInt) {
        MLOG("[MESH] essential element class missing");
        return false;
    }
    return true;
}

}  // namespace

bool build_mesh(const Model& model, const Skeleton& skel, BuiltMesh& out) {
    out = BuiltMesh{};

    const size_t nv = model.verts.size();
    const size_t nf = model.faceVertex.size();
    const int    nb = skel.count();
    if (nv == 0 || nf == 0) {
        MLOG("[MESH] empty model nv=%zu nf=%zu", nv, nf);
        return false;
    }

    MeshClasses mc;
    if (!init_mesh_classes(mc)) return false;

    // ---- 1) new Mesh ----
    void* mesh = uo::u_new_object(mc.meshCls);
    if (!mesh) { MLOG("[MESH] object_new failed"); return false; }
    uo::u_call(mesh, mc.mCtor, nullptr);

    // ---- 2) vertices ----
    {
        void* arr = uo::u_new_array(mc.elemVec3, nv);
        if (!arr) { MLOG("[MESH] vertices array alloc failed"); return false; }
        float* d = (float*)uo::u_array_data(arr);
        for (size_t i = 0; i < nv; i++) {
            const Vertex& v = model.verts[i];
            d[i * 3 + 0] = v.pos[0];
            d[i * 3 + 1] = v.pos[1];
            d[i * 3 + 2] = v.pos[2];
        }
        void* a[1] = { arr };
        uo::u_call(mesh, mc.mSetVertices, a);
    }

    // ---- 3) normals ----
    {
        void* arr = uo::u_new_array(mc.elemVec3, nv);
        if (arr) {
            float* d = (float*)uo::u_array_data(arr);
            for (size_t i = 0; i < nv; i++) {
                const Vertex& v = model.verts[i];
                d[i * 3 + 0] = v.nrm[0];
                d[i * 3 + 1] = v.nrm[1];
                d[i * 3 + 2] = v.nrm[2];
            }
            void* a[1] = { arr };
            uo::u_call(mesh, mc.mSetNormals, a);
        }
    }

    // ---- 4) uv ----
    {
        void* arr = uo::u_new_array(mc.elemVec2, nv);
        if (arr) {
            float* d = (float*)uo::u_array_data(arr);
            for (size_t i = 0; i < nv; i++) {
                const Vertex& v = model.verts[i];
                d[i * 2 + 0] = v.uv[0];
                d[i * 2 + 1] = v.uv[1];
            }
            void* a[1] = { arr };
            uo::u_call(mesh, mc.mSetUV, a);
        }
    }

    // ---- 5) triangles ----
    {
        void* arr = uo::u_new_array(mc.elemInt, nf);
        if (!arr) { MLOG("[MESH] triangles array alloc failed"); return false; }
        int32_t* d = (int32_t*)uo::u_array_data(arr);
        for (size_t i = 0; i < nf; i++) d[i] = model.faceVertex[i];
        void* a[1] = { arr };
        uo::u_call(mesh, mc.mSetTriangles, a);
    }

    // ---- 6) boneWeights ----
    // Unity BoneWeight 内存布局：int32 boneIndex0..3, float weight0..3（共 32 字节）
    if (mc.mSetBoneWeights && mc.elemBoneWeight) {
        void* arr = uo::u_new_array(mc.elemBoneWeight, nv);
        if (arr) {
            char* base = (char*)uo::u_array_data(arr);
            for (size_t i = 0; i < nv; i++) {
                const Vertex& v = model.verts[i];
                char* p = base + i * 32;
                int32_t* idx = (int32_t*)p;
                float* w = (float*)(p + 16);
                for (int k = 0; k < 4; k++) {
                    int bi = v.bone[k];
                    idx[k] = (bi >= 0 && bi < nb) ? bi : 0;
                    w[k] = (bi >= 0 && bi < nb) ? v.weight[k] : 0.0f;
                }
                // BDEF1/BDEF2 的剩余权重是 0，Unity 能接受；若全为 0 会退化，兜个底
                if (w[0] == 0.0f && w[1] == 0.0f && w[2] == 0.0f && w[3] == 0.0f) {
                    w[0] = 1.0f;
                }
            }
            void* a[1] = { arr };
            uo::u_call(mesh, mc.mSetBoneWeights, a);
        }
    } else {
        MLOG("[MESH] set_boneWeights missing, skinning will use single bone");
    }

    // ---- 7) bindposes ----
    // 每根骨骼一个：世界矩阵的逆。MMD 骨骼初始是单位旋转 + 平移，所以逆 = 平移取负
    if (mc.mSetBindposes && mc.elemMatrix4x4 && nb > 0) {
        void* arr = uo::u_new_array(mc.elemMatrix4x4, (size_t)nb);
        if (arr) {
            float* base = (float*)uo::u_array_data(arr);
            for (int i = 0; i < nb; i++) {
                float* m = base + (size_t)i * 16;
                memset(m, 0, 16 * sizeof(float));
                const BoneNode& n = skel.at(i);
                m[0] = 1.0f;
                m[5] = 1.0f;
                m[10] = 1.0f;
                m[15] = 1.0f;
                // Unity Matrix4x4 内存序：m00,m10,m20,m30, m01,m11,m21,m31, ...
                // 平移落在 m03/m13/m23 → 索引 12/13/14
                m[12] = -n.worldPos[0];
                m[13] = -n.worldPos[1];
                m[14] = -n.worldPos[2];
            }
            void* a[1] = { arr };
            uo::u_call(mesh, mc.mSetBindposes, a);
        }
    } else {
        MLOG("[MESH] set_bindposes missing or no bones");
    }

    // ---- 8) UploadMeshData ----
    if (mc.mUpload) {
        uint8_t keepReadable = 1;   // true：保留 CPU 副本，方便后面调试
        void* a[1] = { &keepReadable };
        uo::u_call(mesh, mc.mUpload, a);
    }

    out.mesh = mesh;
    out.vertexCount = (int)nv;
    out.indexCount = (int)nf;
    out.boneCount = nb;
    out.subMeshes = 1;
    out.ok = true;

    MLOG("[MESH] built ok: verts=%zu tris=%zu bones=%d mesh=%p",
         nv, nf / 3, nb, mesh);
    return true;
}

}  // namespace mmd
