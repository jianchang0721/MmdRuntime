// =====================================================================
// skeleton.cpp
//
// 建树分三遍，顺序不能乱：
//   ① 全部 new 出来（不带父级、不设位置）
//   ② 按 parent 索引 set_parent，再设 localPosition
//      顺序反了会被 SetParent 的 worldPositionStays 把位置算飞
//   ③ 记录每根骨骼 Transform 的 instanceID（后面写 m_Bones 用）
// =====================================================================
#include "logsink.h"
#include "skeleton.h"

#include <android/log.h>
#include <cmath>
#include <cstring>

#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define STAG "MmdSkeleton"
#define SLOG(...) logsink::Write(__VA_ARGS__)

namespace mmd {

namespace {
const int kMaxDepthGuard = 64;   // 防循环引用
}

bool Skeleton::build(const Model& model, void* rootParent) {
    const int n = (int)model.bones.size();
    if (n <= 0) {
        SLOG("[SKEL] no bones");
        return false;
    }
    if (!uo::ready() && !uo::init()) {
        SLOG("[SKEL] unity objects not ready");
        return false;
    }

    SLOG("[SKEL] start n=%d sizeof(BoneNode)=%zu total=%zuB",
         n, sizeof(BoneNode), sizeof(BoneNode) * (size_t)n);
    nodes_.clear();
    nodes_.resize((size_t)n);
    SLOG("[SKEL] resized ok, nodes=%zu", nodes_.size());
    built_ = roots_ = maxDepth_ = 0;

    // ---- ① 全部建出来 ----
    for (int i = 0; i < n; i++) {
        const Bone& b = model.bones[(size_t)i];
        BoneNode& node = nodes_[(size_t)i];
        node.pmxIndex = i;
        node.parentPmx = b.parent;
        node.worldPos[0] = b.pos[0];
        node.worldPos[1] = b.pos[1];
        node.worldPos[2] = b.pos[2];
        node.transform = uo::create_node(b.name.empty() ? "bone" : b.name.c_str(),
                                         nullptr, 0.0f, 0.0f, 0.0f);
        node.ok = (node.transform != nullptr);
        // 钉住：骨骼树只被 C++ 静态数组引用，il2cpp GC 不认裸指针。
        // build-75 实测：SUCCESS 之后进程直接没 —— GC 把 Transform 收了，
        // SMR 的 m_Bones 里存着的 instanceID 变成野指针，渲染第一帧就崩。
        if (node.ok && g_api.gchandle_new) {
            node.gcHandle = g_api.gchandle_new((Il2CppObject*)node.transform, true);
        }
        if (node.ok) built_++;
    }

    if (built_ == 0) {
        SLOG("[SKEL] no transform created");
        return false;
    }

    // ---- ② 挂父级 + 设局部位置 ----
    for (int i = 0; i < n; i++) {
        BoneNode& node = nodes_[(size_t)i];
        if (!node.ok) continue;

        int p = node.parentPmx;
        void* parentTr = rootParent;
        float px = 0.0f, py = 0.0f, pz = 0.0f;

        if (p >= 0 && p < n && nodes_[(size_t)p].ok) {
            parentTr = nodes_[(size_t)p].transform;
            px = nodes_[(size_t)p].worldPos[0];
            py = nodes_[(size_t)p].worldPos[1];
            pz = nodes_[(size_t)p].worldPos[2];
        } else {
            roots_++;
        }

        if (parentTr) {
            uo::set_parent(node.transform, parentTr);
        }

        // PMX 存的是模型空间绝对坐标，localPosition 必须减父级绝对值
        uo::set_local_position(node.transform,
                               node.worldPos[0] - px,
                               node.worldPos[1] - py,
                               node.worldPos[2] - pz);
    }

    // ---- ③ instanceID ----
    for (int i = 0; i < n; i++) {
        BoneNode& node = nodes_[(size_t)i];
        if (!node.ok) continue;
        node.instanceId = uo::instance_id(node.transform);
    }

    // ---- 统计层级深度（顺带查出环） ----
    for (int i = 0; i < n; i++) {
        if (!nodes_[(size_t)i].ok) continue;
        int depth = 0;
        int cur = i;
        while (cur >= 0 && cur < n && depth < kMaxDepthGuard) {
            cur = nodes_[(size_t)cur].parentPmx;
            depth++;
        }
        if (depth > maxDepth_) maxDepth_ = depth;
    }

    int idOk = 0;
    for (int i = 0; i < n; i++) if (nodes_[(size_t)i].ok && nodes_[(size_t)i].instanceId != 0) idOk++;

    SLOG("[SKEL] built=%d/%d roots=%d maxDepth=%d instanceIds=%d",
         built_, n, roots_, maxDepth_, idOk);

    // 以前这里写的是 built_ == n，少一根就整个失败，直接把整条加载链卡死。
    // 实际上少数骨骼建不出来只会让对应顶点权重失效（变成原点），
    // 不至于让整个模型上不了屏。八成以上就继续，剩下的看日志定位。
    return built_ >= (n * 8 / 10);
}

void Skeleton::destroy() {
    // 只清引用，GameObject 交给 Unity 场景管理（我们没法安全 Destroy）
    nodes_.clear();
    built_ = roots_ = maxDepth_ = 0;
}

}  // namespace mmd
