// =====================================================================
// skin_cpu.cpp —— CPU 线性混合蒙皮
// =====================================================================
#include "skin_cpu.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>

#include "logsink.h"
#include "mmd/pmx.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define STAG "MmdSkin"
#define SLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {
namespace skin {

namespace {

// ---- 模型侧数据（init 时拷一份，PMX 之后可能被释放）----
int   g_nVerts = 0;
int   g_nBones = 0;

std::vector<float>    g_pos;       // 3 * nVerts，绑定姿态（模型空间）
std::vector<uint8_t>  g_boneIdx;   // 4 * nVerts
std::vector<float>    g_weight;    // 4 * nVerts
std::vector<int32_t>  g_parent;    // nBones
std::vector<float>    g_bindPos;   // 3 * nBones，模型空间绝对坐标
std::vector<float>    g_localPos;  // 3 * nBones，相对父级

// 每帧的骨骼世界旋转（四元数）与世界位置
std::vector<float>    g_worldRot;  // 4 * nBones
std::vector<float>    g_worldPos;  // 3 * nBones
std::vector<uint8_t>  g_hasRot;    // nBones，是否有游戏骨骼驱动

// 蒙皮矩阵：3x4（行主序 12 float），已经是 T(pos)*R*T(-bind)
std::vector<float>    g_skin;      // 12 * nBones

// 输出数组（钉住复用，别每帧分配）
void*   g_arr = nullptr;
uint32_t g_arrHandle = 0;
void*   g_mesh = nullptr;
void*   mSetVertices = nullptr;
void*   mSetNormals  = nullptr;

// ---- 法线（白辉光的根源：只蒙皮位置、不蒙皮法线）----
std::vector<float>    g_norm;      // 3 * nVerts，bind 姿态法线
void*   g_normArr = nullptr;
uint32_t g_normArrHandle = 0;
static bool g_hasNormal = false;

int g_lastUs = 0;
int g_updateCount = 0;

long long now_us() {
    struct timespec ts;
    syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

// ---- 四元数/向量小工具 ----
inline void q_identity(float* q) { q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 1; }

inline void q_normalize(float* q) {
    const float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n < 1e-6f) { q_identity(q); return; }
    const float inv = 1.0f / n;
    q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
}

inline void q_mul(const float* a, const float* b, float* out) {
    // out = a * b
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    out[0] = aw*bx + ax*bw + ay*bz - az*by;
    out[1] = aw*by - ax*bz + ay*bw + az*bx;
    out[2] = aw*bz + ax*by - ay*bx + az*bw;
    out[3] = aw*bw - ax*bx - ay*by - az*bz;
}

inline void q_conj(const float* q, float* out) {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

inline void q_rotate(const float* q, const float* v, float* out) {
    // v' = q * v * q^-1
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float vx = v[0], vy = v[1], vz = v[2];
    // t = 2 * cross(q.xyz, v)
    const float tx = 2.0f * (qy*vz - qz*vy);
    const float ty = 2.0f * (qz*vx - qx*vz);
    const float tz = 2.0f * (qx*vy - qy*vx);
    out[0] = vx + qw*tx + (qy*tz - qz*ty);
    out[1] = vy + qw*ty + (qz*tx - qx*tz);
    out[2] = vz + qw*tz + (qx*ty - qy*tx);
}

// 四元数 → 3x3 行主序
inline void q_to_mat3(const float* q, float* m) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x*x, yy = y*y, zz = z*z;
    const float xy = x*y, xz = x*z, yz = y*z;
    const float wx = w*x, wy = w*y, wz = w*z;
    m[0] = 1 - 2*(yy+zz); m[1] =     2*(xy-wz); m[2] =     2*(xz+wy);
    m[3] =     2*(xy+wz); m[4] = 1 - 2*(xx+zz); m[5] =     2*(yz-wx);
    m[6] =     2*(xz-wy); m[7] =     2*(yz+wx); m[8] = 1 - 2*(xx+yy);
}

}  // namespace

bool ready() { return g_mesh && g_nVerts > 0 && g_nBones > 0; }
int  bone_count() { return g_nBones; }
int  vertex_count() { return g_nVerts; }
int  last_update_us() { return g_lastUs; }

bool init(const mmd::Model& model, void* meshObj) {
    if (!meshObj) { SLOG("[SKIN] mesh is null"); return false; }
    if (model.verts.empty() || model.bones.empty()) { SLOG("[SKIN] empty model"); return false; }

    g_mesh = meshObj;
    g_nVerts = (int)model.verts.size();
    g_nBones = (int)model.bones.size();

    // ---- 法线：必须一起蒙皮，否则角色一动就一块白辉光 ----
    g_norm.assign((size_t)g_nVerts * 3, 0.0f);
    g_hasNormal = false;
    for (int i = 0; i < g_nVerts; i++) {
        const mmd::Vertex& v = model.verts[(size_t)i];
        const float nx = v.nrm[0], ny = v.nrm[1], nz = v.nrm[2];
        const float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > 1e-5f) {
            g_norm[(size_t)i*3+0] = nx / len;
            g_norm[(size_t)i*3+1] = ny / len;
            g_norm[(size_t)i*3+2] = nz / len;
            g_hasNormal = true;
        }
    }

    g_pos.assign((size_t)g_nVerts * 3, 0.0f);
    g_boneIdx.assign((size_t)g_nVerts * 4, 0);
    g_weight.assign((size_t)g_nVerts * 4, 0.0f);
    for (int i = 0; i < g_nVerts; i++) {
        const mmd::Vertex& v = model.verts[(size_t)i];
        g_pos[(size_t)i*3+0] = v.pos[0];
        g_pos[(size_t)i*3+1] = v.pos[1];
        g_pos[(size_t)i*3+2] = v.pos[2];
        for (int k = 0; k < 4; k++) {
            int bi = v.bone[k];
            g_boneIdx[(size_t)i*4+k] = (uint8_t)((bi >= 0 && bi < g_nBones) ? bi : 0);
            g_weight[(size_t)i*4+k]  = (bi >= 0 && bi < g_nBones) ? v.weight[k] : 0.0f;
        }
    }

    g_parent.assign((size_t)g_nBones, -1);
    g_bindPos.assign((size_t)g_nBones * 3, 0.0f);
    g_localPos.assign((size_t)g_nBones * 3, 0.0f);
    for (int i = 0; i < g_nBones; i++) {
        const mmd::Bone& b = model.bones[(size_t)i];
        g_parent[(size_t)i] = b.parent;
        g_bindPos[(size_t)i*3+0] = b.pos[0];
        g_bindPos[(size_t)i*3+1] = b.pos[1];
        g_bindPos[(size_t)i*3+2] = b.pos[2];
        if (b.parent >= 0 && b.parent < g_nBones) {
            g_localPos[(size_t)i*3+0] = b.pos[0] - model.bones[(size_t)b.parent].pos[0];
            g_localPos[(size_t)i*3+1] = b.pos[1] - model.bones[(size_t)b.parent].pos[1];
            g_localPos[(size_t)i*3+2] = b.pos[2] - model.bones[(size_t)b.parent].pos[2];
        } else {
            g_localPos[(size_t)i*3+0] = b.pos[0];
            g_localPos[(size_t)i*3+1] = b.pos[1];
            g_localPos[(size_t)i*3+2] = b.pos[2];
        }
    }

    g_worldRot.assign((size_t)g_nBones * 4, 0.0f);
    g_worldPos.assign((size_t)g_nBones * 3, 0.0f);
    g_hasRot.assign((size_t)g_nBones, 0);
    g_skin.assign((size_t)g_nBones * 12, 0.0f);
    clear_all_rot();

    // 输出数组：只分配一次，gchandle 钉住，之后每帧原地写
    Il2CppClass* vec3 = (Il2CppClass*)uo::u_class("UnityEngine", "Vector3");
    Il2CppClass* meshCls = (Il2CppClass*)uo::u_class("UnityEngine", "Mesh");
    if (!vec3 || !meshCls) { SLOG("[SKIN] class missing"); return false; }

    mSetVertices = uo::u_method(meshCls, "set_vertices", 1);
    mSetNormals  = uo::u_method(meshCls, "set_normals", 1);
    if (!mSetVertices) { SLOG("[SKIN] set_vertices missing"); return false; }
    if (mSetNormals && g_hasNormal) {
        g_normArr = uo::u_new_array(vec3, (size_t)g_nVerts);
        if (g_normArr && g_api.gchandle_new)
            g_normArrHandle = g_api.gchandle_new((Il2CppObject*)g_normArr, true);
    }
    SLOG("[SKIN] normals: src=%d setNormals=%p arr=%p",
         (int)g_hasNormal, (void*)mSetNormals, g_normArr);

    g_arr = uo::u_new_array(vec3, (size_t)g_nVerts);
    if (!g_arr) { SLOG("[SKIN] array alloc failed"); return false; }
    if (g_api.gchandle_new) g_arrHandle = g_api.gchandle_new((Il2CppObject*)g_arr, true);

    SLOG("[SKIN] init ok: verts=%d bones=%d arr=%p handle=%u",
         g_nVerts, g_nBones, g_arr, g_arrHandle);
    return true;
}

void set_bone_world_rot(int boneIndex, const float q[4]) {
    if (boneIndex < 0 || boneIndex >= g_nBones) return;
    float* dst = &g_worldRot[(size_t)boneIndex * 4];
    dst[0] = q[0]; dst[1] = q[1]; dst[2] = q[2]; dst[3] = q[3];
    q_normalize(dst);
    g_hasRot[(size_t)boneIndex] = 1;
}

void clear_all_rot() {
    if (g_nBones <= 0) return;
    for (int i = 0; i < g_nBones; i++) {
        q_identity(&g_worldRot[(size_t)i * 4]);
        g_hasRot[(size_t)i] = 0;
        g_worldPos[(size_t)i*3+0] = g_bindPos[(size_t)i*3+0];
        g_worldPos[(size_t)i*3+1] = g_bindPos[(size_t)i*3+1];
        g_worldPos[(size_t)i*3+2] = g_bindPos[(size_t)i*3+2];
    }
}

void update() {
    if (!ready() || !mSetVertices) return;
    const long long t0 = now_us();

    // ---- 1) 按层级推世界位置（旋转直接用翻译层给的值；没驱动的用父级）----
    for (int i = 0; i < g_nBones; i++) {
        const int p = g_parent[(size_t)i];
        const float* pRot = (p >= 0) ? &g_worldRot[(size_t)p * 4] : nullptr;
        const float* pPos = (p >= 0) ? &g_worldPos[(size_t)p * 3] : nullptr;

        if (!g_hasRot[(size_t)i] && pRot) {
            memcpy(&g_worldRot[(size_t)i * 4], pRot, 4 * sizeof(float));
        }

        const float* local = &g_localPos[(size_t)i * 3];
        float* wp = &g_worldPos[(size_t)i * 3];
        if (pRot && pPos) {
            float rotated[3];
            q_rotate(pRot, local, rotated);
            wp[0] = pPos[0] + rotated[0];
            wp[1] = pPos[1] + rotated[1];
            wp[2] = pPos[2] + rotated[2];
        } else {
            wp[0] = local[0];
            wp[1] = local[1];
            wp[2] = local[2];
        }

        // 蒙皮矩阵 = T(wp) * R * T(-bind)
        float* m = &g_skin[(size_t)i * 12];
        float rot3[9];
        q_to_mat3(&g_worldRot[(size_t)i * 4], rot3);
        const float* bp = &g_bindPos[(size_t)i * 3];
        for (int r = 0; r < 3; r++) {
            m[r*4+0] = rot3[r*3+0];
            m[r*4+1] = rot3[r*3+1];
            m[r*4+2] = rot3[r*3+2];
            m[r*4+3] = wp[r]
                     - (rot3[r*3+0]*bp[0] + rot3[r*3+1]*bp[1] + rot3[r*3+2]*bp[2]);
        }
    }

    // ---- 2) 逐顶点混合 ----
    float* out = (float*)uo::u_array_data(g_arr);
    if (!out) return;
    float* nout = g_normArr ? (float*)uo::u_array_data(g_normArr) : nullptr;

    for (int i = 0; i < g_nVerts; i++) {
        const float* p = &g_pos[(size_t)i * 3];
        const uint8_t* bi = &g_boneIdx[(size_t)i * 4];
        const float* w  = &g_weight[(size_t)i * 4];

        float ax = 0, ay = 0, az = 0;
        float nx = 0, ny = 0, nz = 0;
        const float* np = &g_norm[(size_t)i * 3];
        for (int k = 0; k < 4; k++) {
            const float wk = w[k];
            if (wk <= 0.0f) continue;
            const float* m = &g_skin[(size_t)bi[k] * 12];
            ax += wk * (m[0]*p[0] + m[1]*p[1] + m[2]*p[2]  + m[3]);
            ay += wk * (m[4]*p[0] + m[5]*p[1] + m[6]*p[2]  + m[7]);
            az += wk * (m[8]*p[0] + m[9]*p[1] + m[10]*p[2] + m[11]);
            // 法线必须一起蒙皮：法线留在 bind 姿态的话，光照/描边/球面贴图
            // 全用错方向，角色一动就是一块白辉光（用户实测反馈）。
            // 骨骼只有旋转+平移、无缩放，法线用同一个 3x3 即可。
            if (nout) {
                nx += wk * (m[0]*np[0] + m[1]*np[1] + m[2]*np[2]);
                ny += wk * (m[4]*np[0] + m[5]*np[1] + m[6]*np[2]);
                nz += wk * (m[8]*np[0] + m[9]*np[1] + m[10]*np[2]);
            }
        }
        out[(size_t)i*3+0] = ax;
        out[(size_t)i*3+1] = ay;
        out[(size_t)i*3+2] = az;

        if (nout) {
            const float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len > 1e-5f) { nx /= len; ny /= len; nz /= len; }
            else { nx = np[0]; ny = np[1]; nz = np[2]; }
            nout[(size_t)i*3+0] = nx;
            nout[(size_t)i*3+1] = ny;
            nout[(size_t)i*3+2] = nz;
        }
    }

    void* a[1] = { g_arr };
    uo::u_call_void(g_mesh, mSetVertices, a);

    // 读回验证：mesh 里到底有没有新顶点。
    // 上面那个 v0 是从 CPU 缓冲打的，只能证明"算对了"，不能证明"上传了"。
    // 如果这里读回来还是 bind pose 的值，就说明 set_vertices 没生效（或者
    // 改的根本不是渲染用的那个 mesh）。
    {
        static int vBeat = 0;
        if (++vBeat % 120 == 1) {
            Il2CppClass* meshCls = (Il2CppClass*)uo::u_class("UnityEngine", "Mesh");
            void* mGetV = meshCls ? uo::u_method(meshCls, "get_vertices", 0) : nullptr;
            void* back = mGetV ? uo::u_call(g_mesh, mGetV, nullptr) : nullptr;
            if (back) {
                const int32_t n = *(int32_t*)((char*)back + 24);
                const float* v = (const float*)((char*)back + 32);
                const bool match = (n == g_nVerts) &&
                    fabsf(v[0] - out[0]) < 1e-3f &&
                    fabsf(v[1] - out[1]) < 1e-3f &&
                    fabsf(v[2] - out[2]) < 1e-3f;
                SLOG("[SKIN] mesh readback n=%d v0=(%.3f,%.3f,%.3f) want=(%.3f,%.3f,%.3f)%s",
                     n, v[0], v[1], v[2], out[0], out[1], out[2],
                     match ? " MATCH" : " <-- 没上传到 mesh!");
            } else {
                SLOG("[SKIN] mesh readback: get_vertices 返回 null");
            }
        }
    }

    if (mSetNormals && g_normArr) {
        void* an[1] = { g_normArr };
        uo::u_call_void(g_mesh, mSetNormals, an);

        // 读回验证：法线到底有没有进 mesh（白辉光是否修好的唯一证据）
        static int verifyBeat = 0;
        if (++verifyBeat % 300 == 1) {
            void* mGetNormals = uo::u_method(uo::u_class("UnityEngine", "Mesh"), "get_normals", 0);
            void* back = mGetNormals ? uo::u_call(g_mesh, mGetNormals, nullptr) : nullptr;
            if (back) {
                int32_t n = *(int32_t*)((char*)back + 24);
                float* v = (float*)((char*)back + 32);
                const float* want = (float*)uo::u_array_data(g_normArr);
                const bool same = (n == g_nVerts) && (n > 0) &&
                    (fabsf(v[0] - want[0]) < 1e-4f) &&
                    (fabsf(v[(size_t)(n/2)*3+1] - want[(size_t)(n/2)*3+1]) < 1e-4f);
                SLOG("[SKIN] normals readback: n=%d v[0]=(%.3f,%.3f,%.3f) %s",
                     n, v[0], v[1], v[2], same ? "MATCH" : "<-- 没进去！");
            } else {
                SLOG("[SKIN] normals readback: get_normals 返回 null");
            }
        }
    }

    // 诊断：到底有多少根骨骼被翻译层驱动、第一根的旋转/蒙皮矩阵长什么样。
    // 模型全程 T-pose 的话，这里 driven 会是 0 或者 q 是 identity。
    {
        static int dBeat = 0;
        if (++dBeat % 300 == 1) {
            int driven = 0, first = -1;
            for (int i = 0; i < g_nBones; i++) {
                if (g_hasRot[(size_t)i]) { driven++; if (first < 0) first = i; }
            }
            if (first >= 0) {
                const float* q = &g_worldRot[(size_t)first * 4];
                const float* m = &g_skin[(size_t)first * 12];
                SLOG("[SKIN] driven=%d/%d first=%d q=(%.2f,%.2f,%.2f,%.2f) m=(%.2f,%.2f,%.2f,%.2f)",
                     driven, g_nBones, first, q[0], q[1], q[2], q[3], m[0], m[1], m[2], m[3]);
            } else {
                SLOG("[SKIN] driven=0/%d  一根都没被驱动 —— 模型只能是 T-pose", g_nBones);
            }
        }
    }

    g_lastUs = (int)(now_us() - t0);
    g_updateCount++;
    if (g_updateCount % 120 == 1) {
        // Also print vertex 0 so we can see whether the mesh actually moves.
        SLOG("[SKIN] update #%d verts=%d bones=%d took=%dus v0=(%.3f,%.3f,%.3f)",
             g_updateCount, g_nVerts, g_nBones, g_lastUs,
             out[0], out[1], out[2]);
    }
}

}  // namespace skin
}  // namespace game
