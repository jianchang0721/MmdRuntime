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
#include "logsink.h"
#include "mesh_builder.h"

#include <android/log.h>
#include <cstring>
#include <vector>

#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define MTAG "MmdMesh"
#define MLOG(...) logsink::Write(__VA_ARGS__)

namespace mmd {

namespace {

struct MeshClasses {
    void* meshCls = nullptr;
    void* mCtor = nullptr;
    void* mSetVertices = nullptr;
    void* mSetNormals = nullptr;
    void* mSetUV = nullptr;
    void* mSetTriangles = nullptr;
    void* mSetTrianglesSub = nullptr;   // SetTriangles(int[], int, bool)（Unity 2018 只有 3 参版本）
    void* mGetSubMeshCount = nullptr;   // get_subMeshCount，用来验证 setter 真生效
    void* mSetSubMeshCount = nullptr;   // set_subMeshCount(int)
    void* mSetBoneWeights = nullptr;
    void* mSetBindposes = nullptr;
    void* mUpload = nullptr;
    void* mGetVertexCount = nullptr;   // 验证 mesh 对象是否真的活着

    void* elemVec3 = nullptr;
    void* elemVec2 = nullptr;
    void* elemInt = nullptr;
    void* elemBoneWeight = nullptr;
    void* elemMatrix4x4 = nullptr;
};

// 找「第一个参数是 int[]」的指定方法（避开 List<int> 重载）
// 找 SetTriangles(int[], int) 这种重载。
//
// 之前按 class_get_name(p0) == "Int32[]" 判，build-72 实测一个都没命中
// （25 个子网格全没设上）。所以改成看 Il2CppType 的 type 字段：
//   Il2CppType 布局：data(8) + attrs(2, +8) + type(1, +10) + flags(1, +11)
//   IL2CPP_TYPE_SZARRAY = 0x1D
// 同时把所有重载打出来，换游戏/Unity 版本时一眼能看出参数类型。
void* find_int_array_method(void* klass, const char* name, int nparams) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m = nullptr;
    int seen = 0;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;

        int pc = (int)g_api.method_get_param_count(m);
        const Il2CppType* t0 = g_api.method_get_param(m, 0);
        uint8_t ty = 0;
        const char* c0 = "?";
        if (t0) {
            ty = ((const uint8_t*)t0)[10];
            Il2CppClass* k = g_api.class_from_type(t0);
            if (k) c0 = g_api.class_get_name(k);
        }
        MLOG("[MESH] %s overload #%d: params=%d p0type=0x%02x p0=%s",
             name, ++seen, pc, (unsigned)ty, c0 ? c0 : "?");

        if (pc != nparams) continue;
        // SZARRAY + 元素是 Int32
        if (ty == 0x1d && c0 && strstr(c0, "Int32")) return (void*)m;
    }
    if (seen == 0) MLOG("[MESH] %s: 一个重载都没找到（方法被 strip？）", name);
    return nullptr;
}

bool init_mesh_classes(MeshClasses& mc) {
    mc.meshCls = uo::u_class("UnityEngine", "Mesh");
    if (!mc.meshCls) { MLOG("[MESH] Mesh class not found"); return false; }

    // .ctor 必须只在本类找：find_method 会沿父链，Mesh 自己没无参构造时
    // 会撞到 Object..ctor()，拿父类构造函数初始化 Mesh 是错的（native 侧
    // 初始化路径不一样）。build-55 就停在这里，日志只到 [MESH] elems。
    mc.mCtor = uo::u_method_own(mc.meshCls, ".ctor", 0);
    {
        void* iter = nullptr;
        const MethodInfo* m;
        int idx = 0;
        while ((m = g_api.class_get_methods((Il2CppClass*)mc.meshCls, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (!nm || strcmp(nm, ".ctor") != 0) continue;
            MLOG("[MESH]   meshCtor#%d params=%u %s", idx++,
                 g_api.method_get_param_count(m),
                 (m == mc.mCtor) ? "<== SELECTED" : "");
        }
        if (idx == 0) MLOG("[MESH]   Mesh 本类没有任何 .ctor（被 strip？）");
    }
    mc.mSetVertices     = uo::u_method(mc.meshCls, "set_vertices", 1);
    mc.mSetNormals      = uo::u_method(mc.meshCls, "set_normals", 1);
    mc.mSetUV           = uo::u_method(mc.meshCls, "set_uv", 1);
    mc.mSetTriangles    = uo::u_method(mc.meshCls, "set_triangles", 1);
    mc.mSetSubMeshCount = uo::u_method(mc.meshCls, "set_subMeshCount", 1);
    // SetTriangles 有 int[] 和 List<int> 两个双参重载，得按第一个参数类型筛
    // build-73 实测重载列表：3参(int[],int,bool) / 4参 / 2参(List) / 3参(List) / 4参(List)
    // **没有** 2 参 int[] 版本，所以按 3 参找。
    mc.mSetTrianglesSub = find_int_array_method(mc.meshCls, "SetTriangles", 3);
    mc.mGetSubMeshCount = uo::u_method(mc.meshCls, "get_subMeshCount", 0);
    mc.mSetBoneWeights  = uo::u_method(mc.meshCls, "set_boneWeights", 1);
    mc.mSetBindposes    = uo::u_method(mc.meshCls, "set_bindposes", 1);
    mc.mUpload          = uo::u_method(mc.meshCls, "UploadMeshData", 1);
    mc.mGetVertexCount  = uo::u_method(mc.meshCls, "get_vertexCount", 0);

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
    //
    // 走 object_new + Mesh..ctor()，这是原定 V3 路线的基础，不绕。
    // build-57 实测崩在 ctor 内部（日志停在 step0b 后，无 tombstone），
    // 下面把每一步的状态都打出来，正面定位。
    MLOG("[MESH] step0 object_new(Mesh) class=%p", (void*)mc.meshCls);
    void* mesh = uo::u_new_object(mc.meshCls);
    MLOG("[MESH] step0 done mesh=%p klass=%p", mesh,
         mesh ? *(void**)mesh : nullptr);
    if (!mesh) { MLOG("[MESH] object_new failed"); return false; }

    if (!mc.mCtor) {
        MLOG("[MESH] FATAL: Mesh 无可用 .ctor");
        return false;
    }

    // 直接调 runtime_invoke，把异常和返回值都掰开看：
    // invoke_void 只报有没有异常，这里额外确认它到底进没进去。
    MLOG("[MESH] step0b Mesh..ctor mCtor=%p obj=%p ...", (void*)mc.mCtor, mesh);
    {
        Il2CppException* exc = nullptr;
        Il2CppObject* r = g_api.runtime_invoke((const MethodInfo*)mc.mCtor, mesh, nullptr, &exc);
        if (exc) {
            void* ecl = *(void**)exc;
            const char* en = ecl && g_api.class_get_name
                           ? g_api.class_get_name((Il2CppClass*)ecl) : "?";
            MLOG("[MESH] step0b THREW %s", en ? en : "?");
            return false;
        }
        MLOG("[MESH] step0b done ret=%p", (void*)r);
    }

    // ---- 1.5) 打印 set_vertices 的真实参数类型 ----
    // 崩在第一次灌顶点上，先确认签名是 Vector3[] 还是 Vector3[]&
    {
        void* iter = nullptr;
        const MethodInfo* m = nullptr;
        while ((m = g_api.class_get_methods((Il2CppClass*)mc.meshCls, &iter)) != nullptr) {
            const char* mn = g_api.method_get_name(m);
            if (!mn || strcmp(mn, "set_vertices") != 0) continue;
            uint32_t pc = g_api.method_get_param_count(m);
            const Il2CppType* t = g_api.method_get_param(m, 0);
            Il2CppClass* pcl = t ? g_api.class_from_type(t) : nullptr;
            MLOG("[MESH] set_vertices sig: params=%u p0class='%s'",
                 pc, pcl ? g_api.class_get_name(pcl) : "(null)");
        }
    }

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
        // ---- 先用小批量探一次路 ----
        // 全量 21804 个顶点一次灌进去，崩了都不知道是传参错还是数量问题。
        // 先拿 64 个试，通了再上全量。
        if (mc.mSetVertices) {
            void* probe = uo::u_new_array(mc.elemVec3, 64);
            if (probe) {
                float* pd = (float*)uo::u_array_data(probe);
                for (int k = 0; k < 64; k++) {
                    pd[k * 3 + 0] = (float)k * 0.01f;
                    pd[k * 3 + 1] = 0.0f;
                    pd[k * 3 + 2] = 0.0f;
                }
                // 先把数组自身布局打出来：元素大小不对的话 set_vertices
                // 会按自己的 stride 写，写越界直接信号崩（09:03 那次连 tombstone 都没）
                {
                    Il2CppClass* ec = (Il2CppClass*)mc.elemVec3;
                    MLOG("[MESH] probe arr=%p klass=%p elemCls=%p(%s) len=%zu data=%p elemSz=%d",
                         probe, *(void**)probe, (void*)ec,
                         ec && g_api.class_get_name ? g_api.class_get_name(ec) : "?",
                         (size_t)((Il2CppArray*)probe)->max_length,
                         uo::u_array_data(probe),
                         (ec && g_api.class_array_element_size)
                             ? (int)g_api.class_array_element_size(ec) : -1);
                }
                // 先反问一句：mesh 对象到底活没活。
                // get_vertexCount 是纯读，崩了就是 mesh 本身无效；能返回 0 就说明 mesh
                // 是好的，问题在传参。build-64 实测返回 0，于是罪名落到传参上。
                if (mc.mGetVertexCount) {
                    void* r = uo::u_call(mesh, mc.mGetVertexCount, nullptr);
                    int32_t vc = r ? *(int32_t*)((char*)r + 16) : -12345;
                    MLOG("[MESH] probe: get_vertexCount()=%d", vc);
                }

                // ---- 参数到底是 Vector3[] 还是 Vector3[]& ----
                // 不再猜：直接读 Il2CppType 的 byref 位（+11 字节 bit6）。
                // 值传递（byref=0）：args[0] = 数组对象指针
                // 引用传递（byref=1）：args[0] = 指向数组指针的指针
                bool byref = false;
                {
                    const Il2CppType* pt = nullptr;
                    if (g_api.method_get_param)
                        pt = (const Il2CppType*)g_api.method_get_param(
                                 (const MethodInfo*)mc.mSetVertices, 0);
                    if (pt) {
                        uint8_t tail = ((const uint8_t*)pt)[11];
                        byref = (tail & 0x40) != 0;
                        MLOG("[MESH] set_vertices param0 typeByte=%02x byref=%d",
                             tail, (int)byref);
                    }
                }
                MLOG("[MESH] probe: set_vertices x64...");
                void* pa[1] = { byref ? (void*)&probe : (void*)probe };
                uo::u_call(mesh, mc.mSetVertices, pa);
                MLOG("[MESH] probe: x64 survived");
            }
        }

        MLOG("[MESH] step1/8 uploading vertices nv=%zu arr=%p...", nv, arr);
        // Unity 2018 的 set_vertices 是 set_vertices(Vector3[]&)，
        // runtime_invoke 要的是「指向数组引用的指针」
        bool byrefAll = false;
        {
            const Il2CppType* pt = g_api.method_get_param
                ? (const Il2CppType*)g_api.method_get_param((const MethodInfo*)mc.mSetVertices, 0)
                : nullptr;
            if (pt) byrefAll = ((((const uint8_t*)pt)[11]) & 0x40) != 0;
        }
        void* a[1] = { byrefAll ? (void*)&arr : (void*)arr };
        uo::u_call_void(mesh, mc.mSetVertices, a);
        MLOG("[MESH] step1/8 vertices done");
    }

    // ---- 3) normals ----
    MLOG("[MESH] step2/8 normals...");
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
            uo::u_call_void(mesh, mc.mSetNormals, a);
        }
    }

    // ---- 4) uv ----
    MLOG("[MESH] step3/8 uv...");
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
            uo::u_call_void(mesh, mc.mSetUV, a);
        }
    }

    // ---- 5) triangles：按材质切子网格 ----
    //
    // PMX 的索引数组是按材质顺序排的：材质 0 占前 surfaceCount 个索引，
    // 材质 1 接着往下……所以不用重排数据，只需要知道每段的范围。
    {
        struct Range { int start; int count; };
        std::vector<Range> subs;
        int cursor = 0;
        for (const Material& mt : model.materials) {
            int c = (int)mt.surfaceCount;
            if (c <= 0) continue;
            if (cursor >= (int)nf) break;
            if (cursor + c > (int)nf) c = (int)nf - cursor;   // 防御：越界就截
            subs.push_back({cursor, c});
            cursor += c;
        }
        // 材料表没覆盖到的尾巴，挂到最后一个子网格后面
        if (cursor < (int)nf) {
            if (subs.empty()) subs.push_back({0, (int)nf});
            else subs.back().count += (int)nf - cursor;
        }

        MLOG("[MESH] step4/8 triangles, subs=%zu...", subs.size());

        // ---- 数据校验：三角形索引越界是换 mesh 后崩掉的头号嫌疑 ----
        // PMX 用 0xFFFF 表示“无引用”，如果解析时没转成 -1，就会变成 65535，
        // 远大于 nv(21804) —— Unity 蒙皮时直接踩野内存。
        {
            int32_t maxIdx = -1;
            size_t bad = 0;
            for (int32_t v : model.faceVertex) {
                if (v < 0 || (size_t)v >= nv) bad++;
                else if (v > maxIdx) maxIdx = v;
            }
            MLOG("[MESH] validate tris: nv=%zu nf=%zu maxIndex=%d badIndex=%zu",
                 nv, model.faceVertex.size(), maxIdx, bad);
            for (size_t i = 0; i < subs.size(); i++) {
                if (subs[i].count <= 0)
                    MLOG("[MESH] WARNING submesh %zu count=0 (start=%d)", i, subs[i].start);
            }
        }

        bool multiOk = false;
        if (mc.mSetSubMeshCount && mc.mSetTrianglesSub && subs.size() > 1) {
            int32_t sn = (int32_t)subs.size();
            int32_t before = -1;
            if (mc.mGetSubMeshCount) {
                void* r = uo::u_call(mesh, mc.mGetSubMeshCount, nullptr);
                if (r) before = *(int32_t*)((char*)r + 16);
            }
            void* an[1] = { &sn };
            uo::u_call_void(mesh, mc.mSetSubMeshCount, an);
            if (mc.mGetSubMeshCount) {
                void* r = uo::u_call(mesh, mc.mGetSubMeshCount, nullptr);
                int32_t after = r ? *(int32_t*)((char*)r + 16) : -1;
                MLOG("[MESH] subMeshCount %d -> %d (want %d) %s",
                     before, after, sn, after == sn ? "OK" : "SETTER-FAILED");
            }

            int okCount = 0;
            for (size_t si = 0; si < subs.size(); si++) {
                const Range& r = subs[si];
                void* arr = uo::u_new_array(mc.elemInt, (size_t)r.count);
                if (!arr) continue;
                int32_t* d = (int32_t*)uo::u_array_data(arr);
                for (int k = 0; k < r.count; k++) {
                    d[k] = model.faceVertex[(size_t)(r.start + k)];
                }
                int32_t idx = (int32_t)si;
                // Unity 2018 的实际签名是 SetTriangles(int[], int, bool)：
                //   arr  引用类型 → 传对象指针
                //   idx   int32    → 传地址
                //   calc  bool     → 传地址（uint8）
                uint8_t calcBounds = 1;
                void* a3[3] = { arr, &idx, &calcBounds };
                uo::u_call_void(mesh, mc.mSetTrianglesSub, a3);
                okCount++;
            }
            multiOk = okCount == (int)subs.size();
            MLOG("[MESH] submeshes=%zu set=%d", subs.size(), okCount);
            if (multiOk) out.subMeshes = (int)subs.size();
        }

        if (!multiOk) {
            // 退回单子网格（或者接口缺失）
            void* arr = uo::u_new_array(mc.elemInt, nf);
            if (!arr) { MLOG("[MESH] triangles array alloc failed"); return false; }
            int32_t* d = (int32_t*)uo::u_array_data(arr);
            for (size_t i = 0; i < nf; i++) d[i] = model.faceVertex[i];
            void* a[1] = { arr };
            uo::u_call_void(mesh, mc.mSetTriangles, a);
            out.subMeshes = 1;
            const char* why = !mc.mSetSubMeshCount      ? "set_subMeshCount missing"
                            : !mc.mSetTrianglesSub      ? "SetTriangles(int[],int) missing"
                            : "per-submesh call failed";
            MLOG("[MESH] SUBS FALLBACK: %s (ok=%d/%zu)", why,
                 multiOk ? (int)subs.size() : 0, subs.size());
            MLOG("[MESH] WARNING: only 1 of %zu materials will render (subMeshes=1)",
                 model.materials.size());
        }
    }

    // ---- 6) boneWeights ----
    // Unity BoneWeight 内存布局：int32 boneIndex0..3, float weight0..3（共 32 字节）
    MLOG("[MESH] step6/8 boneWeights nv=%zu...", nv);
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
            uo::u_call_void(mesh, mc.mSetBoneWeights, a);
        }
    } else {
        MLOG("[MESH] set_boneWeights missing, skinning will use single bone");
    }

    // ---- 7) bindposes ----
    // 每根骨骼一个：世界矩阵的逆。MMD 骨骼初始是单位旋转 + 平移，所以逆 = 平移取负
    MLOG("[MESH] step7/8 bindposes nb=%d...", nb);
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
            uo::u_call_void(mesh, mc.mSetBindposes, a);
        }
    } else {
        MLOG("[MESH] set_bindposes missing or no bones");
    }

    // ---- 8) UploadMeshData(false) ----
    // 实测（2026-10-22 frida + /proc/mem）：
    //   · UploadMeshData(true)  → mesh native 顶点缓冲被标记为空、
    //     SMR 的 +0x1d8 建不出来、get_bounds() 返回 0、isVisible=1 也不渲染
    //   · 完全不调              → Unity 渲染时踩不完整的 mesh，加载成功后静默退出（无 tombstone）
    //   · UploadMeshData(false) → 上传 GPU 后释放 CPU 副本，GPU 缓冲完整（ModelMod test4 用法）
    MLOG("[MESH] step8/8 UploadMeshData(false)...");
    if (mc.mUpload) {
        uint8_t keepReadable = 0;   // false：上传后释放 CPU 副本
        void* a[1] = { &keepReadable };
        uo::u_call_void(mesh, mc.mUpload, a);
        MLOG("[MESH] step8/8 UploadMeshData(false) done");
    } else {
        MLOG("[MESH] step8/8 UploadMeshData missing (mesh 可能不完整)");
    }

    out.mesh = mesh;
    out.vertexCount = (int)nv;
    out.indexCount = (int)nf;
    out.boneCount = nb;
    if (out.subMeshes <= 0) out.subMeshes = 1;
    out.ok = true;

    MLOG("[MESH] built ok: verts=%zu tris=%zu bones=%d submeshes=%d mesh=%p",
         nv, nf / 3, nb, out.subMeshes, mesh);
    return true;
}

// =====================================================================
// 最小测试网格（A 方案）
//
// 3 顶点 / 1 三角形 / 权重全绑骨骼 0 / bindposes = 骨骼总数（与 m_Bones 严格对齐）。
// 目的：把「换 mesh 后渲染崩」缩到最小变量。
//   不崩 → 数据规模/子网格结构问题
//   崩   → Unity 内部 m_Skin 缓存不随 mesh 更新，换 mesh 这条路本身不通
// =====================================================================
bool build_simple_mesh(const Skeleton& skel, BuiltMesh& out) {
    out = BuiltMesh{};
    MeshClasses mc;
    if (!init_mesh_classes(mc)) {
        MLOG("[MESH] simple: init classes failed");
        return false;
    }

    void* mesh = uo::u_new_object(mc.meshCls);
    if (!mesh) { MLOG("[MESH] simple: object_new failed"); return false; }
    uo::u_call_void(mesh, mc.mCtor, nullptr);
    MLOG("[MESH] simple: mesh=%p", mesh);

    const int nv = 3;
    const float verts[9] = { -1.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f };
    void* varr = uo::u_new_array(mc.elemVec3, (size_t)nv);
    if (!varr) { MLOG("[MESH] simple: verts alloc failed"); return false; }
    memcpy(uo::u_array_data(varr), verts, sizeof(verts));
    void* av[1] = { varr };
    uo::u_call_void(mesh, mc.mSetVertices, av);
    MLOG("[MESH] simple: vertices=3");

    const float norms[9] = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    void* narr = uo::u_new_array(mc.elemVec3, (size_t)nv);
    if (narr) {
        memcpy(uo::u_array_data(narr), norms, sizeof(norms));
        void* an[1] = { narr };
        uo::u_call_void(mesh, mc.mSetNormals, an);
    }

    const float uvs[6] = { 0, 0,  1, 0,  0.5f, 1 };
    void* uarr = uo::u_new_array(mc.elemVec2, (size_t)nv);
    if (uarr) {
        memcpy(uo::u_array_data(uarr), uvs, sizeof(uvs));
        void* au[1] = { uarr };
        uo::u_call_void(mesh, mc.mSetUV, au);
    }

    int32_t tris[3] = { 0, 1, 2 };
    void* tarr = uo::u_new_array(mc.elemInt, 3);
    if (tarr) {
        memcpy(uo::u_array_data(tarr), tris, sizeof(tris));
        void* at[1] = { tarr };
        uo::u_call_void(mesh, mc.mSetTriangles, at);
    }
    MLOG("[MESH] simple: triangles=1");

    // BoneWeight 布局：int32 boneIndex0..3 在前 16 字节，float weight0..3 在后 16 字节
    char bw[32];
    memset(bw, 0, sizeof(bw));
    *(float*)(bw + 16) = 1.0f;   // weight0 = 1.0, boneIndex0 = 0
    void* barr = uo::u_new_array(mc.elemBoneWeight, (size_t)nv);
    if (barr) {
        char* bd = (char*)uo::u_array_data(barr);
        for (int i = 0; i < nv; i++) memcpy(bd + (size_t)i * 32, bw, 32);
        void* ab[1] = { barr };
        uo::u_call_void(mesh, mc.mSetBoneWeights, ab);
    }
    MLOG("[MESH] simple: boneWeights=%d", nv);

    const int nb = skel.count();
    void* parr = uo::u_new_array(mc.elemMatrix4x4, (size_t)nb);
    if (parr) {
        float* pd = (float*)uo::u_array_data(parr);
        for (int i = 0; i < nb; i++) {
            float* m = pd + (size_t)i * 16;
            memset(m, 0, 16 * sizeof(float));
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            const BoneNode& n = skel.at(i);
            m[12] = -n.worldPos[0];
            m[13] = -n.worldPos[1];
            m[14] = -n.worldPos[2];
        }
        void* ap[1] = { parr };
        uo::u_call_void(mesh, mc.mSetBindposes, ap);
    }
    MLOG("[MESH] simple: bindposes=%d", nb);

    if (mc.mUpload) {
        uint8_t keep = 0;
        void* au[1] = { &keep };
        uo::u_call_void(mesh, mc.mUpload, au);
    }
    MLOG("[MESH] simple: upload done");

    out.mesh = mesh;
    out.vertexCount = nv;
    out.indexCount = 3;
    out.boneCount = nb;
    out.subMeshes = 1;
    out.ok = true;
    return true;
}

}  // namespace mmd
