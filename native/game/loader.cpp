// =====================================================================
// loader.cpp
// =====================================================================
#include "logsink.h"
#include "loader.h"

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

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

// 诊断开关文件：游戏私有目录/mmd_diag，内容一个字符
//   0/不存在 = 正常
//   1 = 原角色 mesh + 原角色材质     2 = 我们的 mesh + 原材质
//   3 = 原 mesh + 我们的材质         4 = 我们的 mesh + 我们的材质，但不挂贴图
//   5 = 我们的 mesh + 我们的材质，25 个子网格全用 material[0]
static int read_diag_mode() {
    int fd = open("/data/data/com.hermes.h1game/files/mmd_diag", O_RDONLY);
    if (fd < 0) return 0;
    char c = 0;
    int n = (int)read(fd, &c, 1);
    close(fd);
    if (n != 1 || c < '0' || c > '9') return 0;
    return c - '0';
}

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

    // ---- ① 骨骼容器析构拦截（非必需）----
    // 自建 SMR 的 m_Bones 用 capacity bit0=1 让 Unity 析构跳过 Deallocate，
    // 不依赖 hook。Dobby 在 build-88 已实证走不通，装不上也要继续加载。
    const bool hooked = bonehook::install();
    LLOG("[LOADER] bone hook installed=%d addr=%p (non-fatal)",
         (int)hooked, bonehook::found_address());

    // ---- ② Unity 对象层 ----
    if (!uo::ready() && !uo::init()) {
        set_err(out, "unity objects init failed");
        return false;
    }

    // ---- ②b 模型根节点：挂在游戏角色下，带缩放 ----
    // PMX 模型高约 20 单位，游戏角色约 1.5~2，不缩放就是个巨人。
    // 骨骼树也挂在这个节点下，才能跟着一起缩放；否则骨骼在场景根按 PMX 绝对坐标
    // 摆着、模型缩了骨骼没缩，蒙皮直接错位。
    // 必须与 module.cpp 的 UiCmd::modelScale 默认值一致（0.08），
    // 否则 tick 首帧的 apply 会把这里的缩放覆盖掉。
    const float kModelScale = 0.08f;
    // Model root goes to the SCENE ROOT, not under the character:
    // if we parent it to the game character, the whole model disappears the
    // moment that character gets deactivated (pooled / switched out).
    // module.cpp copies the anchored character's world transform onto it每frame.
    void* modelRoot = uo::create_node("MMDModel", nullptr, 0.0f, 0.0f, 0.0f);
    if (!modelRoot) { set_err(out, "create model root failed"); return false; }
    uo::set_local_scale(modelRoot, kModelScale, kModelScale, kModelScale);
    out.modelRoot = modelRoot;
    LLOG("[LOADER] model root=%p (scene root) roleRoot=%p scale=%.3f",
         modelRoot, roleRoot, kModelScale);

    // ---- ③ 骨骼树（挂到模型根下）----
    if (!out.skeleton.build(model, modelRoot)) {
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

    // 钉住 mesh：m_Mesh 里存的是 instanceID，GC 把 mesh 收了就是悬空 id，
    // Unity 找不到 mesh -> 不建顶点缓存 -> 不渲染（build-110 实测 +0x1d8 一直是 0）。
    if (g_api.gchandle_new && bm.mesh) {
        uint32_t h = g_api.gchandle_new((Il2CppObject*)bm.mesh, true);
        if (h) out.pinHandles.push_back(h);
        LLOG("[LOADER] mesh pinned handle=%u", h);
    }
    LLOG("[LOADER] mesh verts=%d tris=%d bones=%d submeshes=%d",
         out.verts, out.tris, bm.boneCount, bm.subMeshes);

    // ===== 子网格分段校验（贴图错位的头号嫌疑人）=====
    // 每段索引算包围盒：腿袜应该在下、首应该在上。
    // 如果每段的 y 范围都跨整个身体，说明 surfaceCount 分段和索引顺序对不上，
    // 那就是「每个子网格贴了别人的贴图」的直接原因。
    {
        int cursor = 0;
        const int nfAll = (int)model.faceVertex.size();
        LLOG("[SUBS] 每段索引的包围盒（mat / tex / 三角数 / y 范围 / x 范围）:");
        for (size_t i = 0; i < model.materials.size() && i < 32; i++) {
            const mmd::Material& mt = model.materials[i];
            int c = (int)mt.surfaceCount;
            if (c <= 0) { LLOG("[SUBS] mat[%zu] '%s' surf=0 跳过", i, mt.name.c_str()); continue; }
            const int start = cursor;
            if (cursor + c > nfAll) c = nfAll - cursor;
            float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
            int used = 0;
            for (int k = 0; k < c; k++) {
                const int idx = model.faceVertex[(size_t)(start + k)];
                if (idx < 0 || (size_t)idx >= model.verts.size()) continue;
                const mmd::Vertex& v = model.verts[(size_t)idx];
                if (v.pos[1] < ymin) ymin = v.pos[1];
                if (v.pos[1] > ymax) ymax = v.pos[1];
                if (v.pos[0] < xmin) xmin = v.pos[0];
                if (v.pos[0] > xmax) xmax = v.pos[0];
                used++;
            }
            LLOG("[SUBS] mat[%zu] %-10s tex=%2d tris=%5d y=[%6.2f..%6.2f] x=[%6.2f..%6.2f] v=%d",
                 i, mt.name.c_str(), mt.texIndex, c / 3, ymin, ymax, xmin, xmax, used);
            cursor += c;
        }
        LLOG("[SUBS] 索引总数=%d 游标走到=%d %s", nfAll, cursor,
             cursor == nfAll ? "(对得上)" : "<-- 对不上！分段算错了");
    }

    // ---- ④b 贴图 + 材质 ----
    const int diagMode = read_diag_mode();
    if (diagMode) LLOG("[DIAG] mode=%d", diagMode);

    mmd::TextureSet tex;
    mmd::load_textures(model, pmxPath, tex, logf_cb);
    out.textures = tex.loaded;

    // mode 4：材质不挂贴图，单独验证是不是 Texture2D 把渲染命令流喂坏的
    if (diagMode == 4) {
        LLOG("[DIAG] 跳过贴图绑定（清空 tex.items，纹理已解码但不用）");
        tex.items.clear();
    }

    // mmd_checker：全部材质改用棋盘格贴图，专门验 UV
    // （方格贴着表面大小均匀 = UV 正确；大小乱跳/撕裂 = UV 错）
    if (access("/data/data/com.hermes.h1game/files/mmd_checker", F_OK) == 0) {
        void* ck = mmd::make_checker_texture(256, 256, 16, logf_cb);
        if (ck) {
            tex.items.assign(model.textures.size(), ck);
            if (g_api.gchandle_new) g_api.gchandle_new((Il2CppObject*)ck, true);
            LLOG("[DIAG] 棋盘格贴图已铺到全部 %zu 个贴图槽（验证 UV）", tex.items.size());
        } else {
            LLOG("[DIAG] 棋盘格贴图创建失败");
        }
    }

    // 贴图必须钉住：材质里存的是 native PPtr，Texture2D 被 GC 收掉后
    // native 纹理就释放了，渲染线程下一帧解引用直接 SIGSEGV。
    // build-121 实测：MeshRenderer 真渲染之后，加载完 5~20 秒必崩（SIGSEGV），
    // 旧版 SMR 不渲染所以从没踩到——这是真渲染之后才暴露的悬空引用。
    if (g_api.gchandle_new) {
        int pinned = 0;
        for (void* tp : tex.items) {
            if (!tp) continue;
            uint32_t h = g_api.gchandle_new((Il2CppObject*)tp, true);
            if (h) { out.pinHandles.push_back(h); pinned++; }
        }
        LLOG("[LOADER] textures pinned: %d / %zu", pinned, tex.items.size());
    }

    // 大块分配到此结束（Mesh 21804 顶点 + 8 张贴图 16MB 数组）。
    // 赶紧把 GC 放回去，否则后面的 Shader.Find / Material..ctor 必死锁。
    gcGuard.enable_now();
    LLOG("[LOADER] GC restored before materials");

    // 材质侧诊断开关（6/7/8 见 material.cpp 顶部注释）
    mmd::set_material_mode((diagMode >= 6 && diagMode <= 8) ? diagMode : 0);

    // 【默认回到游戏原材质 shader】
    // build-138 实测：Shader.Find 拿到的内置 shader（Mobile/Diffuse /
    // Unlit/Texture）一渲染就崩在同一地址 pc=libunity+0x826df8、
    // fault 恒定 0x6800600000001 —— 说明游戏打包时把这些内置 shader 的
    // pass 全 strip 了，对象存在但没有可用 pass。
    // 只有游戏自己的 shader 是完整编译过的，只能用它。
    // 想试别的就在 mmd_shader 文件里写名字。
    if (true) {
        void* rCls = uo::u_class("UnityEngine", "Renderer");
        void* mGetMat = rCls ? uo::u_method(rCls, "get_sharedMaterial", 0) : nullptr;
        void* refMat = mGetMat ? uo::u_call(smr.managed, mGetMat, nullptr) : nullptr;
        void* matCls = uo::u_class("UnityEngine", "Material");
        void* mGetShader = matCls ? uo::u_method(matCls, "get_shader", 0) : nullptr;
        void* sh = (refMat && mGetShader) ? uo::u_call(refMat, mGetShader, nullptr) : nullptr;
        if (sh) { mmd::set_shader_override(sh); LLOG("[MAT] 默认用游戏原材质 shader=%p", sh); }
        else    { LLOG("[MAT] 拿不到游戏 shader，退回 Shader.Find 链"); }

        // 诊断：把 shader 的属性表全打出来。
        // 「白辉光 + 人物发黑」通常是 shader 要的某个数据我们没给：
        // 描边宽度 / 法线贴图 / 自发光 / 金属度。先看清楚它有哪些属性。
        if (sh) {
            Il2CppClass* shCls = (Il2CppClass*)uo::u_class("UnityEngine", "Shader");
            void* mCnt  = shCls ? uo::u_method(shCls, "GetPropertyCount", 0) : nullptr;
            void* mName = shCls ? uo::u_method_typed(shCls, "GetPropertyName", 1, "Int32") : nullptr;
            void* mType = shCls ? uo::u_method_typed(shCls, "GetPropertyType", 1, "Int32") : nullptr;
            if (mCnt) {
                void* r = uo::u_call(sh, mCnt, nullptr);
                const int tn = r ? *(int32_t*)((char*)r + 16) : 0;
                LLOG("[MAT] shader 属性 %d 个", tn);
                for (int i = 0; i < tn && i < 64; i++) {
                    int32_t idx = i;
                    void* a[1] = { &idx };
                    char nm[96] = {0};
                    void* str = mName ? uo::u_call(sh, mName, a) : nullptr;
                    if (str) {
                        const int32_t len = *(int32_t*)((char*)str + 16);
                        const uint16_t* ch = (const uint16_t*)((char*)str + 20);
                        int k = 0;
                        for (int32_t j = 0; j < len && k < (int)sizeof(nm) - 1; j++)
                            nm[k++] = (ch[j] < 0x80) ? (char)ch[j] : '?';
                        nm[k] = 0;
                    }
                    int ty = -1;
                    if (mType) {
                        void* rt = uo::u_call(sh, mType, a);
                        if (rt) ty = *(int32_t*)((char*)rt + 16);
                    }
                    LLOG("[MAT]   [%d] '%s' type=%d", i, nm, ty);
                }
            }
        }
    }

    // mmd_shader：文件里写 shader 名字（如 Mobile/Diffuse），用它做实验，
    // 不用重编就能换 shader。空文件/不存在 = 用游戏原材质 shader。
    {
        char want[64] = {0};
        int fd = open("/data/data/com.hermes.h1game/files/mmd_shader", O_RDONLY);
        if (fd >= 0) {
            int n = (int)read(fd, want, sizeof(want) - 1);
            close(fd);
            if (n > 0) {
                for (int i = 0; i < n; i++) if (want[i] == '\n' || want[i] == '\r') want[i] = 0;
                void* shCls = uo::u_class("UnityEngine", "Shader");
                void* mFind = shCls ? uo::u_method(shCls, "Find", 1) : nullptr;
                Il2CppString* s = g_api.string_new(want);
                void* args[1] = { s };
                void* sh = (mFind && s) ? uo::u_call(nullptr, mFind, args) : nullptr;
                if (sh) { mmd::set_shader_override(sh); LLOG("[DIAG] 指定 shader '%s' -> %p", want, sh); }
                else    { LLOG("[DIAG] Shader.Find('%s') 没找到，保持游戏 shader", want); }
            }
        }
    }

    // 诊断模式开关：10 = 单子网格（验证子网格/材质配对）
    if (diagMode == 10) mmd::set_single_submesh(true);

    if (diagMode == 9) {
        void* rCls = uo::u_class("UnityEngine", "Renderer");
        void* mGetMat = rCls ? uo::u_method(rCls, "get_sharedMaterial", 0) : nullptr;
        void* refMat = mGetMat ? uo::u_call(smr.managed, mGetMat, nullptr) : nullptr;
        void* matCls = uo::u_class("UnityEngine", "Material");
        void* mGetShader = matCls ? uo::u_method(matCls, "get_shader", 0) : nullptr;
        void* sh = (refMat && mGetShader) ? uo::u_call(refMat, mGetShader, nullptr) : nullptr;
        if (sh) { mmd::set_shader_override(sh); LLOG("[DIAG] mode=9 用原材质 shader=%p", sh); }
        else    { LLOG("[DIAG] mode=9 拿不到原 shader，退回 Shader.Find"); }
    }
    if (diagMode >= 6) LLOG("[DIAG] 材质模式=%d", diagMode);

    mmd::MaterialSet mats;
    if (mmd::build_materials(model, tex, mats, logf_cb)) {
        out.materials = mats.created;
        LLOG("[LOADER] materials=%d shader='%s'", mats.created, mats.shaderName);
        // 材质同样钉住（SMR 的 sharedMaterials 只是 managed 引用，native 侧
        // 的 m_Materials 是 PPtr，GC 收了就渲染不出）
        if (g_api.gchandle_new) {
            for (void* mp : mats.items) {
                if (!mp) continue;
                uint32_t h = g_api.gchandle_new((Il2CppObject*)mp, true);
                if (h) out.pinHandles.push_back(h);
            }
            LLOG("[LOADER] materials pinned: %zu handles", out.pinHandles.size());
        }
    } else {
        LLOG("[LOADER] 材质创建失败，模型会是一个默认色块");
    }

    // ---- ⑤ 收集骨骼 native Transform*（8 字节/元素）----
    // native m_Bones 存的是 Transform*，不是 4 字节 instanceID。
    // managed 对象 +16 就是 m_CachedPtr（native Transform*）。
    std::vector<void*> bonePtrs((size_t)nb, nullptr);
    // ids 只给 kUseSelfSmr=false 的旧换 mesh 路径用（当前不跑，保留以便回退）
    std::vector<int32_t> ids((size_t)nb, 0);
    int idOk = 0;
    for (int i = 0; i < nb; i++) {
        ids[(size_t)i] = out.skeleton.instance_at(i);
        void* tr = out.skeleton.transform_at(i);
        void* nat = tr ? *(void**)((char*)tr + 16) : nullptr;
        bonePtrs[(size_t)i] = nat;
        if (nat) idOk++;
    }
    LLOG("[LOADER] native bone ptrs %d/%d valid", idOk, nb);
    if (idOk < nb / 2) {
        set_err(out, "too many invalid bone native pointers");
        return false;
    }

    // =============================================================
    // D 方案（2026-10-22 起先试）：MeshFilter + MeshRenderer
    //
    // 为什么要它：自建 SMR 的 m_Mesh/m_Bones/材质全写对、get_sharedMesh
    // 读回非空、isVisible=1，但 +0x1d8（蒙皮缓存）永远为 0、Renderer bounds
    // 恒 0 —— Unity 的 SMR 只在自己的 SetMesh 路径里建蒙皮缓存，直写字段
    // 绕过了那一步。MeshRenderer 没有蒙皮，顶点走 Unity 标准上传路径。
    // 这条路只负责「把模型画出来」；蒙皮/翻译层后面再接。
    // 失败就继续往下走 C 方案（自建 SMR），不丢功能。
    // =============================================================
    {
        std::vector<void*> matsOk;
        for (void* mp : mats.items) if (mp) matsOk.push_back(mp);
        if (!matsOk.empty() && matsOk.size() < (size_t)bm.subMeshes)
            matsOk.resize((size_t)bm.subMeshes, matsOk[0]);
        LLOG("[LOADER] materials: %zu 有效 / %zu 原始 (submesh=%d)",
             matsOk.size(), mats.items.size(), bm.subMeshes);

        // ===== 诊断二分开关（文件：游戏私有目录/mmd_diag，内容 1~3）=====
        //   1 = 用游戏原角色的 mesh + 原角色的材质（完全不用我们的）
        //   2 = 我们的 mesh + 原角色的材质
        //   3 = 原角色的 mesh + 我们的材质
        // 目的：崩溃在 libunity 渲染命令流里（ldrb [x20]，野指针），
        // 先分清是「我们建的网格」还是「我们建的材质」把命令流喂坏了。
        void* useMesh = bm.mesh;
        std::vector<void*> useMats = matsOk;
        const int diag = diagMode;
        // mode 5：25 个子网格全用 material[0]
        if (diag == 5 && !matsOk.empty()) {
            useMats.assign((size_t)bm.subMeshes, matsOk[0]);
            LLOG("[DIAG] mode=5：%d 个子网格全用 material[0]=%p", bm.subMeshes, matsOk[0]);
        }
        if (diag) {
            LLOG("[DIAG] mode=%d（1=refmesh+refmat 2=ourmesh+refmat 3=refmesh+ourmat）", diag);
            if (diag == 1 || diag == 3) {
                void* smrCls = uo::u_class("UnityEngine", "SkinnedMeshRenderer");
                void* mGetMesh = smrCls ? uo::u_method(smrCls, "get_sharedMesh", 0) : nullptr;
                void* rm = mGetMesh ? uo::u_call(smr.managed, mGetMesh, nullptr) : nullptr;
                if (rm) { useMesh = rm; LLOG("[DIAG] mesh <- ref %p", rm); }
                else    LLOG("[DIAG] ref mesh 拿不到，保持我们的");
            }
            if (diag == 1 || diag == 2) {
                void* rCls = uo::u_class("UnityEngine", "Renderer");
                void* mGetMats = rCls ? uo::u_method(rCls, "get_sharedMaterials", 0) : nullptr;
                void* arr = mGetMats ? uo::u_call(smr.managed, mGetMats, nullptr) : nullptr;
                if (arr) {
                    int32_t n = *(int32_t*)((char*)arr + 24);
                    if (n > 0 && n <= 64) {
                        useMats.clear();
                        for (int i = 0; i < n; i++)
                            useMats.push_back(*(void**)((char*)arr + 32 + (size_t)i * 8));
                        LLOG("[DIAG] materials <- ref x%d", n);
                    }
                } else {
                    LLOG("[DIAG] ref materials 拿不到，保持我们的");
                }
            }
        }

        game::SelfSmr self;
        if (game::create_self_mesh_renderer(modelRoot, smr.managed, useMesh,
                                            useMats.empty() ? nullptr : useMats.data(),
                                            (int)useMats.size(), self)) {
            out.mesh      = bm.mesh;
            out.verts     = bm.vertexCount;
            out.tris      = bm.indexCount / 3;
            out.subMeshes = bm.subMeshes;
            out.boneSlots = nb;
            out.meshId    = uo::instance_id(bm.mesh);
            out.ok        = true;
            LLOG("[LOADER] MR 路径 OK: go=%p meshId=%d layer=%d verts=%d tris=%d subs=%d",
                 self.go, out.meshId, self.layer, out.verts, out.tris, out.subMeshes);
            return true;
        }
        LLOG("[LOADER] MR 路径失败，回退自建 SMR（原因见上面的 [MR] 日志）");
    }

    // =============================================================
    // C 方案：自建 SMR（build-79 已证实给游戏 SMR 换 mesh 必崩）
    // =============================================================
    const bool kUseSelfSmr = true;
    if (kUseSelfSmr) {
        game::SelfSmr self;
        // 材质数组里可能有 nullptr（该材质构造失败）。null 材质塞给 SMR 会让
        // 对应子网格渲染不出来、甚至在渲染线程解引用空指针，先过滤；
        // 数量不足 submesh 数时用第一个有效材质补齐。
        std::vector<void*> matsOk;
        for (void* mp : mats.items) if (mp) matsOk.push_back(mp);
        if (!matsOk.empty() && matsOk.size() < (size_t)bm.subMeshes) {
            matsOk.resize((size_t)bm.subMeshes, matsOk[0]);
        }
        void** matsPtr = matsOk.empty() ? nullptr : matsOk.data();
        int    matsN   = (int)matsOk.size();
        LLOG("[LOADER] materials to SMR: %d (raw %zu, null filtered)",
             matsN, mats.items.size());
        if (!game::create_self_smr(modelRoot, smr.managed, bm.mesh,
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
