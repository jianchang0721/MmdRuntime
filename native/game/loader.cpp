// =====================================================================
// loader.cpp
// =====================================================================
#include "loader.h"

#include <android/log.h>
#include <cstdio>
#include <vector>

#include "bone_hook.h"
#include "unity/unity_objects.h"

#define LTAG "MmdLoader"
#define LLOG(...) __android_log_print(ANDROID_LOG_INFO, LTAG, __VA_ARGS__)

namespace game {

namespace {
void set_err(LoadResult& out, const char* msg) {
    snprintf(out.error, sizeof(out.error), "%s", msg ? msg : "unknown");
    LLOG("[LOADER] FAILED: %s", out.error);
}
}

bool load_model_into_smr(const mmd::Model& model, Smr& smr, void* roleRoot, LoadResult& out) {
    out = LoadResult{};

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
    if (!mmd::build_mesh(model, out.skeleton, bm)) {
        set_err(out, "mesh build failed");
        return false;
    }
    out.mesh = bm.mesh;
    out.verts = bm.vertexCount;
    out.tris = bm.indexCount / 3;
    LLOG("[LOADER] mesh verts=%d tris=%d bones=%d", out.verts, out.tris, bm.boneCount);

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

    // ---- ⑥ 扩容 + 写 m_Bones ----
    int written = expand_bones(smr, ids.data(), nb);
    if (written < 0) {
        set_err(out, "expand_bones failed");
        return false;
    }
    out.boneSlots = written;

    // ---- ⑦ 换 m_Mesh ----
    int32_t meshId = uo::instance_id(bm.mesh);
    if (meshId == 0) { set_err(out, "mesh instanceID is 0"); return false; }
    if (!set_mesh_instance(smr, meshId)) {
        set_err(out, "set_mesh_instance failed");
        return false;
    }
    out.meshId = meshId;

    // ---- ⑧ 读回校验 ----
    int32_t sz = 0, mid = 0;
    read_back(smr, &sz, &mid);
    LLOG("[LOADER] read back: bones=%d (expect %d) meshId=%d (expect %d)",
         sz, nb, mid, meshId);

    if (sz != nb || mid != meshId) {
        set_err(out, "read back mismatch");
        return false;
    }

    out.ok = true;
    LLOG("[LOADER] SUCCESS: %d bones, %d verts, %d tris, meshId=%d",
         nb, out.verts, out.tris, meshId);
    return true;
}

}  // namespace game
