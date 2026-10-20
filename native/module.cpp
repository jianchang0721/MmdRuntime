// =====================================================================
// module.cpp —— MmdRuntime v0.2
//
// 本版任务（一次拿全量答案）：
//   1. PMX 解析统计（回归：21804 / 82965 / 8 / 25 / 224）
//   2. 绑定 il2cpp（maps + 手工 .dynsym）
//   3. 找到场景里所有 SkinnedMeshRenderer
//   4. 验证 libunity 侧字段偏移（纯读，不改）：
//        native+0x1d4  ← m_Mesh 的 instanceID
//        native+0x1e0  ← m_Bones.data
//        native+0x1f0  ← m_Bones.size
//      做法：读出来的值和 Object.GetInstanceID(sharedMesh) / get_bones().Length 对比
//   5. 写入通道实验（改完立刻还原）：
//      两个 SMR 的 m_Mesh instanceID 对调，再调 get_sharedMesh() 读回比对
//
// 全程不发一次崩溃，任何一个环节失败就停在那一步并打日志。
// =====================================================================
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <android/log.h>
#include <string>
#include <vector>

#include "mmd/pmx.h"
#include "bone_hook.h"
#include "unity/il2cpp_api.h"

#define TAG "MmdRuntime"

// il2cpp_api.h 里是 extern 声明，这里给定义
Il2CppApi g_api;

// =====================================================================
// 日志
// =====================================================================
static std::string g_dir = "/data/data/com.hermes.h1game/files";
static FILE* g_fp = nullptr;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static void pick_dir() {
    const char* cands[] = {
        "/storage/emulated/0/MT2/log",
        "/sdcard/MT2/log",
        "/data/data/com.hermes.h1game/files",
    };
    for (int i = 0; i < 3; i++) {
        std::string probe = std::string(cands[i]) + "/.mmdrtest";
        FILE* f = fopen(probe.c_str(), "w");
        if (f) { fclose(f); unlink(probe.c_str()); g_dir = cands[i]; break; }
    }
}

static FILE* logfp() {
    if (g_fp) return g_fp;
    std::string p = g_dir + "/mmdruntime.log";
    g_fp = fopen(p.c_str(), "a");
    if (!g_fp && g_dir != "/data/data/com.hermes.h1game/files") {
        g_dir = "/data/data/com.hermes.h1game/files";
        g_fp = fopen((g_dir + "/mmdruntime.log").c_str(), "a");
    }
    return g_fp;
}

static void L(const char* fmt, ...) {
    pthread_mutex_lock(&g_mtx);
    FILE* f = logfp();
    if (f) {
        time_t t = time(nullptr);
        struct tm tmv; localtime_r(&t, &tmv);
        fprintf(f, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }
    va_list ap2; va_start(ap2, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap2);
    va_end(ap2);
    pthread_mutex_unlock(&g_mtx);
}

#define LERR(...) L(__VA_ARGS__)

// =====================================================================
// il2cpp 绑定：不能用 dlopen，从 /proc/self/maps 拿 base，手工解析 .dynsym
// =====================================================================
static uintptr_t maps_base(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) { LERR("[IL2CPP] cannot open /proc/self/maps"); return 0; }
    // 路径可能很长（/data/app/~~xxx==/com.hermes.h1game-xxx==/lib/arm64/libil2cpp.so）
    static char line[2048];
    uintptr_t addrs[128];
    int n = 0;
    int totalLines = 0, hintLines = 0;
    while (fgets(line, sizeof(line), f)) {
        totalLines++;
        if (!strstr(line, "il2cpp")) { }
        if (!strstr(line, soname)) continue;
        hintLines++;
        uintptr_t start = strtoull(line, nullptr, 16);
        if (n < 128) addrs[n++] = start;
    }
    fclose(f);
    static int lastLog = -1;
    if (n != lastLog) {
        lastLog = n;
        L("[IL2CPP] maps: %d lines, %d match '%s'", totalLines, hintLines, soname);
    }
    (void)hintLines;
    uintptr_t lowest = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)addrs[i];
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return addrs[i];
        if (!lowest || addrs[i] < lowest) lowest = addrs[i];
    }
    return lowest;
}

static uintptr_t elf_sym(uintptr_t base, const char* name) {
    if (!base) return 0;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;

    uintptr_t dyn = 0;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = base + ph[i].p_vaddr; break; }
    }
    if (!dyn) return 0;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    for (ElfW(Dyn)* d = (ElfW(Dyn)*)dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab = d->d_un.d_ptr; break;
            case DT_HASH:     hash   = d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu    = d->d_un.d_ptr; break;
            default: break;
        }
    }
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    if (hash && hash < base) hash += base;
    if (gnu && gnu < base) gnu += base;
    if (!symtab || !strtab) return 0;

    uint32_t nsym = 0;
    if (hash) {
        nsym = ((uint32_t*)hash)[1];
    } else if (gnu) {
        uint32_t* g = (uint32_t*)gnu;
        uint32_t nbuckets = g[0], symoffset = g[1], bloom = g[2];
        uint32_t* buckets = &g[4 + bloom * 2];
        uint32_t* chain = &buckets[nbuckets];
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= symoffset) {
            while (!(chain[last - symoffset] & 1)) last++;
            nsym = last + 1;
        }
    }
    if (!nsym) return 0;
    if (nsym > 200000) nsym = 200000;

    ElfW(Sym)* syms = (ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    size_t nameLen = strlen(name);
    for (uint32_t i = 0; i < nsym; i++) {
        uint32_t off = syms[i].st_name;
        if (off == 0 || off > 0xFFFFFF) continue;
        const char* s = strs + off;
        size_t k = 0;
        while (k <= nameLen && s[k] == name[k]) k++;
        if (k == nameLen + 1) return base + syms[i].st_value;
    }
    return 0;
}

static bool g_bound = false;
static uintptr_t g_libunity = 0;

static bool bind_api() {
    uintptr_t base = maps_base("libil2cpp.so");
    if (!base) return false;   // 调用方负责重试，不在这里刷屏
    g_libunity = maps_base("libunity.so");
    L("[IL2CPP] il2cpp base=%p  unity base=%p", (void*)base, (void*)g_libunity);

    int missing = 0;
    #define B(field, sym) g_api.field = (fn_##field)elf_sym(base, sym); \
        if (!g_api.field) { missing++; LERR("[IL2CPP] MISSING %s", sym); }
    B(domain_get,               "il2cpp_domain_get");
    B(domain_get_assemblies,    "il2cpp_domain_get_assemblies");
    B(thread_attach,            "il2cpp_thread_attach");
    B(assembly_get_image,       "il2cpp_assembly_get_image");
    B(image_get_name,           "il2cpp_image_get_name");
    B(class_from_name,          "il2cpp_class_from_name");
    B(class_get_name,           "il2cpp_class_get_name");
    B(class_get_namespace,      "il2cpp_class_get_namespace");
    B(class_get_parent,         "il2cpp_class_get_parent");
    B(class_get_type,           "il2cpp_class_get_type");
    B(class_get_methods,        "il2cpp_class_get_methods");
    B(method_get_name,          "il2cpp_method_get_name");
    B(method_get_param_count,   "il2cpp_method_get_param_count");
    B(runtime_invoke,           "il2cpp_runtime_invoke");
    B(string_new,               "il2cpp_string_new");
    B(object_get_class,         "il2cpp_object_get_class");
    B(type_get_object,          "il2cpp_type_get_object");
    #undef B

    L("[IL2CPP] resolved, missing=%d", missing);
    {
        char cmd[256] = {0};
        FILE* cf = fopen("/proc/self/cmdline", "rb");
        if (cf) { fread(cmd, 1, sizeof(cmd) - 1, cf); fclose(cf); }
        L("[IL2CPP] pid=%d cmdline='%s'", getpid(), cmd);
    }
    if (!g_api.domain_get || !g_api.class_from_name || !g_api.runtime_invoke) {
        LERR("[IL2CPP] core api missing");
        return false;
    }
    // 不再调 il2cpp_thread_attach。
    // 实测（2026-10-20）：在主线程重复 attach 会让 Unity 2018.4 直接 abort()，
    // crash buffer 里是 SIGABRT + libc.abort，日志停在 "pid=..." 之后、"bind ok" 之前。
    // tick 由 Java Handler 每 16ms 在主线程调，那个线程本来就是 Unity 主线程，
    // 已经在 il2cpp 域里，这步纯属多余。
    g_bound = true;
    L("[IL2CPP] bind ok");
    return true;
}

// =====================================================================
// il2cpp 工具
// =====================================================================
static Il2CppClass* find_class_any(const char* ns, const char* name) {
    Il2CppDomain* d = g_api.domain_get();
    if (!d) return nullptr;
    size_t n = 0;
    const Il2CppAssembly** as = g_api.domain_get_assemblies(d, &n);
    for (size_t i = 0; i < n; i++) {
        Il2CppImage* img = g_api.assembly_get_image(as[i]);
        if (!img) continue;
        Il2CppClass* k = g_api.class_from_name(img, ns, name);
        if (k) return k;
    }
    return nullptr;
}

static const MethodInfo* find_method_any(Il2CppClass* klass, const char* name) {
    if (!klass) return nullptr;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent(k)) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (nm && strcmp(nm, name) == 0) return m;
        }
    }
    return nullptr;
}

static std::string to_utf8(Il2CppString* s) {
    if (!s) return "";
    char* p = (char*)s;
    int32_t len = *(int32_t*)(p + 16);
    const uint16_t* ch = (const uint16_t*)(p + 20);
    std::string out;
    out.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = ch[i];
        if (c < 0x80) out.push_back((char)c);
        else if (c < 0x800) {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (c >> 12)));
            out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// 调实例/静态方法。obj 为 nullptr 表示静态
static Il2CppObject* invoke(const MethodInfo* m, void* obj, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, obj, args, &exc);
    if (exc) {
        // 异常不致命，返回 nullptr 让上层跳过
        void* cl = *(void**)exc;
        std::string n = cl ? g_api.class_get_name((Il2CppClass*)cl) : "?";
        LERR("[IL2CPP] invoke threw %s", n.c_str());
        return nullptr;
    }
    return r;
}

// 读 boxed int32 的返回值
static int32_t invoke_int(const MethodInfo* m, void* obj, void** args, int32_t dflt) {
    Il2CppObject* r = invoke(m, obj, args);
    if (!r) return dflt;
    return *(int32_t*)((char*)r + 16);
}

static int32_t obj_instance_id(Il2CppObject* o) {
    if (!o) return 0;
    Il2CppClass* k = g_api.object_get_class(o);
    const MethodInfo* m = find_method_any(k, "GetInstanceID");
    if (!m) return 0;
    return invoke_int(m, o, nullptr, 0);
}

static std::string obj_name(Il2CppObject* o) {
    if (!o) return "";
    Il2CppClass* k = g_api.object_get_class(o);
    const MethodInfo* m = find_method_any(k, "get_name");
    if (!m) return "";
    Il2CppObject* r = invoke(m, o, nullptr);
    if (!r) return "";
    return to_utf8((Il2CppString*)r);
}

// managed Object 的 m_CachedPtr 偏移（Unity 2018 il2cpp: klass8 monitor8 cached8 instanceID4）
static void* obj_native(Il2CppObject* o) {
    if (!o) return nullptr;
    return *(void**)((char*)o + 16);
}

// =====================================================================
// 阶段 1：PMX 解析
// =====================================================================
static void run_parse_test() {
    L("========== [PMX] PARSE TEST ==========");
    const char* cands[] = {
        "/data/data/com.hermes.h1game/files/nangongyu.pmx",
        "/data/user/0/com.hermes.h1game/files/nangongyu.pmx",
        "/data/data/com.hermes.h1game/files/models/nangongyu.pmx",
    };
    const char* path = nullptr;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        FILE* f = fopen(cands[i], "rb");
        if (f) { fclose(f); path = cands[i]; break; }
    }
    if (!path) {
        LERR("[PMX] no pmx found, looked in:");
        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) LERR("[PMX]   %s", cands[i]);
        L("========== [PMX] SKIP ==========");
        return;
    }
    L("[PMX] file = %s", path);
    mmd::Model model;
    std::string err;
    if (!mmd::Load(path, model, err)) {
        LERR("[PMX] parse failed: %s", err.c_str());
        L("========== [PMX] FAILED ==========");
        return;
    }
    mmd::DumpStats(model, L);
    L("[PMX] CHECK verts=%zu(exp 21804)%s mats=%zu(exp 25)%s bones=%zu(exp 224)%s",
      model.verts.size(), model.verts.size() == 21804 ? " OK" : " BAD",
      model.materials.size(), model.materials.size() == 25 ? " OK" : " BAD",
      model.bones.size(), model.bones.size() == 224 ? " OK" : " BAD");
    L("========== [PMX] END ==========");
}

// =====================================================================
// 阶段 2：SMR 探测
// =====================================================================
struct SmrInfo {
    Il2CppObject* obj = nullptr;
    void*         native = nullptr;
    std::string   name;
    int32_t       meshId = 0;     // get_sharedMesh() 的 instanceID
    int32_t       meshIdRaw = 0;  // 从 native +0x1d4 直接读出来的
    int32_t       boneCount = 0;  // get_bones().Length
    int32_t       boneSizeRaw = 0;// 从 native +0x1e0 + 0x10 读出来的
    void*         boneData = nullptr;
    bool          active = false;
};

static Il2CppClass* g_smrClass = nullptr;
static Il2CppClass* g_meshClass = nullptr;
static const MethodInfo* g_mFindAll = nullptr;   // Resources.FindObjectsOfTypeAll(Type)
static const MethodInfo* g_mGetBones = nullptr;  // SMR.get_bones
static const MethodInfo* g_mGetMesh = nullptr;   // SMR.get_sharedMesh
static const MethodInfo* g_mGetVC = nullptr;     // Mesh.get_vertexCount

static bool init_unity_refs() {
    // 诊断：域里有几个 assembly、每个叫什么（只在失败时打，避免刷屏）
    if (g_api.domain_get && g_api.domain_get_assemblies) {
        Il2CppDomain* d = g_api.domain_get();
        size_t an = 0;
        const Il2CppAssembly** as = d ? g_api.domain_get_assemblies(d, &an) : nullptr;
        static size_t lastAn = 0;
        if (an != lastAn) {
            lastAn = an;
            L("[UNITY] assemblies=%zu", an);
        }
    }
    g_smrClass = find_class_any("UnityEngine", "SkinnedMeshRenderer");
    g_meshClass = find_class_any("UnityEngine", "Mesh");
    if (!g_smrClass || !g_meshClass) {
        LERR("[UNITY] class lookup failed smr=%p mesh=%p", (void*)g_smrClass, (void*)g_meshClass);
        return false;
    }
    Il2CppClass* resClass = find_class_any("UnityEngine", "Resources");
    if (!resClass) { LERR("[UNITY] Resources class missing"); return false; }

    g_mFindAll = find_method_any(resClass, "FindObjectsOfTypeAll");
    g_mGetBones = find_method_any(g_smrClass, "get_bones");
    g_mGetMesh = find_method_any(g_smrClass, "get_sharedMesh");
    g_mGetVC = find_method_any(g_meshClass, "get_vertexCount");
    L("[UNITY] SMR=%p mesh=%p FindObjectsOfTypeAll=%p get_bones=%p get_sharedMesh=%p",
      (void*)g_smrClass, (void*)g_meshClass, (void*)g_mFindAll,
      (void*)g_mGetBones, (void*)g_mGetMesh);
    return g_mFindAll && g_mGetBones && g_mGetMesh;
}

static std::vector<SmrInfo> g_smrs;

static bool scan_smrs() {
    Il2CppReflectionType* t = g_api.type_get_object(g_api.class_get_type(g_smrClass));
    if (!t) { LERR("[UNITY] type object null"); return false; }
    void* args[1] = { t };
    Il2CppObject* arr = invoke(g_mFindAll, nullptr, args);
    if (!arr) { LERR("[UNITY] FindObjectsOfTypeAll returned null"); return false; }

    size_t n = ((Il2CppArray*)arr)->max_length;
    if (n > 0) L("[UNITY] FindObjectsOfTypeAll(SkinnedMeshRenderer) -> %zu", n);

    g_smrs.clear();
    for (size_t i = 0; i < n; i++) {
        Il2CppObject* o = *(Il2CppObject**)array_addr((Il2CppArray*)arr, 8, i);
        if (!o) continue;
        SmrInfo si;
        si.obj = o;
        si.native = obj_native(o);
        si.name = obj_name(o);

        Il2CppObject* bones = invoke(g_mGetBones, o, nullptr);
        if (bones) si.boneCount = (int32_t)((Il2CppArray*)bones)->max_length;

        Il2CppObject* mesh = invoke(g_mGetMesh, o, nullptr);
        if (mesh) {
            si.meshId = obj_instance_id(mesh);
            if (g_mGetVC) {
                int32_t vc = invoke_int(g_mGetVC, mesh, nullptr, -1);
                (void)vc;
            }
        }
        if (si.native) {
            si.meshIdRaw   = *(int32_t*)((char*)si.native + 0x1d4);
            si.boneData    = *(void**)((char*)si.native + 0x1e0);
            si.boneSizeRaw = *(int32_t*)((char*)si.native + 0x1f0);
        }
        g_smrs.push_back(si);
    }
    return !g_smrs.empty();
}

static void dump_smrs() {
    L("========== [UNITY] SMR DUMP (%zu) ==========", g_smrs.size());
    int offMatch = 0, offTotal = 0;
    for (size_t i = 0; i < g_smrs.size(); i++) {
        const SmrInfo& s = g_smrs[i];
        bool canMatch = (s.meshId != 0);
        if (canMatch) {
            offTotal++;
            if (s.meshIdRaw == s.meshId) offMatch++;
        }
        L("[UNITY] #%zu name='%s' native=%p bones=%d(sizeRaw=%d) data=%p meshID=%d(raw@0x1d4=%d) %s",
          i, s.name.c_str(), s.native, s.boneCount, s.boneSizeRaw, s.boneData,
          s.meshId, s.meshIdRaw,
          !canMatch ? "?" : (s.meshIdRaw == s.meshId ? "OFFSET-OK" : "OFFSET-MISMATCH"));
    }
    L("[UNITY] offset check: %d / %d match (%s)", offMatch, offTotal,
      offTotal && offMatch == offTotal ? "PASS" : "FAIL");
    L("========== [UNITY] SMR DUMP END ==========");
}

// =====================================================================
// 阶段 3：写入通道实验 —— 两个 SMR 的 m_Mesh instanceID 对调再还原
// =====================================================================
static bool g_expDone = false;

static void run_swap_experiment() {
    // 选两个 native 有效、mesh 有效的 SMR
    int a = -1, b = -1;
    for (size_t i = 0; i < g_smrs.size(); i++) {
        if (!g_smrs[i].native || g_smrs[i].meshId == 0) continue;
        if (a < 0) { a = (int)i; continue; }
        if (g_smrs[i].meshId != g_smrs[a].meshId) { b = (int)i; break; }
    }
    if (a < 0 || b < 0) {
        LERR("[UNITY] swap experiment skipped (need 2 SMRs with distinct meshes, have a=%d b=%d)", a, b);
        g_expDone = true;
        return;
    }
    SmrInfo& A = g_smrs[a];
    SmrInfo& B = g_smrs[b];
    L("========== [UNITY] WRITE EXPERIMENT ==========");
    L("[UNITY] A=#%d '%s' meshID=%d  B=#%d '%s' meshID=%d",
      a, A.name.c_str(), A.meshIdRaw, b, B.name.c_str(), B.meshIdRaw);

    int32_t* pA = (int32_t*)((char*)A.native + 0x1d4);
    int32_t  oldA = *pA;
    *pA = B.meshIdRaw;      // ← 唯一的写入动作

    // 立刻读回：managed getter 走的是同一条路（native+0x1d4 → instanceID 查表）
    Il2CppObject* meshAfter = invoke(g_mGetMesh, A.obj, nullptr);
    int32_t afterId = meshAfter ? obj_instance_id(meshAfter) : 0;
    L("[UNITY] after write: get_sharedMesh() instanceID = %d (expected %d) %s",
      afterId, B.meshIdRaw, afterId == B.meshIdRaw ? "WRITE-OK" : "WRITE-FAIL");

    *pA = oldA;             // 还原
    Il2CppObject* meshBack = invoke(g_mGetMesh, A.obj, nullptr);
    int32_t backId = meshBack ? obj_instance_id(meshBack) : 0;
    L("[UNITY] after restore: instanceID = %d (expected %d) %s",
      backId, oldA, backId == oldA ? "RESTORE-OK" : "RESTORE-FAIL");
    L("========== [UNITY] WRITE EXPERIMENT END ==========");
    g_expDone = true;
}

// =====================================================================
// 阶段 4：m_Bones 扩容探测（1B / hook 路线前置）
//
// 已知：m_Bones 起始 +0x1e0，data 指针 @+0x00（int32 instanceID 数组），
//       size @+0x10（get_bones 里 `ldrsw x2, [x21, #0x10]` 读的就是它）。
// 未知：capacity 字段在哪。本次只做只读 dump，一个字节都不写。
// =====================================================================
static void probe_bones_layout() {
    int idx = -1;
    for (size_t i = 0; i < g_smrs.size(); i++) {
        if (g_smrs[i].name.find("11055_Body") != std::string::npos) { idx = (int)i; break; }
    }
    if (idx < 0) {
        for (size_t i = 0; i < g_smrs.size(); i++) {
            if (g_smrs[i].boneCount > 40) { idx = (int)i; break; }
        }
    }
    if (idx < 0) { LERR("[BONE] no suitable SMR found"); return; }

    SmrInfo& s = g_smrs[idx];
    uint8_t* base = (uint8_t*)s.native;
    if (!base) { LERR("[BONE] native null"); return; }

    L("========== [BONE] LAYOUT DUMP ==========");
    L("[BONE] target='%s' native=%p getbones=%d", s.name.c_str(), s.native, s.boneCount);

    void*  dataPtr = *(void**)(base + 0x1e0);
    int32_t sizeVal = *(int32_t*)(base + 0x1f0);
    L("[BONE] data=%p  size@0x1f0=%d", dataPtr, sizeVal);

    // 把 0x1d0 ~ 0x220 全部打出来，人工对着找 capacity
    for (int off = 0x1d0; off <= 0x220; off += 16) {
        uint64_t a = *(uint64_t*)(base + off);
        uint64_t b = *(uint64_t*)(base + off + 8);
        L("[BONE] +0x%03x  %016llx  %016llx", off,
          (unsigned long long)a, (unsigned long long)b);
    }

    // data 前 8 个 instanceID，确认数组内容可读
    if (dataPtr) {
        int32_t* ids = (int32_t*)dataPtr;
        L("[BONE] data[0..7] = %d %d %d %d %d %d %d %d",
          ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], ids[6], ids[7]);
    }

    // 猜 capacity：size 后面 4/8 字节上的小整数
    int32_t c1 = *(int32_t*)(base + 0x1f4);
    int32_t c2 = *(int32_t*)(base + 0x1f8);
    int32_t c3 = *(int32_t*)(base + 0x1e8);
    // +0x1f8 是 int64 capacity（实测 144 = 2×72，标准 vector 扩容策略）
    int64_t cap64 = *(int64_t*)(base + 0x1f8);
    L("[BONE] size@0x1f0=%d  capacity@0x1f8=%lld", sizeVal, (long long)cap64);
    L("[BONE] verdict: need 224, have cap %lld -> %s",
      (long long)cap64, cap64 >= 224 ? "ENOUGH" : "NEED-REALLOC");

    // ---- 零风险写测试：把 size 顶到 capacity（不越界），读回，还原 ----
    int32_t* sizePtr = (int32_t*)(base + 0x1f0);
    int32_t orig = *sizePtr;
    int32_t target = (cap64 > orig && cap64 < 4096) ? (int32_t)cap64 : orig;

    *sizePtr = target;
    Il2CppObject* after = invoke(g_mGetBones, s.obj, nullptr);
    int32_t afterN = after ? (int32_t)((Il2CppArray*)after)->max_length : -1;
    L("[BONE] size write test: %d -> %d, get_bones()=%d  %s",
      orig, target, afterN, afterN == target ? "WRITE-OK" : "WRITE-FAIL");

    *sizePtr = orig;
    Il2CppObject* back = invoke(g_mGetBones, s.obj, nullptr);
    int32_t backN = back ? (int32_t)((Il2CppArray*)back)->max_length : -1;
    L("[BONE] restored: get_bones()=%d  %s", backN, backN == orig ? "RESTORE-OK" : "RESTORE-FAIL");

    // ---- 装 hook（容器析构拦截）----
    bool hooked = bonehook::install();
    L("[BONE] hook installed=%d addr=%p", (int)hooked, bonehook::found_address());

    if (!hooked) {
        LERR("[BONE] hook not installed -> 不做持久化扩容（会崩在析构）");
        L("========== [BONE] LAYOUT DUMP END ==========");
        return;
    }

    // ---- 持久化扩容 ----
    // hook 已经把容器析构接管了：析构前会把这几个字段还原成 Unity 原值，
    // 所以 data 换成我们 malloc 的指针是安全的，Unity 会 free 它自己那块。
    const int32_t TARGET = 224;
    int32_t* oldData = (int32_t*)*(void**)(base + 0x1e0);
    int64_t  oldCap  = *(int64_t*)(base + 0x1f8);
    int32_t  oldSize = orig;

    if (oldData && oldSize > 0 && oldSize < TARGET) {
        int32_t* newData = (int32_t*)malloc((size_t)TARGET * 4);
        if (newData) {
            memset(newData, 0, (size_t)TARGET * 4);
            for (int32_t i = 0; i < oldSize; i++) newData[i] = oldData[i];
            // 多出来的槽位先填第一个骨骼的 instanceID（合法非零，不会崩）
            for (int32_t i = oldSize; i < TARGET; i++) newData[i] = oldData[0];

            bonehook::set_ours(newData, oldData, oldSize, oldCap);

            *(void**)(base + 0x1e0) = newData;
            *(int32_t*)(base + 0x1f0) = TARGET;
            *(int64_t*)(base + 0x1f8) = TARGET;   // 正常偶数，析构交给 hook 还原

            Il2CppObject* eb = invoke(g_mGetBones, s.obj, nullptr);
            int32_t eN = eb ? (int32_t)((Il2CppArray*)eb)->max_length : -1;
            L("[BONE] PERSISTENT expand: %d -> %d  data=%p (orig %p)  get_bones()=%d  %s",
              oldSize, TARGET, (void*)newData, oldData, eN,
              eN == TARGET ? "EXPAND-OK" : "EXPAND-FAIL");

            if (eN != TARGET) {
                // 读回不对就立刻回滚，别留半吊子状态
                *(void**)(base + 0x1e0) = oldData;
                *(int32_t*)(base + 0x1f0) = oldSize;
                *(int64_t*)(base + 0x1f8) = oldCap;
                bonehook::set_ours(nullptr, nullptr, 0, 0);
                free(newData);
                LERR("[BONE] rolled back");
            } else {
                L("[BONE] 持久化生效：现在这个 SMR 认为自己有 %d 根骨骼", TARGET);
            }
        } else {
            LERR("[BONE] malloc failed");
        }
    } else {
        LERR("[BONE] skip: oldData=%p oldSize=%d", oldData, oldSize);
    }

    // ---- vtable / RTTI 侦察：为 hook 析构铺路 ----
    {
        void** vptr = *(void***)base;
        void*  ti   = nullptr;
        const char* tname = nullptr;
        if (vptr) {
            ti = vptr[-1];                       // Itanium ABI: vtable[-1] = typeinfo
            if (ti) tname = *(const char**)((char*)ti + 8);   // type_info::__name
        }
        L("[HOOK] native=%p vptr=%p typeinfo=%p name=%s",
          base, (void*)vptr, ti, tname ? tname : "?");

        if (vptr) {
            for (int i = -2; i < 24; i++) {
                void* fn = vptr[i];
                L("[HOOK] vt[%+d] = %p", i, fn);
            }
        }
    }

    L("========== [BONE] LAYOUT DUMP END ==========");
}

// =====================================================================
// tick 状态机
// =====================================================================
static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum Stage { ST_WAIT, ST_BIND, ST_PARSE, ST_SCAN, ST_DUMP, ST_EXP, ST_PROBE, ST_DONE };

static Stage    g_stage = ST_WAIT;
static long long g_t0 = 0;
static int      g_ticks = 0;
static char     g_status[256] = "init";

extern "C" void native_tick() {
    if (g_stage == ST_DONE) return;
    if (g_t0 == 0) { g_t0 = now_ms(); return; }
    g_ticks++;

    switch (g_stage) {
    case ST_WAIT:
        // 等 4 秒，让游戏把场景铺开
        if (now_ms() - g_t0 > 4000) {
            snprintf(g_status, sizeof(g_status), "binding il2cpp");
            g_stage = ST_BIND;
        }
        break;

    case ST_BIND: {
        // il2cpp 可能比我们晚加载，每秒重试，最多 90 次
        static int tries = 0;
        static long long lastTry = 0;
        if (now_ms() - lastTry < 1000) break;
        lastTry = now_ms();
        tries++;
        if (bind_api()) {
            L("[IL2CPP] bound after %d tries", tries);
            run_parse_test();
            snprintf(g_status, sizeof(g_status), "il2cpp bound (%d tries)", tries);
            g_stage = ST_PARSE;
        } else if (tries >= 90) {
            snprintf(g_status, sizeof(g_status), "il2cpp bind failed x90");
            g_stage = ST_DONE;
        } else {
            snprintf(g_status, sizeof(g_status), "waiting il2cpp (%d)", tries);
        }
        break;
    }

    case ST_PARSE: {
        // Unity 的类表在游戏启动早期还没加载完，第一次查必然空。
        // 跟 ST_SCAN 一样做成每秒重试，别再一脚踩死。
        static int tries = 0;
        static long long lastTry = 0;
        if (now_ms() - lastTry < 1000) break;
        lastTry = now_ms();
        tries++;
        if (init_unity_refs()) {
            L("[UNITY] class refs ready after %d tries", tries);
            snprintf(g_status, sizeof(g_status), "scanning SMR");
            g_stage = ST_SCAN;
        } else if (tries >= 120) {
            snprintf(g_status, sizeof(g_status), "unity class lookup failed x120");
            g_stage = ST_DONE;
        } else {
            snprintf(g_status, sizeof(g_status), "waiting unity classes (%d)", tries);
        }
        break;
    }

    case ST_SCAN: {
        // 场景可能还没加载完（登录界面一个 SMR 都没有），每 2 秒重试
        static int scans = 0;
        static long long lastScan = 0;
        if (now_ms() - lastScan < 2000) break;
        lastScan = now_ms();
        scans++;
        if (scan_smrs()) {
            L("[UNITY] SMR found on scan #%d: %zu", scans, g_smrs.size());
            snprintf(g_status, sizeof(g_status), "found %zu SMR (scan %d)", g_smrs.size(), scans);
            g_stage = ST_DUMP;
        } else if (scans >= 300) {
            snprintf(g_status, sizeof(g_status), "no SMR after %d scans", scans);
            g_stage = ST_DONE;
        } else {
            snprintf(g_status, sizeof(g_status), "scanning SMR #%d (0 found)", scans);
        }
        break;
    }

    case ST_DUMP:
        dump_smrs();
        snprintf(g_status, sizeof(g_status), "dump done");
        g_stage = ST_EXP;
        break;

    case ST_EXP:
        if (!g_expDone) run_swap_experiment();
        snprintf(g_status, sizeof(g_status), "experiment done");
        g_stage = ST_PROBE;
        break;

    case ST_PROBE:
        probe_bones_layout();
        snprintf(g_status, sizeof(g_status), "bone layout dumped");
        g_stage = ST_DONE;
        break;

    default:
        break;
    }
}

// =====================================================================
// JNI
// =====================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeTick(JNIEnv*, jclass) {
    native_tick();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeStatus(JNIEnv* env, jclass) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s | smr=%zu stage=%d", g_status, g_smrs.size(), (int)g_stage);
    return env->NewStringUTF(buf);
}

static void* worker(void*) {
    pick_dir();
    L("===== libmmdruntime v1.1 (game side only) loaded =====");
    return nullptr;
}

static JavaVM* g_vm = nullptr;
static jobject g_hookCls = nullptr;
static jmethodID g_midDoLoad = nullptr;
static jmethodID g_midStatus = nullptr;

// 游戏侧不再有 UI。这些 JNI 保留为空实现，Java 那边照旧调用也不会崩。
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeBind(JNIEnv*, jclass, jclass) {}
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeReset(JNIEnv*, jclass) {
    g_stage = ST_PARSE;
    g_expDone = false;
    g_smrs.clear();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeTouch(JNIEnv*, jclass,
                                               jint, jfloat, jfloat, jfloat, jfloat) {
    return JNI_FALSE;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;

    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);

    // UI 走 App 自己的悬浮窗（libimgui_overlay.so），游戏侧一律不碰 GL
    return JNI_VERSION_1_6;
}
