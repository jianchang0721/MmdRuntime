// =====================================================================
// loader.cpp
// =====================================================================
#include "logsink.h"
#include "loader.h"

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "bone_hook.h"
#include "self_smr.h"
#include "mmd/material.h"
#include "mmd/texture.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define LTAG "MmdLoader"
#define LLOG(...) logsink::Write(__VA_ARGS__)

namespace game {

namespace {
// 加载期间禁用 il2cpp GC。
//
// build-71 实测：第 7 张 2048x2048 贴图（u_new_array 16MB）直接卡死，
// load average 飙到 134。原因是 GC 被触发后要等所有线程到 safepoint，
// 而 UnityMain 正在跑我们的 native tick（不在托管代码里）—— 永远到不了，
// 于是 GC 线程和主线程互相等。
//
// 另一个好处：数组不会被中途回收（之前踩过「缓存 Il2CppArray 几帧后变野指针」）。
struct GcGuard {
    bool on = false;
    GcGuard() {
        if (g_api.gc_disable) { g_api.gc_disable(); on = true; }
    }
    ~GcGuard() {
        if (on && g_api.gc_enable) g_api.gc_enable();
    }
    // 中途恢复 GC。
    // build-74 实测：GC 从加载开始一直禁用，到材质阶段 Shader.Find 直接卡死
    // （只分配不回收，分配器开始等 GC，而 GC 永远等不到 safepoint）。
    // 所以只禁用到「Mesh + 贴图大数组分配」结束，材质/Shader 阶段必须放开。
    void enable_now() {
        if (on && g_api.gc_enable) { g_api.gc_enable(); on = false; }
    }
};

void set_err(LoadResult& out, const char* msg) {
    snprintf(out.error, sizeof(out.error), "%s", msg ? msg : "unknown");
    LLOG("[LOADER] FAILED: %s", out.error);
}

// mmd 层要的是 printf 风格回调，LLOG 是宏传不了函数指针，包一层
void logf_cb(const char* fmt, ...) {
    // 注意：这里必须走 logsink（写文件 + fflush + fsync），
    // 只打 __android_log_vprint 的话，崩溃后文件日志里什么都看不到。
    // build-67 就是因此卡在「mesh 建完然后一片空白」——[TEX] 全进了 logcat，
    // 而 logcat 早被 chatty 限流/滚掉了。
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LLOG("%s", buf);
}
}

bool load_model_into_smr(const mmd::Model& model, const char* pmxPath,
                         Smr& smr, void* roleRoot, LoadResult& out) {
    out = LoadResult{};

    // 整个加载过程禁用 GC（贴图 16MB 数组一分配就会触发，不禁必死锁）
    GcGuard gcGuard;
    LLOG("[LOADER] GC %s for this load", gcGuard.on ? "disabled" : "disable-api-missing");

    if (!smr.native) { set_err(out, "SMR native pointer is null"); return false; }
    if (model.verts.empty() || model.bones.empty()) {
        set_err(out, "PMX model is empty");
        return false;
    }

    // ---- ① hook 必须先装 ----
    if (!bonehook::install()) {
        set_err(out, "bone hook install failed (扩容后析构会崩，直接放弃)");
        return false;
    }
    LLOG("[LOADER] bone hook addr=%p", bonehook::found_address());

    // ---- ② Unity 对象层 ----
    if (!uo::ready() && !uo::init()) {
        set_err(out, "unity objects init failed");
        return false;
    }

    // ---- ③ 骨骼树 ----
    if (!out.skeleton.build(model, roleRoot)) {
        set_err(out, "skeleton build failed");
        return false;
    }
    out.bonesBuilt = out.skeleton.built();
    LLOG("[LOADER] skeleton built=%d roots=%d depth=%d",
         out.bonesBuilt, out.skeleton.roots(), out.skeleton.maxDepth());

    const int nb = out.skeleton.count();
    if (nb <= 0) { set_err(out, "no bones in skeleton"); return false; }

    // ---- ④ Mesh ----
    mmd::BuiltMesh bm;
    // A 方案：true = 用 3 顶点最小网格（定位换 mesh 后崩的原因）
    //          false = 完整 PMX mesh
    const bool kUseSimpleMesh = false;   // C 方案最小验证已过，换回完整 PMX mesh
    bool meshOk = kUseSimpleMesh
                ? mmd::build_simple_mesh(out.skeleton, bm)
                : mmd::build_mesh(model, out.skeleton, bm);
    if (!meshOk) {
        set_err(out, "mesh build failed");
        return false;
    }
    LLOG("[LOADER] mesh mode=%s", kUseSimpleMesh ? "SIMPLE(3 verts)" : "FULL");
    out.mesh = bm.mesh;
    out.verts = bm.vertexCount;
    out.tris = bm.indexCount / 3;
    out.subMeshes = bm.subMeshes;
    LLOG("[LOADER] mesh verts=%d tris=%d bones=%d submeshes=%d",
         out.verts, out.tris, bm.boneCount, bm.subMeshes);

    // ---- ④b 贴图 + 材质 ----
    mmd::TextureSet tex;
    mmd::load_textures(model, pmxPath, tex, logf_cb);
    out.textures = tex.loaded;

    // 大块分配到此结束（Mesh 21804 顶点 + 8 张贴图 16MB 数组）。
    // 赶紧把 GC 放回去，否则后面的 Shader.Find / Material..ctor 必死锁。
    gcGuard.enable_now();
    LLOG("[LOADER] GC restored before materials");

    mmd::MaterialSet mats;
    if (mmd::build_materials(model, tex, mats, logf_cb)) {
        out.materials = mats.created;
        LLOG("[LOADER] materials=%d shader='%s'", mats.created, mats.shaderName);
    } else {
        LLOG("[LOADER] 材质创建失败，模型会是一个默认色块");
    }

    // ---- ⑤ 收集骨骼 instanceID ----
    std::vector<int32_t> ids((size_t)nb, 0);
    int idOk = 0;
    for (int i = 0; i < nb; i++) {
        ids[(size_t)i] = out.skeleton.instance_at(i);
        if (ids[(size_t)i] != 0) idOk++;
    }
    LLOG("[LOADER] instanceIDs %d/%d valid", idOk, nb);
    if (idOk < nb / 2) {
        set_err(out, "too many invalid bone instanceIDs");
        return false;
    }

    // =============================================================
    // C 方案：自建 SMR（build-79 已证实给游戏 SMR 换 mesh 必崩）
    // =============================================================
    const bool kUseSelfSmr = true;
    if (kUseSelfSmr) {
        game::SelfSmr self;
        void** matsPtr = mats.items.empty() ? nullptr : mats.items.data();
        int    matsN   = (int)mats.items.size();
        if (!game::create_self_smr("MMDModel", roleRoot, bm.mesh,
                                   ids.data(), (int)nb,
                                   matsPtr, matsN, self)) {
            set_err(out, "create_self_smr failed");
            return false;
        }
        out.mesh = bm.mesh;
        out.verts = bm.vertexCount;
        out.tris = bm.indexCount / 3;
        out.boneSlots = self.bones;
        out.meshId = self.meshId;
        out.ok = true;
        LLOG("[LOADER] SELF SMR OK: go=%p tr=%p meshId=%d bones=%d mats=%d",
             self.go, self.tr, self.meshId, self.bones, matsN);
        return true;
    }

    // =============================================================
    // 调试二分开关（build-76 还是 SUCCESS 后崩，要分清是扩容还是 mesh）
    //   kSkipExpand    = true  → 不扩 m_Bones，只换 mesh
    //   kSkipSwapMesh  = true  → 只扩容，不换 mesh（当前默认，先测这个）
    // 两个都 false 才是完整流程。
    // =============================================================
    const bool kSkipExpand   = false;
    const bool kSkipSwapMesh = false;   // 恢复完整流程；崩溃后会看到 validate 日志

    // ---- ⑥ 扩容 + 写 m_Bones ----
    if (!kSkipExpand) {
        int written = expand_bones(smr, ids.data(), nb);
        if (written < 0) {
            set_err(out, "expand_bones failed");
            return false;
        }
        out.boneSlots = written;
    } else {
        LLOG("[LOADER] DEBUG skip expand: m_Bones 保持原样");
    }

    // ---- ⑦ 换 m_Mesh ----
    int32_t meshId = 0;
    if (!kSkipSwapMesh) {
        meshId = uo::instance_id(bm.mesh);
        if (meshId == 0) { set_err(out, "mesh instanceID is 0"); return false; }
        if (!set_mesh_instance(smr, meshId)) {
            set_err(out, "set_mesh_instance failed");
            return false;
        }
        out.meshId = meshId;
    } else {
        LLOG("[LOADER] DEBUG skip mesh swap: m_Mesh 保持游戏原值（只验证扩容）");
    }

    // ---- ⑦b 材质 ----
    if (!kSkipSwapMesh && !mats.items.empty()) {
        if (!set_shared_materials(smr, mats.items.data(), (int)mats.items.size())) {
            LLOG("[LOADER] 材质没设上去（共用子网格数 %d），画面会是默认材质", bm.subMeshes);
        }
    }

    // ---- ⑧ 读回校验 ----
    int32_t sz = 0, mid = 0;
    read_back(smr, &sz, &mid);
    LLOG("[LOADER] read back: bones=%d (expect %d) meshId=%d (expect %d)",
         sz, nb, mid, meshId);

    bool readOk = kSkipSwapMesh ? true : (sz == nb && mid == meshId);
    if (!readOk) {
        set_err(out, "read back mismatch");
        return false;
    }

    out.ok = true;
    LLOG("[LOADER] SUCCESS: %d bones, %d verts, %d tris, meshId=%d",
         nb, out.verts, out.tris, meshId);
    return true;
}

}  // namespace game
