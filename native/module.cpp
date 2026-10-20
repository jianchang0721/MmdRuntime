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
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <android/dlext.h>
#include <android/log.h>
#include <string>
#include <vector>

#include "game/camera_ctrl.h"
#include "mmd/pmx.h"
#include "bone_hook.h"
#include "logsink.h"
#include "stealth.h"
#include "public/mmd_api.h"
#include "game/loader.h"
#include <dirent.h>
#include <strings.h>
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
        // 光 fflush 只把用户态缓冲推到内核，进程被 SIGSEGV 打死时
        // 页缓存里没落盘的部分照样丢。加载/崩溃排查期间必须 fsync。
        int fd = fileno(f);
        if (fd >= 0) fsync(fd);
    }
    va_list ap2; va_start(ap2, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap2);
    va_end(ap2);
    pthread_mutex_unlock(&g_mtx);
}

#define LERR(...) L(__VA_ARGS__)

// 给 logsink 用的转发：别的模块拼好字符串丢过来，这里补时间戳写文件
static void logsink_forward(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    L("%s", buf);
}

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

    // ---- 下面这些是 V3（建骨骼树 / 建 Mesh / 建材质）必需的 ----
    // 之前漏了一整半：V1/V2 只用 runtime_invoke + 几个 getter，所以没暴露。
    // 结果 create_node 里 g_api.object_new 永远是 null，224 根骨骼一根建不出来。
    B(object_new,               "il2cpp_object_new");
    B(array_new,                "il2cpp_array_new");
    B(class_from_type,          "il2cpp_class_from_type");
    B(method_get_param,         "il2cpp_method_get_param");
    B(class_is_assignable_from, "il2cpp_class_is_assignable_from");
    B(image_get_class_count,    "il2cpp_image_get_class_count");
    B(image_get_class,          "il2cpp_image_get_class");
    B(class_get_fields,         "il2cpp_class_get_fields");
    B(field_get_name,           "il2cpp_field_get_name");
    B(field_get_offset,         "il2cpp_field_get_offset");
    B(field_get_type,           "il2cpp_field_get_type");
    B(type_get_name,            "il2cpp_type_get_name");
    B(class_array_element_size, "il2cpp_class_array_element_size");
    #undef B

    L("[IL2CPP] resolved, missing=%d", missing);
    {
        char cmd[256] = {0};
        FILE* cf = fopen("/proc/self/cmdline", "rb");
        if (cf) { fread(cmd, 1, sizeof(cmd) - 1, cf); fclose(cf); }
        L("[IL2CPP] pid=%d cmdline='%s'", getpid(), cmd);
    }
    // 核心 API 缺一个后面就会在莫名其妙的地方崩，不如在这里直接判死
    if (!g_api.domain_get || !g_api.class_from_name || !g_api.runtime_invoke) {
        LERR("[IL2CPP] core api missing");
        return false;
    }
    if (!g_api.object_new || !g_api.array_new) {
        LERR("[IL2CPP] object_new=%p array_new=%p 缺失，建对象/数组全废",
             (void*)g_api.object_new, (void*)g_api.array_new);
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

// =====================================================================
// 状态机 + UI 桥接 + 模型加载
//
// 生命周期：
//   WAIT → BIND → CLASSES → SCAN → IDLE ⇄ LOAD → ACTIVE
//                                   ↑______________|
//                    (UI 发 cmd.reapply / 重新 enable 时回到 LOAD)
// =====================================================================
enum Stage {
    ST_WAIT = 0,   // 等游戏主循环起来
    ST_BIND,       // 绑 il2cpp（每秒重试）
    ST_CLASSES,    // 等 Unity 类表就绪（每秒重试）
    ST_SCAN,       // 扫 SMR（每 2 秒重试）
    ST_IDLE,       // 待命：轮询 ui.conf，等命令
    ST_LOAD,       // 正在加载模型
    ST_ACTIVE      // 模型已生效
};

static Stage     g_stage = ST_WAIT;
static long long g_t0 = 0;
static int       g_ticks = 0;
static char      g_status[256] = "init";
static bool      g_traced = false;   // 有人在 ptrace 我们（调试器/反作弊机器人）

// ---- UI 下发的配置（mmd_get_api 的 setter 写进来，tick 消费）----
struct UiCmd {
    int   enableReplace = 0;
    float modelScale    = 1.0f;
    char  activeModel[160] = {0};
    int   cmdRescan  = 0;
    int   cmdReapply = 0;
    int   cmdUnload  = 0;
};

static pthread_mutex_t g_cfgMtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_statMtx = PTHREAD_MUTEX_INITIALIZER;
static UiCmd     g_ui;
static bool      g_hasUi = false;
static MmdStatus g_stat;      // 最近一帧的状态快照（给 mmd_get_api 读）
static int       g_statVer = 0;
static game::LoadResult  g_load;       // 最近一次加载结果
static mmd::Model        g_model;      // 解析出来的模型（常驻，别重复解析）
static bool              g_modelLoaded = false;
static volatile int      g_forceReparse = 0;   // API 置位，主线程消费
static int               g_lastSmrIndex = -1;

// ---- 在目录里递归找 .pmx（最多 3 层） ----
static bool find_pmx_in_dir(const char* dir, std::string& out, int depth) {
    if (depth > 3) return false;
    DIR* d = opendir(dir);
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (find_pmx_in_dir(path, out, depth + 1)) { found = true; break; }
        } else {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcasecmp(e->d_name + n - 4, ".pmx")) {
                out = path;
                found = true;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

// ---- UI 给的模型名 → 游戏私有目录里的 pmx 路径 ----
static std::string find_pmx_path() {
    const char* root = "/data/data/com.hermes.h1game/files/models";

    // 拷贝一份再用，g_ui 是 UI 线程写的，不能裸读
    char want[160] = {0};
    pthread_mutex_lock(&g_cfgMtx);
    if (g_hasUi) snprintf(want, sizeof(want), "%s", g_ui.activeModel);
    pthread_mutex_unlock(&g_cfgMtx);

    if (want[0]) {
        char sub[1200];
        snprintf(sub, sizeof(sub), "%s/%s", root, want);
        std::string out;
        if (find_pmx_in_dir(sub, out, 0)) return out;
    }

    std::string out;
    if (find_pmx_in_dir(root, out, 0)) return out;

    const char* legacy = "/data/data/com.hermes.h1game/files/nangongyu.pmx";
    FILE* f = fopen(legacy, "rb");
    if (f) { fclose(f); return legacy; }
    return std::string();
}

// ---- 挑目标 SMR：优先玩家身体 ----
static int pick_target_smr() {
    int idx = -1;
    for (size_t i = 0; i < g_smrs.size(); i++) {
        if (g_smrs[i].name.find("11055_Body") != std::string::npos) { idx = (int)i; break; }
    }
    if (idx < 0) {
        for (size_t i = 0; i < g_smrs.size(); i++) {
            if (g_smrs[i].boneCount >= 40) { idx = (int)i; break; }
        }
    }
    if (idx < 0 && !g_smrs.empty()) idx = 0;
    return idx;
}

// ---- 真正加载 ----
static bool do_load_model() {
    std::string pmx = find_pmx_path();
    if (pmx.empty()) {
        snprintf(g_load.error, sizeof(g_load.error), "找不到 pmx（models 目录为空）");
        LERR("[LOAD] %s", g_load.error);
        return false;
    }
    L("[LOAD] pmx = %s", pmx.c_str());

    if (g_forceReparse) {
        g_forceReparse = 0;
        g_modelLoaded = false;
        L("[LOAD] 收到 reparse 请求，重新读 PMX");
    }

    if (!g_modelLoaded) {
        std::string err;
        g_model = mmd::Model{};
        if (!mmd::Load(pmx.c_str(), g_model, err)) {
            snprintf(g_load.error, sizeof(g_load.error), "PMX 解析失败: %s", err.c_str());
            LERR("[LOAD] %s", g_load.error);
            return false;
        }
        g_modelLoaded = true;
        L("[LOAD] parsed verts=%zu bones=%zu mats=%zu",
          g_model.verts.size(), g_model.bones.size(), g_model.materials.size());
    }

    if (g_smrs.empty()) {
        snprintf(g_load.error, sizeof(g_load.error), "没有扫到 SkinnedMeshRenderer");
        return false;
    }

    int idx = pick_target_smr();
    if (idx < 0) {
        snprintf(g_load.error, sizeof(g_load.error), "没有可用目标");
        return false;
    }
    L("[LOAD] target SMR #%d '%s' bones=%d",
      idx, g_smrs[(size_t)idx].name.c_str(), g_smrs[(size_t)idx].boneCount);

    game::Smr smr;
    smr.managed   = g_smrs[(size_t)idx].obj;
    smr.native    = g_smrs[(size_t)idx].native;
    smr.boneSlots = g_smrs[(size_t)idx].boneCount;

    g_load = game::LoadResult{};
    if (!game::load_model_into_smr(g_model, pmx.c_str(), smr, nullptr, g_load)) {
        LERR("[LOAD] 失败: %s", g_load.error);
        return false;
    }

    g_lastSmrIndex = idx;
    L("[LOAD] 成功: bones=%d verts=%d tris=%d submeshes=%d tex=%d mat=%d meshId=%d",
      g_load.bonesBuilt, g_load.verts, g_load.tris,
      g_load.subMeshes, g_load.textures, g_load.materials, g_load.meshId);
    return true;
}

// ---- 刷新状态快照（每帧末尾调一次，开销就是 1KB memset + 4 个 snprintf）----
static void refresh_status() {
    pthread_mutex_lock(&g_cfgMtx);
    UiCmd ui = g_ui;
    bool  hasUi = g_hasUi;
    pthread_mutex_unlock(&g_cfgMtx);

    MmdStatus s;
    memset(&s, 0, sizeof(s));
    s.apiVersion = MMD_API_VERSION;
    s.stage      = (int)g_stage;
    switch (g_stage) {
        case ST_WAIT:
        case ST_BIND:
        case ST_CLASSES: s.gameState = MMD_GS_DETACHED; break;
        case ST_SCAN:
        case ST_IDLE:    s.gameState = MMD_GS_ATTACHED; break;
        case ST_LOAD:    s.gameState = MMD_GS_LOADING;  break;
        case ST_ACTIVE:  s.gameState = MMD_GS_ACTIVE;   break;
        default:         s.gameState = MMD_GS_UNKNOWN;  break;
    }
    snprintf(s.statusMessage, sizeof(s.statusMessage), "%s", g_status);
    snprintf(s.activeModel,   sizeof(s.activeModel),   "%s", hasUi ? ui.activeModel : "");
    snprintf(s.lastError,     sizeof(s.lastError),     "%s", g_load.ok ? "" : g_load.error);
    snprintf(s.logPath,       sizeof(s.logPath),       "%s/mmdruntime.log", g_dir.c_str());
    s.smrTotal     = (int)g_smrs.size();
    s.smrReplaced  = g_load.ok ? 1 : 0;
    s.boneCount    = g_load.ok ? g_load.bonesBuilt : 0;
    s.vertexCount  = g_load.ok ? g_load.verts : 0;
    s.triCount     = g_load.ok ? g_load.tris : 0;
    s.progress     = g_load.ok ? 100 : (g_stage == ST_LOAD ? 50 : 0);
    s.lastResult   = g_load.ok ? 1 : (g_load.error[0] ? 0 : -1);
    s.enableReplace= hasUi ? ui.enableReplace : 0;
    s.modelScale   = hasUi ? ui.modelScale : 1.0f;
    s.uptimeMs     = g_t0 ? (now_ms() - g_t0) : 0;

    pthread_mutex_lock(&g_statMtx);
    MmdStatus prev = g_stat;
    prev.statusVersion = 0;
    s.statusVersion    = 0;
    long long keepMs   = s.uptimeMs;
    s.uptimeMs         = 0;
    prev.uptimeMs      = 0;
    if (memcmp(&prev, &s, sizeof(MmdStatus)) != 0) g_statVer++;   // 内容真变了才 +1
    s.uptimeMs      = keepMs;
    s.statusVersion = g_statVer;
    g_stat = s;
    pthread_mutex_unlock(&g_statMtx);
}

extern "C" void native_tick() {
    if (g_t0 == 0) { g_t0 = now_ms(); return; }
    g_ticks++;

    // 每 2 秒看一次 tracer。被 ptrace 时我们什么都不做，
    // 因为此时任何“正好在这时发生”的行为都会成为关联证据。
    {
        static long long lastTraceCheck = 0;
        if (now_ms() - lastTraceCheck > 2000) {
            lastTraceCheck = now_ms();
            bool tr = stealth::is_traced();
            if (tr != g_traced) {
                g_traced = tr;
                L("[STEALTH] tracer=%d %s", (int)tr, tr ? "暂停模型操作" : "恢复正常");
            }
        }
    }

    switch (g_stage) {
    case ST_WAIT:
        if (now_ms() - g_t0 > 4000) {
            snprintf(g_status, sizeof(g_status), "binding il2cpp");
            g_stage = ST_BIND;
        }
        break;

    case ST_BIND: {
        static int tries = 0;
        static long long lastTry = 0;
        if (now_ms() - lastTry < 1000) break;
        lastTry = now_ms();
        tries++;
        if (bind_api()) {
            L("[IL2CPP] bound after %d tries", tries);
            run_parse_test();
            snprintf(g_status, sizeof(g_status), "il2cpp bound (%d tries)", tries);
            g_stage = ST_CLASSES;
        } else if (tries >= 120) {
            snprintf(g_status, sizeof(g_status), "il2cpp bind failed x120");
            g_stage = ST_IDLE;   // 还是进 IDLE，至少 UI 能连上看到状态
        } else {
            snprintf(g_status, sizeof(g_status), "waiting il2cpp (%d)", tries);
        }
        break;
    }

    case ST_CLASSES: {
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
            snprintf(g_status, sizeof(g_status), "unity class lookup failed");
            g_stage = ST_IDLE;
        } else {
            snprintf(g_status, sizeof(g_status), "waiting unity classes (%d)", tries);
        }
        break;
    }

    case ST_SCAN: {
        static int scans = 0;
        static long long lastScan = 0;
        if (now_ms() - lastScan < 2000) break;
        lastScan = now_ms();
        scans++;
        if (scan_smrs()) {
            L("[UNITY] SMR found on scan #%d: %zu", scans, g_smrs.size());
            dump_smrs();
            snprintf(g_status, sizeof(g_status), "ready (%zu SMR)", g_smrs.size());
            g_stage = ST_IDLE;
        } else if (scans >= 300) {
            snprintf(g_status, sizeof(g_status), "no SMR after %d scans", scans);
            g_stage = ST_IDLE;
        } else {
            snprintf(g_status, sizeof(g_status), "scanning SMR #%d", scans);
        }
        break;
    }

    case ST_IDLE: {
        {
            static long long lastBeat = 0;
            static int beats = 0;
            if (now_ms() - lastBeat > 5000) {
                lastBeat = now_ms();
                beats++;
                pthread_mutex_lock(&g_cfgMtx);
                bool hasUi = g_hasUi;
                int  en    = g_ui.enableReplace;
                char md[160];
                snprintf(md, sizeof(md), "%s", g_ui.activeModel);
                pthread_mutex_unlock(&g_cfgMtx);
                L("[IDLE] beat #%d statVer=%d hasUi=%d enable=%d model='%s' loadOk=%d",
                  beats, g_statVer, (int)hasUi, hasUi ? en : -1, md, (int)g_load.ok);
            }
        }

        // 取命令（API 侧置位，这里消费）
        pthread_mutex_lock(&g_cfgMtx);
        bool rescan  = g_ui.cmdRescan  != 0;
        bool reapply = g_ui.cmdReapply != 0;
        bool hasUi   = g_hasUi;
        bool want    = hasUi && g_ui.enableReplace;
        g_ui.cmdRescan  = 0;
        g_ui.cmdReapply = 0;
        pthread_mutex_unlock(&g_cfgMtx);

        // 兜底：UI 一直没连上来就在 15 秒后自动跑一次，
        // 让「so 自己能不能加载模型」和「UI 能不能连上」两件事能分开定位
        static bool autoTried = false;
        if (!autoTried && !hasUi && g_t0 && now_ms() - g_t0 > 15000) {
            autoTried = true;
            snprintf(g_status, sizeof(g_status), "no ui, auto load");
            L("[IDLE] 没有 UI 连上来，自动尝试加载一次");
            g_stage = ST_LOAD;
            break;
        }

        if (rescan) {
            g_smrs.clear();
            snprintf(g_status, sizeof(g_status), "rescan requested");
            g_stage = ST_SCAN;
            break;
        }

        // 重新套用（或者第一次打开总开关）
        if (reapply || (want && !g_load.ok)) {
            g_stage = ST_LOAD;
            break;
        }

        break;
    }

    case ST_LOAD: {
        if (g_traced) {
            snprintf(g_status, sizeof(g_status), "paused: tracer attached");
            g_stage = ST_IDLE;
            break;
        }
        snprintf(g_status, sizeof(g_status), "loading model");
        bool ok = do_load_model();
        if (ok) {
            snprintf(g_status, sizeof(g_status), "model active: %s",
                     g_hasUi && g_ui.activeModel[0] ? g_ui.activeModel : "?");
            g_stage = ST_ACTIVE;
        } else {
            snprintf(g_status, sizeof(g_status), "load failed: %s", g_load.error);
            g_stage = ST_IDLE;
            g_modelLoaded = false;   // 允许下次重新解析
        }
        break;
    }

    case ST_ACTIVE: {
        // UI 关总开关 / 发命令时处理（还原模型还没接，先回 IDLE）
        pthread_mutex_lock(&g_cfgMtx);
        bool rescan  = g_ui.cmdRescan  != 0;
        bool reapply = g_ui.cmdReapply != 0;
        bool unload  = g_ui.cmdUnload  != 0;
        bool en      = g_hasUi && g_ui.enableReplace != 0;
        g_ui.cmdRescan  = 0;
        g_ui.cmdReapply = 0;
        g_ui.cmdUnload  = 0;
        pthread_mutex_unlock(&g_cfgMtx);

        if (unload) {
            snprintf(g_status, sizeof(g_status), "unload requested (stub)");
            g_stage = ST_IDLE;
        } else if (!en) {
            snprintf(g_status, sizeof(g_status), "disabled by ui");
            g_stage = ST_IDLE;
        } else if (rescan) {
            g_smrs.clear();
            g_stage = ST_SCAN;
        } else if (reapply) {
            g_stage = ST_LOAD;
        }
        break;
    }
    }

    // 摄像机：放在状态机之后，保证是渲染前最后一写
    if (game::g_camera.enabled) game::g_camera.update();

    refresh_status();   // 状态快照每帧刷一次，UI 侧随时能读
}

// =====================================================================
// UI so：从 memfd 加载，不落盘
//
// 原本是 HookEntry 用 System.load 加载私有目录里的 libimgui_overlay_1.so，
// 那个文件会一直躺在盘上，进程外一扫就能看到（maps 过滤只管进程内）。
//
// 现在改成：文件读到 memfd → android_dlopen_ext 从 fd 加载 →
// maps 里显示的名字是我们自己编的假路径 → 落盘文件立刻 unlink。
//
// 代价：ART 不认识这个模块，所有 Java_com_xxx 形式的 JNI 符号都不会被解析
// （也没人调），所以 JNI 入口全部收回到本 so，这里 dlsym 到 ui_* 再转一手。
// =====================================================================
struct UiFns {
    void (*init)(void);
    void (*set_fonts)(const char*, const char*);
    int  (*touch)(int, float, float, float, float);
    void (*key)(int, int, int);
    int  (*want_text_input)(void);
    int  (*ready)(void);
};

static UiFns g_uiFns = {};
static void* g_uiHandle = nullptr;
static char  g_uiName[256] = {0};

static bool load_ui_from_memfd(const char* soPath, const char* fakeName) {
    int in = open(soPath, O_RDONLY | O_CLOEXEC);
    if (in < 0) { LERR("[UI] open failed: %s errno=%d", soPath, errno); return false; }

    std::vector<char> buf;
    {
        char tmp[65536];
        ssize_t n;
        while ((n = read(in, tmp, sizeof(tmp))) > 0) buf.insert(buf.end(), tmp, tmp + n);
    }
    close(in);
    if (buf.size() < 4096) { LERR("[UI] so too small: %zu", buf.size()); return false; }

    int fd = (int)syscall(__NR_memfd_create, "u", 1u /*MFD_CLOEXEC*/);
    if (fd < 0) { LERR("[UI] memfd_create errno=%d", errno); return false; }

    size_t off = 0;
    while (off < buf.size()) {
        ssize_t w = write(fd, buf.data() + off, buf.size() - off);
        if (w <= 0) { close(fd); LERR("[UI] write memfd errno=%d", errno); return false; }
        off += (size_t)w;
    }

    android_dlextinfo ext{};
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;

    void* h = android_dlopen_ext(fakeName, RTLD_NOW | RTLD_GLOBAL, &ext);
    close(fd);
    if (!h) { LERR("[UI] android_dlopen_ext failed: %s", dlerror()); return false; }

    g_uiHandle = h;
    snprintf(g_uiName, sizeof(g_uiName), "%s", fakeName);

    g_uiFns.init            = (void (*)(void))dlsym(h, "ui_init");
    g_uiFns.set_fonts       = (void (*)(const char*, const char*))dlsym(h, "ui_set_fonts");
    g_uiFns.touch           = (int (*)(int, float, float, float, float))dlsym(h, "ui_touch");
    g_uiFns.key             = (void (*)(int, int, int))dlsym(h, "ui_key");
    g_uiFns.want_text_input = (int (*)(void))dlsym(h, "ui_want_text_input");
    g_uiFns.ready           = (int (*)(void))dlsym(h, "ui_ready");

    if (!g_uiFns.init || !g_uiFns.touch) {
        LERR("[UI] symbols missing init=%p touch=%p", (void*)g_uiFns.init, (void*)g_uiFns.touch);
        return false;
    }

    g_uiFns.init();

    // 落盘文件删掉。映射已经建立，删了不影响进程，重开游戏会重新解一份。
    unlink(soPath);

    L("[UI] loaded from memfd as '%s' handle=%p", fakeName, h);
    return true;
}

static std::string jstr(JNIEnv* env, jstring s) {
    std::string out;
    if (s) {
        const char* raw = env->GetStringUTFChars(s, nullptr);
        if (raw) { out = raw; env->ReleaseStringUTFChars(s, raw); }
    }
    return out;
}

// 返回 "" 表示成功，否则是错误说明（Java 那边据此决定要不要走 System.load 兜底）
extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeLoadUi(JNIEnv* env, jclass, jstring soPath, jstring fakeName) {
    std::string p = jstr(env, soPath);
    std::string f = jstr(env, fakeName);
    if (p.empty() || f.empty()) return env->NewStringUTF("bad args");
    if (g_uiHandle) return env->NewStringUTF("");
    if (!load_ui_from_memfd(p.c_str(), f.c_str())) {
        return env->NewStringUTF("memfd load failed (see log)");
    }
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeUiFonts(JNIEnv* env, jclass, jstring latin, jstring cjk) {
    if (!g_uiFns.set_fonts) return;
    std::string l = jstr(env, latin), c = jstr(env, cjk);
    g_uiFns.set_fonts(l.c_str(), c.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeUiTouch(JNIEnv*, jclass,
                                                 jint action, jfloat x, jfloat y,
                                                 jfloat vw, jfloat vh) {
    if (!g_uiFns.touch) return JNI_FALSE;
    return g_uiFns.touch((int)action, x, y, vw, vh) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeUiReady(JNIEnv*, jclass) {
    return (g_uiFns.ready && g_uiFns.ready()) ? JNI_TRUE : JNI_FALSE;
}

// =====================================================================
// 对外导出：mmd_get_api()
//
// libimgui_overlay.so 在同进程里用 maps_base + elf_sym 拿到 mmd_get_api
// 然后直接调。不走文件、不走 binder，零延迟。
// =====================================================================
static void api_get_status(MmdStatus* out) {
    if (!out) return;
    pthread_mutex_lock(&g_statMtx);
    *out = g_stat;
    pthread_mutex_unlock(&g_statMtx);
    if (out->apiVersion == 0) {          // tick 还没跑过，给个干净的默认值
        memset(out, 0, sizeof(*out));
        out->apiVersion  = MMD_API_VERSION;
        out->lastResult  = -1;
        out->modelScale  = 1.0f;
        snprintf(out->logPath, sizeof(out->logPath), "%s/mmdruntime.log", g_dir.c_str());
    }
}

static void api_set_enable(int on) {
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.enableReplace = on ? 1 : 0;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] set_enable(%d)", on ? 1 : 0);
}

static void api_set_model(const char* name) {
    pthread_mutex_lock(&g_cfgMtx);
    snprintf(g_ui.activeModel, sizeof(g_ui.activeModel), "%s", name ? name : "");
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] set_model('%s')", name ? name : "");
}

static void api_set_scale(float s) {
    if (!(s > 0.0f)) s = 1.0f;
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.modelScale = s;
    pthread_mutex_unlock(&g_cfgMtx);
}

static void api_cmd_rescan() {
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdRescan = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] cmd_rescan");
}

static void api_cmd_reapply() {
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdReapply = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] cmd_reapply");
}

static void api_cmd_unload() {
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdUnload = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] cmd_unload");
}

static void api_log(const char* msg) {
    if (msg) L("[UI] %s", msg);
}

static const char* api_get_log_path() {
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s/mmdruntime.log", g_dir.c_str());
    return buf;
}

static const char* api_get_last_error() {
    static char buf[256];
    pthread_mutex_lock(&g_statMtx);
    snprintf(buf, sizeof(buf), "%s", g_stat.lastError);
    pthread_mutex_unlock(&g_statMtx);
    return buf;
}

// 列游戏私有目录里的模型。UI 在分区存储下读不到 /sdcard，只能问我们。
static int api_list_models(char* out, int outSize) {
    if (!out || outSize <= 0) return 0;
    out[0] = 0;

    const char* root = "/data/data/com.hermes.h1game/files/models";
    DIR* d = opendir(root);
    if (!d) { LERR("[API] list_models: %s 打不开", root); return 0; }

    int n = 0, used = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const bool isDir = (e->d_type == DT_DIR);
        const size_t len = strlen(e->d_name);
        const bool isPmx = len > 4 && !strcasecmp(e->d_name + len - 4, ".pmx");
        if (!isDir && !isPmx) continue;
        if (used + (int)len + 2 > outSize) break;
        memcpy(out + used, e->d_name, len);
        used += (int)len;
        out[used++] = '\n';
        out[used] = 0;
        n++;
    }
    closedir(d);
    L("[API] list_models -> %d", n);
    return n;
}

static const char* api_active_model() {
    static char buf[160];
    pthread_mutex_lock(&g_cfgMtx);
    snprintf(buf, sizeof(buf), "%s", g_hasUi ? g_ui.activeModel : "");
    pthread_mutex_unlock(&g_cfgMtx);
    return buf;
}

static void api_cmd_reparse() {
static int api_camera_enable(int on) {
    game::camera_set_enabled(on);
    L("[API] camera_enable(%d) -> %d", on, game::camera_is_enabled());
    return game::camera_is_enabled();
}

static int api_camera_is_enabled() {
    return game::camera_is_enabled();
}

static void api_camera_reset() {
    game::g_camera.reset();
    L("[API] camera_reset");
}

static int api_camera_touch(int action, int pointerId, float x, float y, float vw, float vh) {
    return game::camera_touch(action, pointerId, x, y, vw, vh) ? 1 : 0;
}

static void api_camera_zoom(float d) {
    game::g_camera.zoom(d);
}
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdReapply = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    g_forceReparse = 1;   // 主线程的 do_load_model 会看清这个再重新读盘
    L("[API] cmd_reparse");
}

extern "C" __attribute__((visibility("default")))
const MmdApi* mmd_get_api(void) {
    static MmdApi api = {
        (int)sizeof(MmdApi),
        MMD_API_VERSION,
        MMD_BUILD_TAG,
        api_get_status,
        api_set_enable,
        api_set_model,
        api_set_scale,
        api_cmd_rescan,
        api_cmd_reapply,
        api_cmd_unload,
        api_log,
        api_get_log_path,
        api_get_last_error,
        api_list_models,
        api_active_model,
        api_cmd_reparse,
        api_camera_enable,
        api_camera_is_enabled,
        api_camera_reset,
        api_camera_touch,
        api_camera_zoom,
    };
    static bool announced = false;
    if (!announced) {
        announced = true;
        L("[API] mmd_get_api() v%d size=%zu tag=%s",
          MMD_API_VERSION, sizeof(MmdApi), MMD_BUILD_TAG);
    }
    return &api;
}

// =====================================================================
// JNI
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
    L("===== libmmdruntime v1.2 (game side) loaded =====");
    L("[STEALTH] installed=%d self='%s'", (int)stealth::installed(), stealth::self_path());
    L("[STEALTH] hooks: %s", stealth::hook_summary());
    // 两个数各管一边：
    //   real_mappings —— 真实 maps 里我们自己的映射段（2~4 正常）
    //   filtered_hits —— 走 hook 读到的 maps 里我们还有几条（应该 0）
    int realMaps = stealth::real_mappings();
    int filtered = stealth::filtered_hits();
    L("[STEALTH] real mappings=%d (2~4 normal) | via-hook=%d %s",
      realMaps, filtered, filtered == 0 ? "HIDDEN-OK" : "FILTER-BROKEN");
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
    g_stage = ST_SCAN;
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

    // 先接日志汇：stealth / mmd / game 各层都靠它写文件，
    // 不接的话 logcat 一被限流就什么也看不见。
    logsink::Set(logsink_forward);

    // 然后把 /proc 里我们的痕迹盖掉。越早装越安全——
    // 游戏侧反作弊可能在任何时刻扫 maps，晚一秒就多一秒空窗期。
    stealth::Install();

    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);

    // UI 走 App 自己的悬浮窗（libimgui_overlay.so），游戏侧一律不碰 GL
    return JNI_VERSION_1_6;
}
