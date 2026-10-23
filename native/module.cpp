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
#include <math.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <android/dlext.h>
#include <android/log.h>
#include <signal.h>
#include <string>
#include <vector>

#include "game/camera_ctrl.h"
#include "game/cam_native.h"
#include "game/cam_override.h"
#include "game/smr_layout.h"
#include "game/bone_names.h"
#include "mmd/pmx.h"
#include "bone_hook.h"
#include "logsink.h"
#include "stealth.h"
#include "public/mmd_api.h"
#include "game/loader.h"
#include "game/self_smr.h"
#include "game/layer_guard.h"
#include "game/skin_cpu.h"
#include "crash_report.h"
#include <dirent.h>
#include <strings.h>
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

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
    // 显式长度版本：内部不做 strlen，名字异常也不会报名天文数字。
    // build-59 的 malloc(2^64-9330080) 就是 string_new 的 strlen 坑。
    B(string_new_len,           "il2cpp_string_new_len");
    B(gc_disable,               "il2cpp_gc_disable");
    B(gc_enable,                "il2cpp_gc_enable");
    B(gchandle_new,             "il2cpp_gchandle_new");
    B(gchandle_free,            "il2cpp_gchandle_free");
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
    // 上限保护：字符串对象损坏时 len 可能是天文数字，reserve(len) 直接 OOM/bad_alloc
    if (len < 0 || len > 4096) return "";
    const uint16_t* ch = (const uint16_t*)(p + 20);
    std::string out;
    out.reserve((size_t)len);
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
    void*         meshObj = nullptr;   // get_sharedMesh() 拿到的 Mesh 对象，克隆模板用
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
            si.meshObj = mesh;
            si.meshId = obj_instance_id(mesh);
            if (g_mGetVC) {
                int32_t vc = invoke_int(g_mGetVC, mesh, nullptr, -1);
                (void)vc;
            }
        }
        if (si.native) {
            // 偏移全部从 layout() 取：默认值之外，运行时探测会覆盖
            si.meshIdRaw   = *(int32_t*)((char*)si.native + game::layout().offMesh);
            si.boneData    = *(void**)((char*)si.native + game::layout().offBoneData);
            si.boneSizeRaw = *(int32_t*)((char*)si.native + game::layout().offBoneSize);
        }
        g_smrs.push_back(si);
    }
    // 用本批样本校准 SMR 字段偏移：
    // 默认值是 Unity 2018.4.13f1 实测的，游戏或 Unity 升级后靠这里重扫
    {
        std::vector<game::SmrSample> samples;
        samples.reserve(g_smrs.size());
        for (const SmrInfo& s : g_smrs) {
            if (!s.native || s.meshId == 0 || s.boneCount <= 0) continue;
            game::SmrSample x;
            x.native    = s.native;
            x.meshId    = s.meshId;
            x.boneCount = s.boneCount;
            samples.push_back(x);
        }
        if (samples.size() >= 2) game::detect_layout(samples);
    }

    return !g_smrs.empty();
}

// =====================================================================
// 全场景骨骼普查 + 玩家锚定
//
// 三个信号（加权）：
//   ① 相机空间坐标 —— 玩家在 |x|<1.5、z 3..8 的窄带里，而且是常量
//   ② 父链交集 —— 相机和候选共享祖先（第三人称相机挂在角色下）→ 决定性
//   ③ 屏幕投影 —— 玩家稳定落在画面中下方
//
// 所有候选都打一行，锁错了能一眼看出是哪一步判错。
// 锁定后把骨骼 Transform 全部 gchandle 钉住（锚点），翻译层直接用。
// =====================================================================
struct AnchorBone {
    void*    tr     = nullptr;   // Il2CppObject* Transform
    uint32_t handle = 0;         // gchandle（钉住，防 GC）
    int32_t  id     = 0;
    char     name[64] = {0};
};

static std::vector<AnchorBone> g_anchorBones;
static void* g_anchorSmr   = nullptr;
static void* g_anchorTr    = nullptr;   // Transform of the anchored character
static int   g_anchorIdx   = -1;
static int   g_anchorScore = 0;

static void read_name(void* obj, const MethodInfo* mGetName, char* out, int cap) {
    out[0] = 0;
    if (!obj || !mGetName) return;
    void* s = invoke(mGetName, obj, nullptr);
    if (!s) return;
    int32_t len = *(int32_t*)((char*)s + 16);
    if (len < 0 || len > 256) return;
    const uint16_t* ch = (const uint16_t*)((char*)s + 20);
    int n = 0;
    for (int32_t i = 0; i < len && n < cap - 1; i++)
        out[n++] = (ch[i] < 0x80) ? (char)ch[i] : '?';
    out[n] = 0;
}

static float vec3_get(void* boxed, int idx) {
    if (!boxed) return 0.0f;
    return ((float*)((char*)boxed + 16))[idx];
}

// Transform 往上 6 层拼成 /a/b/c（父链名）。
// census 打分和"同一角色多 SMR 合并"都要用。
static void read_chain(void* tr, const MethodInfo* mParent, const MethodInfo* mName,
                       char* out, int cap) {
    out[0] = 0;
    if (!tr || !mName) return;
    int off = 0;
    void* t = tr;
    for (int k = 0; k < 6 && t; k++) {
        char nm[64];
        read_name(t, mName, nm, sizeof(nm));
        if (nm[0] && off < cap - 72)
            off += snprintf(out + off, cap - off, "/%s", nm);
        t = mParent ? invoke(mParent, t, nullptr) : nullptr;
    }
}

// 这套游戏的角色骨架只有两种命名：3ds Max Biped 的 'Bip001 ...' 和 'BN_...'。
// 其他对象（特效、宠物、道具、其他玩家的特殊模型）骨骼名是 'Bone41' 之类，
// 还退一个名字都认不出来 —— 直接排除，别让相机距离分把它们送上来。
// 实测：build-164 里自由相机飞到别处后点初始化，锚到了 'Bone41'，
// 配对只剩 13 对，模型完全不动。
static int count_known_bones(void* smr, const MethodInfo* mBones, const MethodInfo* mName) {
    if (!smr || !mBones || !mName) return 0;
    void* arr = invoke(mBones, smr, nullptr);
    if (!arr) return 0;
    const int32_t n = *(int32_t*)((char*)arr + 24);
    if (n <= 0 || n > 1024) return 0;
    int cnt = 0;
    for (int32_t k = 0; k < n; k++) {
        void* bt = *(void**)((char*)arr + 32 + (size_t)k * 8);
        if (!bt) continue;
        char nm[64];
        read_name(bt, mName, nm, sizeof(nm));
        if (strstr(nm, "Bip001") || strncmp(nm, "BN_", 3) == 0) cnt++;
    }
    return cnt;
}

static void build_retarget();   // defined below, called after a successful anchor

static void census_and_anchor(bool force = false) {
    Il2CppClass* trCls   = find_class_any("UnityEngine", "Transform");
    Il2CppClass* objCls  = find_class_any("UnityEngine", "Object");
    Il2CppClass* compCls = find_class_any("UnityEngine", "Component");
    Il2CppClass* goCls   = find_class_any("UnityEngine", "GameObject");
    Il2CppClass* camCls  = find_class_any("UnityEngine", "Camera");

    const MethodInfo* mBones  = find_method_any(g_smrClass, "get_bones");
    const MethodInfo* mName   = objCls  ? find_method_any(objCls, "get_name") : nullptr;
    const MethodInfo* mGetTr  = compCls ? find_method_any(compCls, "get_transform") : nullptr;
    const MethodInfo* mGetGO  = compCls ? find_method_any(compCls, "get_gameObject") : nullptr;
    const MethodInfo* mPos    = trCls   ? find_method_any(trCls, "get_position") : nullptr;
    const MethodInfo* mParent = trCls   ? find_method_any(trCls, "get_parent") : nullptr;
    const MethodInfo* mId     = objCls  ? find_method_any(objCls, "GetInstanceID") : nullptr;
    const MethodInfo* mW2C    = camCls  ? find_method_any(camCls, "get_worldToCameraMatrix") : nullptr;
    const MethodInfo* mW2S    = camCls  ? find_method_any(camCls, "WorldToScreenPoint") : nullptr;
    const MethodInfo* mPixW   = camCls  ? find_method_any(camCls, "get_pixelWidth") : nullptr;
    const MethodInfo* mPixH   = camCls  ? find_method_any(camCls, "get_pixelHeight") : nullptr;
    const MethodInfo* mActive = goCls   ? find_method_any(goCls, "get_activeInHierarchy") : nullptr;
    const MethodInfo* mMain   = camCls  ? find_method_any(camCls, "get_main") : nullptr;

    // 已经锚定过：对象还活着 **而且 GameObject 还 active** 就不重选。
    //
    // 只看 native 指针不够 —— 角色被对象池收掉时 native 还在，
    // 但整个 GameObject 已经停用，骨骼永远不再更新，表现就是
    // 「模型挂着但摆着不跟着动」。这是 build-166/167 之后剩下的坑。
    if (!force && g_anchorSmr && !g_anchorBones.empty()) {
        void* nat = *(void**)((char*)g_anchorSmr + 16);
        bool act = false;
        if (nat && mGetGO && mActive) {
            void* go = invoke(mGetGO, g_anchorSmr, nullptr);
            void* r  = go ? invoke(mActive, go, nullptr) : nullptr;
            if (r) act = (*(uint8_t*)((char*)r + 16)) != 0;
        }
        if (nat && act) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                L("[CENSUS] already anchored (%zu bones, score=%d, active) -> skip",
                  g_anchorBones.size(), g_anchorScore);
            }
            return;
        }
        L("[CENSUS] anchor stale (native=%p active=%d) -> re-select", nat, (int)act);
    }

    L("[CENSUS] 开始：SMR=%zu  依赖 bones=%p name=%p pos=%p w2c=%p w2s=%p",
      g_smrs.size(), (void*)mBones, (void*)mName, (void*)mPos, (void*)mW2C, (void*)mW2S);

    // ---- 相机 ----
    void* cam = mMain ? invoke(mMain, nullptr, nullptr) : nullptr;
    void* camTr = (cam && mGetTr) ? invoke(mGetTr, cam, nullptr) : nullptr;

    float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (cam && mW2C) {
        void* r = invoke(mW2C, cam, nullptr);
        if (r) memcpy(view, (char*)r + 16, 16 * sizeof(float));
    }
    int pixW = 1920, pixH = 1080;
    if (cam && mPixW) { void* r = invoke(mPixW, cam, nullptr); if (r) pixW = *(int32_t*)((char*)r + 16); }
    if (cam && mPixH) { void* r = invoke(mPixH, cam, nullptr); if (r) pixH = *(int32_t*)((char*)r + 16); }

    // 相机祖先链（含自己，往上 10 层）
    int32_t camChain[12] = {0};
    int camChainN = 0;
    {
        void* t = camTr;
        for (int i = 0; i < 12 && t; i++) {
            void* r = mId ? invoke(mId, t, nullptr) : nullptr;
            camChain[camChainN++] = r ? *(int32_t*)((char*)r + 16) : 0;
            t = mParent ? invoke(mParent, t, nullptr) : nullptr;
        }
    }
    L("[CENSUS] 相机 tr=%p 祖先链 %d 层", camTr, camChainN);

    // ---- 遍历所有 SMR ----
    int best = -1, bestScore = 0;

    // 前 5 名候选，出问题时一眼看得出该选谁、实际选了谁
    struct Cand { int idx; int score; };
    Cand top[10] = {};
    auto push_top = [&](int idx, int sc) {
        if (sc <= 0) return;
        for (int i = 0; i < 10; i++) {
            if (sc > top[i].score) {
                for (int j = 9; j > i; j--) top[j] = top[j - 1];
                top[i] = {idx, sc};
                return;
            }
        }
    };

    for (size_t i = 0; i < g_smrs.size(); i++) {
        SmrInfo& s = g_smrs[i];
        // 池子里的死对象：native 为空、或者骨骼数异常的直接跳（build-143 实测：
        // 137 个 SMR 里第一个就把进程干崩了，大概率是在死对象上调方法）
        if (!s.obj || !s.native) continue;
        if (s.boneCount <= 0 || s.boneCount > 512) continue;

        const bool probe = (i < 2);
        if (probe) L("[CENSUS] probe #%zu obj=%p native=%p bones=%d", i, s.obj, s.native, s.boneCount);

        void* tr = mGetTr ? invoke(mGetTr, s.obj, nullptr) : nullptr;
        if (probe) L("[CENSUS] probe #%zu tr=%p", i, tr);
        if (!tr) continue;

        void* p  = mPos ? invoke(mPos, tr, nullptr) : nullptr;
        if (probe) L("[CENSUS] probe #%zu pos=%p", i, p);
        const float wx = vec3_get(p, 0), wy = vec3_get(p, 1), wz = vec3_get(p, 2);

        // 相机空间（Matrix4x4 列主序：m[col*4+row]）
        const float cx = view[0]*wx + view[4]*wy + view[8]*wz + view[12];
        const float cy = view[1]*wx + view[5]*wy + view[9]*wz + view[13];
        const float cz = view[2]*wx + view[6]*wy + view[10]*wz + view[14];

        // 屏幕投影先关掉：build-143 实测这一步（值类型 Vector3 传参）很可能就是崩溃点，
        // 而它对“锁玩家”不是必需信号（父链交集已经足够决定性）。
        float sx = -1, sy = -1, sz = 0;
        (void)cam; (void)mW2S; (void)sz;

        // 激活
        bool active = true;
        if (mGetGO && mActive) {
            void* go = invoke(mGetGO, s.obj, nullptr);
            void* r  = go ? invoke(mActive, go, nullptr) : nullptr;
            if (r) active = (*(uint8_t*)((char*)r + 16) != 0);
        }

        // 未激活的对象一律不参与锚定。
        //
        // 这是「玩家定位」最关键的一条：游戏的对象池里躺着一大堆
        // 未激活的角色模板（11013_U / 11001_U / 12011_U 各一套），
        // 它们的骨骼永远不更新。之前按分数挑，挑中的很可能就是模板，
        // 于是读数恒定、模型摆着不动 —— 不管换哪个索引都一样。
        //
        // 只有在场上真活着的那个，骨骼才每帧被动画驱动。
        if (!active) continue;

        // 父链交集（build-143 崩在这里面也说不定：把每次 get_parent 的返回值打出来）
        int shareAnc = 0;
        if (mId && mParent) {
            void* t = tr;
            for (int k = 0; k < 10 && t; k++) {
                void* r = invoke(mId, t, nullptr);
                int32_t idv = r ? *(int32_t*)((char*)r + 16) : 0;
                for (int c = 0; c < camChainN; c++)
                    if (idv && idv == camChain[c]) { shareAnc = 1; break; }
                if (shareAnc) break;
                void* nx = invoke(mParent, t, nullptr);
                if (probe) L("[CENSUS] probe #%zu parent[%d] id=%d -> %p", i, k, idv, nx);
                t = nx;
            }
        }

        // 打分
        int score = 0;
        if (active) score += 10;
        if (s.boneCount >= 40) score += 10;
        if (shareAnc) score += 100;
        // Unity view space: forward is NEGATIVE z. Build-147/150 proved that a
        // plain boolean test is useless -- #3 Body scored 60 without ever being
        // in front of the camera. Score by distance instead: the character the
        // camera follows is the nearest one in front.
        // 相机距离分：build-164 之前是 400-12d，这一项就压过了其他所有信号，
        // 导致自由相机飞远后锚到别的对象。降到 150-6d，它仍然能区分“身前/身后/
        // 远近”，但不再一枝独大；真正决定性的信号是父链交集和骨骼名。
        if (cz < -0.5f) {
            const float d = -cz;
            int bonus = (int)(150.0f - d * 6.0f);
            if (bonus < 3) bonus = 3;
            score += bonus;
        } else {
            score -= 50;   // behind the camera
        }
        if (fabsf(cx) < 4.0f) score += 20;
        if (sx >= 0 && pixW > 0 && pixH > 0) {
            const float u = sx / (float)pixW, v = sy / (float)pixH;
            if (u > 0.30f && u < 0.70f && v > 0.10f && v < 0.55f) score += 20;
        }
        // Only the Body SMR carries the Bip001 skeleton. Face/Hair/Weapon
        // SMRs sit at the same place and score the same geometrically, but
        // their bone names never match -> zero retarget pairs (build-149).
        if (s.name.find("Body") != std::string::npos) score += 40;
        if (s.name.find("Face") != std::string::npos ||
            s.name.find("Hair") != std::string::npos ||
            s.name.find("Weapon") != std::string::npos ||
            s.name.find("Cloak") != std::string::npos ||
            s.name.find("Tui") != std::string::npos) score -= 60;

        // Ancestor chain names: the player character usually hangs under a
        // PlayerRoot / Player / Hero node. A "player" hit is worth more than
        // every geometric signal combined.
        char chain[320];
        read_chain(tr, mParent, mName, chain, sizeof(chain));
        {
            char low[320];
            int ln = 0;
            for (const char* p = chain; *p && ln < (int)sizeof(low) - 1; p++)
                low[ln++] = (char)tolower((unsigned char)*p);
            low[ln] = 0;
            if (strstr(low, "player") || strstr(low, "hero") || strstr(low, "mainrole")) score += 200;

            // 对象池排除。
            //
            // 游戏把没上场的模型全丢在 /VisualModelPool/VisualPool 下，
            // 位置停在 (5000,5000,5000)。它们 activeInHierarchy 是 1
            // （池子本身是激活的），但位置在角落、骨骼根本不更新 ——
            // 之前"锚到对面角色"就是这个：锚中的是池里的模板。
            //
            // 实测（b175 那次）：
            //   #7 '11013_Body' world=(5000,5000,5000) chain=/11013_Body/11013_U_1/VisualModelPool/VisualPool
            //   #24 'Body'      world=(0,0,0)          chain=/Body/11021_U_1/EmptyObject   <- 场上的
            if (strstr(low, "visualmodelpool") || strstr(low, "visualpool")) {
                L("[CENSUS] #%zu '%s' 在对象池里 -> 排除", i, s.name.c_str());
                continue;
            }
        }

        // 骨骼名硬门槛：数一下有多少根骨骼是 'Bip001' / 'BN_' 开头的。
        // 对不上就一根都不是，再近也不能当玩家角色。
        // 只对分数已经像样的候选付这个开销（每根骨骼一次 invoke）。
        int known = -1;
        if (score >= 40) {
            known = count_known_bones(s.obj, mBones, mName);
            if (known < 5) {
                L("[CENSUS] #%zu '%s' score=%d 但只有 %d 根认识的骨骼 -> 排除",
                  i, s.name.c_str(), score, known);
                continue;
            }
            score += known * 2;   // 认识得越多越可信
        }

        L("[CENSUS] #%zu '%s' bones=%d known=%d active=%d score=%d world=(%.1f,%.1f,%.1f) cam=(%.1f,%.1f,%.1f) anc=%d chain=%s",
          i, s.name.c_str(), s.boneCount, known, (int)active, score,
          wx, wy, wz, cx, cy, cz, shareAnc, chain);

        push_top((int)i, score);
        // 门槛 30 -> 20：场上的玩家可能是 26 根的简化骨架（对象池里那些
        // 完整模板反倒 66/73 根）。位置对了比骨骼多更重要。
        if (s.boneCount >= 20 && score > bestScore) { bestScore = score; best = (int)i; }
    }

    // ---------------------------------------------------------------
    // 手动指定锚点：<gamefiles>/mmd_pin 里放一个 SMR 索引（就是上面 [CENSUS] 打的 #N）。
    // 优先级最高，跳过所有打分。自动选挑错了，或者想钉死某个角色时用它。
    // 每次 census 都会读这个文件，想恢复自动就把它删掉。
    // ---------------------------------------------------------------
    {
        FILE* pf = fopen("/data/data/com.hermes.h1game/files/mmd_pin", "r");
        if (pf) {
            int want = -1;
            const int got = fscanf(pf, "%d", &want);
            fclose(pf);
            if (got == 1 && want >= 0 && want < (int)g_smrs.size()) {
                L("[CENSUS] mmd_pin -> force anchor #%d '%s' bones=%d",
                  want, g_smrs[(size_t)want].name.c_str(), g_smrs[(size_t)want].boneCount);
                best = want;
                bestScore = 9999;
            } else {
                L("[CENSUS] mmd_pin invalid (got=%d want=%d smr=%zu)",
                  got, want, g_smrs.size());
            }
        }
    }

    for (int i = 0; i < 10 && top[i].score > 0; i++) {
        L("[CENSUS] TOP%d #%d '%s' score=%d", i + 1, top[i].idx,
          g_smrs[(size_t)top[i].idx].name.c_str(), top[i].score);
    }

    if (best < 0) {
        L("[CENSUS] no candidate (SMR=%zu)", g_smrs.size());
        return;
    }
    // Score gate: a pooled/inactive scene scores < 25. Require 60 so we never
    // anchor onto a dummy object again (build-145 picked a random Body at 25).
    if (bestScore < 60) {
        L("[CENSUS] best score %d < 60 -> scene has no active player, skip anchor "
          "(enter battle/lobby with your character visible, then re-run)", bestScore);
        return;
    }

    // ---- 锁定 + 钉骨骼 ----
    g_anchorBones.clear();
    g_anchorIdx = best;
    g_anchorScore = bestScore;
    g_anchorSmr = g_smrs[(size_t)best].obj;
    g_anchorTr  = mGetTr ? invoke(mGetTr, g_anchorSmr, nullptr) : nullptr;

    void* arr = mBones ? invoke(mBones, g_anchorSmr, nullptr) : nullptr;
    if (!arr) { L("[ANCHOR] get_bones 返回空"); return; }
    const int32_t n = *(int32_t*)((char*)arr + 24);
    L("[ANCHOR] 锁定 #%d '%s' bones=%d score=%d", best,
      g_smrs[(size_t)best].name.c_str(), n, bestScore);

    g_anchorBones.reserve((size_t)n);
    for (int32_t k = 0; k < n && k < 512; k++) {
        void* tr = *(void**)((char*)arr + 32 + (size_t)k * 8);
        AnchorBone b;
        b.tr = tr;
        if (tr) {
            void* r = mId ? invoke(mId, tr, nullptr) : nullptr;
            b.id = r ? *(int32_t*)((char*)r + 16) : 0;
            read_name(tr, mName, b.name, sizeof(b.name));
            if (g_api.gchandle_new) b.handle = g_api.gchandle_new((Il2CppObject*)tr, true);
        }
        L("[ANCHOR]   [%d] '%s' id=%d tr=%p h=%u", k, b.name, b.id, b.tr, b.handle);
        g_anchorBones.push_back(b);
    }
    L("[ANCHOR] done: %zu bones pinned", g_anchorBones.size());

    // ---------------------------------------------------------------
    // 合并同一角色的兄弟 SMR 骨骼。
    //
    // 这个游戏把一个角色拆成多个 SMR：
    //   /Body/11021_U_1  26 根   /Body_a/11021_U_1  28 根
    //   /Shou_L/11021_U_1 2 根   /Tui_R/11021_U_1   2 根  ...
    // 每个 SMR 的 bones 数组只装自己那部分骨骼，所以只锚 Body 就是缺手缺腿
    // （实测 11/51 对，game missing 40）。
    //
    // 用父链里的「角色实例名」（如 11021_U_1）做匹配，把同一实例下所有
    // SMR 的骨骼并进来，按名字去重。
    // ---------------------------------------------------------------
    if (mBones && mName && mParent && mGetTr && g_anchorTr) {
        char mainChain[320];
        read_chain(g_anchorTr, mParent, mName, mainChain, sizeof(mainChain));

        char key[64] = {0};
        {
            const char* p = mainChain;
            int slash = 0;
            while (*p) {
                if (*p == '/') {
                    slash++;
                    if (slash == 2) {
                        const char* q = p + 1;
                        int k = 0;
                        while (*q && *q != '/' && k < (int)sizeof(key) - 1) key[k++] = *q++;
                        key[k] = 0;
                        break;
                    }
                }
                p++;
            }
        }

        if (key[0]) {
            int merged = 0;
            for (size_t j = 0; j < g_smrs.size(); j++) {
                if ((int)j == best) continue;
                SmrInfo& sib = g_smrs[j];
                if (!sib.obj || !sib.native) continue;

                void* st = invoke(mGetTr, sib.obj, nullptr);
                if (!st) continue;
                char sc[320];
                read_chain(st, mParent, mName, sc, sizeof(sc));
                if (!strstr(sc, key)) continue;

                void* arr2 = invoke(mBones, sib.obj, nullptr);
                if (!arr2) continue;
                const int32_t n2 = *(int32_t*)((char*)arr2 + 24);

                // 诊断：这个兄弟 SMR 带来了哪些骨骼（只取前 3 根看命名规律）
                {
                    char samp[200] = {0};
                    for (int32_t k = 0; k < n2 && k < 3; k++) {
                        void* bt = *(void**)((char*)arr2 + 32 + (size_t)k * 8);
                        char nm[64] = {0};
                        if (bt) read_name(bt, mName, nm, sizeof(nm));
                        char tmp[72];
                        snprintf(tmp, sizeof(tmp), "%s%s", k ? "," : "", nm);
                        strncat(samp, tmp, sizeof(samp) - strlen(samp) - 1);
                    }
                    L("[CENSUS]   +SMR '%s' bones=%d sample=[%s]",
                      sib.name.c_str(), n2, samp);
                }

                for (int32_t k = 0; k < n2 && k < 512; k++) {
                    void* bt = *(void**)((char*)arr2 + 32 + (size_t)k * 8);
                    if (!bt) continue;
                    char nm[64];
                    read_name(bt, mName, nm, sizeof(nm));
                    if (!nm[0]) continue;
                    bool dup = false;
                    for (size_t q = 0; q < g_anchorBones.size(); q++) {
                        if (strcmp(g_anchorBones[q].name, nm) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    AnchorBone b;
                    b.tr = bt;
                    void* r = mId ? invoke(mId, bt, nullptr) : nullptr;
                    b.id = r ? *(int32_t*)((char*)r + 16) : 0;
                    snprintf(b.name, sizeof(b.name), "%s", nm);
                    if (g_api.gchandle_new) b.handle = g_api.gchandle_new((Il2CppObject*)bt, true);
                    g_anchorBones.push_back(b);
                    merged++;
                }
            }
            L("[CENSUS] 角色实例 '%s'：合并兄弟 SMR 骨骼 %d 根，总 %zu 根",
              key, merged, g_anchorBones.size());
        }
    }

    build_retarget();   // anchor changed -> rebuild the Bip001 -> MMD map
}

// =====================================================================
// Retarget: copy game bone world rotations onto our own MMD bones.
// Same idea as ModelReplacementAPI (boneMap + rotation copy + per-bone offset).
// Table order must be top-down (parent before child) because setting a
// parent's world rotation moves its children.
// =====================================================================
// =====================================================================
// 四元数小工具（Unity 顺序 x,y,z,w）
// =====================================================================
static void qmul(const float a[4], const float b[4], float out[4]) {
    const float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    const float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    const float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    const float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
}
static void qinv(const float q[4], float out[4]) {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}
static void quat_norm(float q[4]) {
    const float n = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n < 1e-6f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n;
}
// 行主序 3x3 -> 四元数
static void mat3_to_quat(const float m[9], float q[4]) {
    const float tr = m[0] + m[4] + m[8];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[7] - m[5]) / s;
        q[1] = (m[2] - m[6]) / s;
        q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[7] - m[5]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[3]) / s;
        q[2] = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[2] - m[6]) / s;
        q[0] = (m[1] + m[3]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[5] + m[7]) / s;
    } else {
        const float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[3] - m[1]) / s;
        q[0] = (m[2] + m[6]) / s;
        q[1] = (m[5] + m[7]) / s;
        q[2] = 0.25f * s;
    }
    quat_norm(q);
}

// 游戏侧每根骨骼在 bind 时的世界旋转（从 bindposes 反算）
static float g_gameBindRot[512][4];
static int   g_gameBindRotN = 0;

// 读 SkinnedMeshRenderer.sharedMesh.bindposes，算出每根骨骼 bind 时的世界旋转。
//
// Unity 约定：bindposes[i] 把 mesh 空间变到骨骼空间（bind 时刻），所以
//     骨骼bind世界矩阵 = renderer世界矩阵 * bindposes[i]⁻¹
// 旋转部分：R = R_renderer * R_bpᵀ
//
// 矩阵在 il2cpp 里是值类型，数组元素内联（Matrix4x4 = 16 float = 64 字节），
// 而且 Unity 的 Matrix4x4 是列主序：raw[col*4 + row]。
// 所以左上 3x3 的转置按行主序读就是 raw 的 {0,1,2, 4,5,6, 8,9,10}。
// 注意：直接用锚定的那个 SMR 对象现拿 sharedMesh —— 不要用 g_smrs[g_anchorIdx]，
// 因为 scan_smrs 重建以后索引会挪到别的 SMR 上（实测锚到 'Tui_L' / 'Weapon_03_L'
// 时拿到的是别人家的 bindposes，offset 全错）。
static bool read_bind_rotations(void* smrObj, int boneCount) {
    if (!smrObj || boneCount <= 0 || boneCount > 512) {
        L("[RT] bindposes 读取跳过（smr=%p bones=%d）", smrObj, boneCount);
        return false;
    }
    Il2CppClass* meshCls = find_class_any("UnityEngine", "Mesh");
    if (!meshCls || !g_smrClass) return false;
    const MethodInfo* mGetMesh = find_method_any(g_smrClass, "get_sharedMesh");
    const MethodInfo* mBp      = find_method_any(meshCls, "get_bindposes");
    if (!mGetMesh) { L("[RT] SMR.get_sharedMesh 缺失"); return false; }
    if (!mBp)      { L("[RT] Mesh.get_bindposes 缺失"); return false; }

    void* meshObj = invoke(mGetMesh, smrObj, nullptr);
    if (!meshObj) { L("[RT] get_sharedMesh 返回 null"); return false; }

    void* arr = invoke(mBp, meshObj, nullptr);
    if (!arr) { L("[RT] get_bindposes 返回 null"); return false; }
    const int32_t n = *(int32_t*)((char*)arr + 24);
    if (n < boneCount) {
        L("[RT] bindposes 数量不足 %d < %d", n, boneCount);
        return false;
    }

    // renderer 自身的世界旋转（bindpose 是相对它的）
    float qR[4] = {0, 0, 0, 1};
    {
        Il2CppClass* compCls = find_class_any("UnityEngine", "Component");
        Il2CppClass* trCls   = find_class_any("UnityEngine", "Transform");
        const MethodInfo* mGetTr  = compCls ? find_method_any(compCls, "get_transform") : nullptr;
        const MethodInfo* mGetRot = trCls   ? find_method_any(trCls, "get_rotation") : nullptr;
        void* tr = (mGetTr && smrObj) ? invoke(mGetTr, smrObj, nullptr) : nullptr;
        void* r  = (tr && mGetRot) ? invoke(mGetRot, tr, nullptr) : nullptr;
        if (r) memcpy(qR, (char*)r + 16, sizeof(qR));
        quat_norm(qR);
    }

    char* base = (char*)arr + 32;
    for (int i = 0; i < boneCount; i++) {
        float raw[16];
        memcpy(raw, base + (size_t)i * 64, sizeof(raw));
        if (fabsf(raw[15] - 1.0f) > 0.01f || fabsf(raw[12]) > 1e5f) {
            L("[RT] bindposes 格式不对 #%d raw[15]=%.3f raw[12]=%.1f，放弃",
              i, raw[15], raw[12]);
            return false;
        }
        const float m[9] = { raw[0], raw[1], raw[2],
                             raw[4], raw[5], raw[6],
                             raw[8], raw[9], raw[10] };
        float qBpInv[4];
        mat3_to_quat(m, qBpInv);
        qmul(qR, qBpInv, g_gameBindRot[i]);
        quat_norm(g_gameBindRot[i]);
    }
    g_gameBindRotN = boneCount;
    L("[RT] bind poses 读完 %d 根（rendererRot=(%.3f,%.3f,%.3f,%.3f)）",
      boneCount, qR[0], qR[1], qR[2], qR[3]);
    return true;
}

struct RetargetPair {
    int game;
    int mmd;
    const char* g;
    const char* m;
    // offset = qinv(gameBindWorldRot) * mmdBindWorldRot
    // 运行时：mmdWorldRot = gameWorldRot * offset
    // 这样 game 回到 bind 时，mmd 正好回到 MMD 自己的 bind。
    float off[4] = {0, 0, 0, 1};
    bool  offValid = false;
};
static std::vector<RetargetPair> g_pairs;

// g_model / g_load are defined further down; access through forward-declared getters.
static mmd::Model& model_ref();
static game::LoadResult& load_ref();
static int g_rtCenterGame = -1;   // Bip001 Pelvis
static int g_rtCenterMmd  = -1;   // center bone for position follow

// MMD names are written as UTF-8 hex escapes so this file stays pure ASCII.
static const struct { const char* g; const char* m; } kRetargetMap[] = {
    { "Bip001 Pelvis",    "\xE4\xB8\x8B\xE5\x8D\x8A\xE8\xBA\xAB" },              // lower body
    { "Bip001 Spine",     "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB" },              // upper body
    { "Bip001 Spine1",    "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB" "2" },           // upper body 2
    { "Bip001 Neck",      "\xE9\xA6\x96" },                                        // neck
    { "Bip001 Head",      "\xE9\xA0\xAD" },                                        // head
    { "Bip001 L Clavicle","\xE5\xB7\xA6\xE8\x82\xA9" },                          // L shoulder
    { "Bip001 R Clavicle","\xE5\x8F\xB3\xE8\x82\xA9" },                          // R shoulder
    { "Bip001 L UpperArm","\xE5\xB7\xA6\xE8\x85\x95" },                          // L arm
    { "Bip001 R UpperArm","\xE5\x8F\xB3\xE8\x85\x95" },                          // R arm
    { "Bip001 L Forearm", "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x98" },             // L elbow
    { "Bip001 R Forearm", "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98" },             // R elbow
    { "Bip001 L Hand",    "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96" },             // L wrist
    { "Bip001 R Hand",    "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96" },             // R wrist
    { "Bip001 L Thigh",   "\xE5\xB7\xA6\xE8\xB6\xB3" },                          // L leg
    { "Bip001 R Thigh",   "\xE5\x8F\xB3\xE8\xB6\xB3" },                          // R leg
    { "Bip001 L Calf",    "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x96" },             // L knee
    { "Bip001 R Calf",    "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x96" },             // R knee
    { "Bip001 L Foot",    "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96" },             // L ankle
    { "Bip001 R Foot",    "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96" },             // R ankle
    { "Bip001 L Toe0",    "\xE5\xB7\xA6\xE3\x81\xA4\xE3\x81\xBE\xE5\x85\x88" },// L toe
    { "Bip001 R Toe0",    "\xE5\x8F\xB3\xE3\x81\xA4\xE3\x81\xBE\xE5\x85\x88" },// R toe

    // ---- 手指 ----
    // Biped 命名规则：Finger0 = 拇指（0/01/02 三节），
    //                 Finger1..4 = 食指/中指/无名指/小指（各有 根/中/尖 三节）。
    // MMD 侧：親指０/１/２（注意拇指从 0 开始），其余 １/２/３。
    // 模型里没有的骨骼 find_mmd_bone_by_name 会返回 -1，直接跳过，不影响别的。
    // ---- 手指 L ----
    { "Bip001 L Finger0", "\xE5\xB7\xA6\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x90" },
    { "Bip001 L Finger01", "\xE5\xB7\xA6\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 L Finger02", "\xE5\xB7\xA6\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 L Finger1", "\xE5\xB7\xA6\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 L Finger11", "\xE5\xB7\xA6\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 L Finger12", "\xE5\xB7\xA6\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 L Finger2", "\xE5\xB7\xA6\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 L Finger21", "\xE5\xB7\xA6\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 L Finger22", "\xE5\xB7\xA6\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 L Finger3", "\xE5\xB7\xA6\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 L Finger31", "\xE5\xB7\xA6\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 L Finger32", "\xE5\xB7\xA6\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 L Finger4", "\xE5\xB7\xA6\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 L Finger41", "\xE5\xB7\xA6\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 L Finger42", "\xE5\xB7\xA6\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x93" },
    // ---- 手指 R ----
    { "Bip001 R Finger0", "\xE5\x8F\xB3\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x90" },
    { "Bip001 R Finger01", "\xE5\x8F\xB3\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 R Finger02", "\xE5\x8F\xB3\xE8\xA6\xAA\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 R Finger1", "\xE5\x8F\xB3\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 R Finger11", "\xE5\x8F\xB3\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 R Finger12", "\xE5\x8F\xB3\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 R Finger2", "\xE5\x8F\xB3\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 R Finger21", "\xE5\x8F\xB3\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 R Finger22", "\xE5\x8F\xB3\xE4\xB8\xAD\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 R Finger3", "\xE5\x8F\xB3\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 R Finger31", "\xE5\x8F\xB3\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 R Finger32", "\xE5\x8F\xB3\xE8\x96\xAC\xE6\x8C\x87\xEF\xBC\x93" },
    { "Bip001 R Finger4", "\xE5\x8F\xB3\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x91" },
    { "Bip001 R Finger41", "\xE5\x8F\xB3\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x92" },
    { "Bip001 R Finger42", "\xE5\x8F\xB3\xE5\xB0\x8F\xE6\x8C\x87\xEF\xBC\x93" },
};

static int find_anchor_by_name(const char* n) {
    for (size_t i = 0; i < g_anchorBones.size(); i++)
        if (strcmp(g_anchorBones[i].name, n) == 0) return (int)i;
    return -1;
}
static int find_mmd_bone_by_name(const char* n) {
    mmd::Model& m = model_ref();
    for (size_t i = 0; i < m.bones.size(); i++)
        if (m.bones[i].name == n) return (int)i;
    return -1;
}

static void build_retarget() {
    g_pairs.clear();
    g_rtCenterGame = g_rtCenterMmd = -1;
    if (g_anchorBones.empty()) { L("[RT] no anchored game bones yet"); return; }
    if (model_ref().bones.empty()) { L("[RT] model bones not parsed yet"); return; }

    int ok = 0, missG = 0, missM = 0;
    for (size_t i = 0; i < sizeof(kRetargetMap) / sizeof(kRetargetMap[0]); i++) {
        int gi = find_anchor_by_name(kRetargetMap[i].g);
        int mi = find_mmd_bone_by_name(kRetargetMap[i].m);
        if (gi < 0) { missG++; continue; }
        if (mi < 0) { missM++; L("[RT] mmd bone missing: %s", kRetargetMap[i].m); continue; }
        g_pairs.push_back({gi, mi, kRetargetMap[i].g, kRetargetMap[i].m});
        ok++;
    }
    g_rtCenterGame = find_anchor_by_name("Bip001 Pelvis");
    g_rtCenterMmd  = find_mmd_bone_by_name("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"); // center
    L("[RT] map built: %d/%zu pairs (game missing %d, mmd missing %d) centerG=%d centerM=%d",
      ok, sizeof(kRetargetMap) / sizeof(kRetargetMap[0]), missG, missM,
      g_rtCenterGame, g_rtCenterMmd);

    // =================================================================
    // bind pose 补偿
    //
    // 两套骨架的骨骼初始朝向完全不一样（Bip001 每根都有自己的轴，
    // MMD 的骨骼默认都是 identity 局部旋转）。直接抄世界旋转，
    // 等于给每根骨骼多转了它的 bind 朝向那么多 —— 模型就是拧着的。
    //
    // offset[i] = qinv(gameBindRot[i]) * mmdBindRot[i]
    // runtime   : mmdWorldRot = gameWorldRot * offset[i]
    //
    // 验证：game 回到 bind → mmd = gameBind * qinv(gameBind) * mmdBind = mmdBind ✓
    // 这就是 Unity.Animation.RigRemap / reze-rig / mmd-bone-tools 都在用的做法。
    // =================================================================
    {
        bool offOk = false;
        if (g_anchorSmr) {
            offOk = read_bind_rotations(g_anchorSmr, (int)g_anchorBones.size());
        }
        if (!offOk) {
            // 降级：拿不到 bindposes 就把当前姿势当 bind（角色通常在站桩，误差可以接受）
            L("[RT] 降级：拿不到 bindposes，用当前姿势当参考");
            Il2CppClass* trCls = find_class_any("UnityEngine", "Transform");
            const MethodInfo* mGetRot = trCls ? find_method_any(trCls, "get_rotation") : nullptr;
            if (mGetRot) {
                for (size_t i = 0; i < g_anchorBones.size() && i < 512; i++) {
                    void* gt = g_anchorBones[i].tr;
                    void* r = gt ? invoke(mGetRot, gt, nullptr) : nullptr;
                    if (r) memcpy(g_gameBindRot[i], (char*)r + 16, 16);
                    else { g_gameBindRot[i][0] = g_gameBindRot[i][1] = g_gameBindRot[i][2] = 0.0f;
                           g_gameBindRot[i][3] = 1.0f; }
                    quat_norm(g_gameBindRot[i]);
                }
                g_gameBindRotN = (int)g_anchorBones.size();
                offOk = true;
            }
        }

        if (offOk) {
            Il2CppClass* trCls = find_class_any("UnityEngine", "Transform");
            const MethodInfo* mGetRot = trCls ? find_method_any(trCls, "get_rotation") : nullptr;
            // 样本：确认 MMD 骨骼起始确实是世界轴对齐（PMX 骨骼没有旋转字段，
            // 自建 skeleton 局部旋转全是 identity，所以我们显式 set_rotation 的时候
            // “bind” 就是 identity）
            if (!g_pairs.empty()) {
                void* mt0 = load_ref().skeleton.transform_at(g_pairs[0].mmd);
                float mb[4] = {0, 0, 0, 1};
                void* r = (mt0 && mGetRot) ? invoke(mGetRot, mt0, nullptr) : nullptr;
                if (r) memcpy(mb, (char*)r + 16, 16);
                L("[RT] MMD bind 样本 '%s' = (%.3f,%.3f,%.3f,%.3f)（期望近似 identity）",
                  g_pairs[0].m, mb[0], mb[1], mb[2], mb[3]);
            }

            int withOff = 0;
            for (auto& p : g_pairs) {
                if (p.game < 0 || p.game >= g_gameBindRotN) continue;
                // MMD 侧 bind = identity
                // → offset = qinv(gameBind) * identity = qinv(gameBind)
                qinv(g_gameBindRot[p.game], p.off);
                quat_norm(p.off);
                p.offValid = true;
                withOff++;
            }
            L("[RT] bind-offset 算好 %d/%zu 对", withOff, g_pairs.size());
            for (size_t i = 0; i < g_pairs.size() && i < 2; i++) {
                if (!g_pairs[i].offValid) continue;
                const float* gb = g_gameBindRot[g_pairs[i].game];
                L("[RT]   '%s' gbind=(%.2f,%.2f,%.2f,%.2f) off=(%.2f,%.2f,%.2f,%.2f)",
                  g_pairs[i].g, gb[0], gb[1], gb[2], gb[3],
                  g_pairs[i].off[0], g_pairs[i].off[1], g_pairs[i].off[2], g_pairs[i].off[3]);
            }
        } else {
            L("[RT] bind-offset 完全拿不到，退回直抄（模型会拧）");
        }
    }
}

static void retarget_update() {
    if (g_pairs.empty() || !load_ref().ok) return;
    Il2CppClass* trCls = find_class_any("UnityEngine", "Transform");
    if (!trCls) return;
    static const MethodInfo* sGetRot = nullptr;
    static const MethodInfo* sSetRot = nullptr;
    static const MethodInfo* sSetPos = nullptr;
    static const MethodInfo* sGetPos = nullptr;
    static bool sTried = false;
    static const MethodInfo* sGetBones = nullptr;
    if (!sTried) {
        sTried = true;
        sGetRot = find_method_any(trCls, "get_rotation");
        sSetRot = find_method_any(trCls, "set_rotation");
        sSetPos = find_method_any(trCls, "set_position");
        sGetPos = find_method_any(trCls, "get_position");
        sGetBones = g_smrClass ? find_method_any(g_smrClass, "get_bones") : nullptr;
        L("[RT] methods getRot=%p setRot=%p getPos=%p setPos=%p getBones=%p atr=%p mroot=%p",
          (void*)sGetRot, (void*)sSetRot, (void*)sGetPos, (void*)sSetPos,
          (void*)sGetBones, g_anchorTr, load_ref().modelRoot);
    }
    if (!sGetRot || !sSetRot) return;

    // 这一帧骨骼旋转是不是从 native 直读拿到的（RTDIAG 要打）
    bool rotNat = false;

    // 每帧重新拿一次骨骼数组。
    // 游戏的对象池会重建 SMR 的 bones，缓存的 Transform 可能指向已经停用
    // 的旧对象 —— 表现就是模型挂着但摆着不动（这是 21 对全死的最可能原因）。
    void* boneArr = (sGetBones && g_anchorSmr) ? invoke(sGetBones, g_anchorSmr, nullptr) : nullptr;
    const int32_t boneN = boneArr ? *(int32_t*)((char*)boneArr + 24) : 0;

    for (size_t i = 0; i < g_pairs.size(); i++) {
        RetargetPair& p = g_pairs[i];
        if (p.game < 0) continue;
        void* gt = nullptr;
        if (boneArr && p.game < boneN) {
            gt = *(void**)((char*)boneArr + 32 + (size_t)p.game * 8);
        }
        if (!gt && p.game < (int)g_anchorBones.size()) {
            gt = g_anchorBones[(size_t)p.game].tr;   // 退回缓存
        }
        void* mt = load_ref().skeleton.transform_at(p.mmd);
        if (!gt || !mt) continue;
        // 先调一次 managed getter：既能确认对象还活着（避免拿着野指针去读
        // native 内存），也是 native 读失败时的兜底。
        void* q = invoke(sGetRot, gt, nullptr);

        // 旋转优先 native 直读。
        // build-169 实测：managed get_rotation/get_position 在骨骼 Transform 上
        // 读回来是一组恒定不动的值（用户走了也一样），而相机那边一直靠 native
        // 直读才准。同一个 Transform 结构，骨骼没理由不一样。
        float quat[4];
        rotNat = game::cam_native::get_world_rot(gt, quat);
        if (!rotNat) {
            if (!q) continue;
            memcpy(quat, (char*)q + 16, sizeof(quat));
        }

        // bind pose 补偿：mmdWorldRot = gameWorldRot * offset
        if (p.offValid) {
            float r[4];
            qmul(quat, p.off, r);
            quat_norm(r);
            memcpy(quat, r, sizeof(quat));
        }

        void* a[1] = { quat };
        invoke(sSetRot, mt, a);
        game::skin::set_bone_world_rot(p.mmd, quat);   // feed CPU skinning
    }

    // 模型根只跟**位置**，不跟旋转。
    //
    // 为什么：游戏角色是 3ds Max Biped 导进来的，SMR 的 Transform 上带着
    // -90°（Max 的 Z-up → Unity 的 Y-up）修正。以前把 modelRoot.rotation
    // 直接设成 g_anchorTr.rotation，等于给 Y-up 的 MMD 模型又转了一次
    // -90°，模型就躺下了。
    //
    // 朝向不用管：每根骨骼的世界旋转本来就从游戏骨骼抄过来，角色转身
    // 的时候骨骼自己在转，整模型跟着转，不需要根节点再补一次。
    if (g_anchorTr && load_ref().modelRoot && sGetPos && sSetPos) {
        void* p = invoke(sGetPos, g_anchorTr, nullptr);
        if (p) {
            float v[3];
            memcpy(v, (char*)p + 16, sizeof(v));
            void* a[1] = { v };
            invoke(sSetPos, load_ref().modelRoot, a);
        }
    }

    // Diagnostic: every ~2s print the first driven bone's quaternion and the
    // first skinned vertex, so we can tell "rotations never change" (wrong
    // anchor) from "rotations change but math is wrong".
    {
        static int sDiagN = 0;
        if ((++sDiagN % 120) == 1 && !g_pairs.empty()) {
            RetargetPair& p0 = g_pairs[0];
            const bool okIdx = (p0.game >= 0 && p0.game < (int)g_anchorBones.size());
            void* gt0 = okIdx ? g_anchorBones[(size_t)p0.game].tr : nullptr;
            void* q0 = (gt0 && sGetRot) ? invoke(sGetRot, gt0, nullptr) : nullptr;
            float qq[4] = {0, 0, 0, 0};
            if (q0) memcpy(qq, (char*)q0 + 16, sizeof(qq));

            // 写进 MMD 骨骼之后读回来，用来判断「补偿到底生没生效」
            float out[4] = {0, 0, 0, 0};
            void* mt0 = load_ref().skeleton.transform_at(p0.mmd);
            void* ro = (mt0 && sGetRot) ? invoke(sGetRot, mt0, nullptr) : nullptr;
            if (ro) memcpy(out, (char*)ro + 16, sizeof(out));

            // 位置诊断：角色走动时 apos/ppos 必须变；mroot 应该跟着 apos 走。
            float ap[3] = {0, 0, 0}, pp[3] = {0, 0, 0}, mp[3] = {0, 0, 0};
            bool apNat = false, ppNat = false;
            if (g_anchorTr) {
                apNat = game::cam_native::get_world_pos(g_anchorTr, ap);
                if (!apNat && sGetPos) {
                    void* apo = invoke(sGetPos, g_anchorTr, nullptr);
                    if (apo) memcpy(ap, (char*)apo + 16, sizeof(ap));
                }
            }
            if (gt0) {
                ppNat = game::cam_native::get_world_pos(gt0, pp);
                if (!ppNat && sGetPos) {
                    void* ppo = invoke(sGetPos, gt0, nullptr);
                    if (ppo) memcpy(pp, (char*)ppo + 16, sizeof(pp));
                }
            }
            void* mpo = (load_ref().modelRoot && sGetPos)
                        ? invoke(sGetPos, load_ref().modelRoot, nullptr) : nullptr;
            if (mpo) memcpy(mp, (char*)mpo + 16, sizeof(mp));

            L("[RTDIAG] pairs=%zu rotNat=%d posNat=%d/%d g=(%.2f,%.2f,%.2f,%.2f) off=%d mmd=(%.2f,%.2f,%.2f,%.2f) apos=(%.1f,%.1f,%.1f) ppos=(%.1f,%.1f,%.1f) mroot=(%.1f,%.1f,%.1f)",
              g_pairs.size(), (int)rotNat, (int)apNat, (int)ppNat,
              qq[0], qq[1], qq[2], qq[3],
              (int)p0.offValid, out[0], out[1], out[2], out[3],
              ap[0], ap[1], ap[2], pp[0], pp[1], pp[2], mp[0], mp[1], mp[2]);
        }
    }

    // Recompute skinned vertices and upload them to the MeshRenderer mesh.
    game::skin::update();

    // Position now comes from the model ROOT (see above); moving the center
    // bone as well would double-shift. Keep it disabled.
    if (false && sGetPos && sSetPos && g_rtCenterGame >= 0 && g_rtCenterMmd >= 0) {
        void* gt = g_anchorBones[(size_t)g_rtCenterGame].tr;
        void* mt = load_ref().skeleton.transform_at(g_rtCenterMmd);
        if (gt && mt) {
            void* p = invoke(sGetPos, gt, nullptr);
            if (p) {
                float v[3];
                memcpy(v, (char*)p + 16, sizeof(v));
                void* a[1] = { v };
                invoke(sSetPos, mt, a);
            }
        }
    }
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
        L("[UNITY] #%zu name='%s' native=%p bones=%d(sizeRaw=%d) data=%p meshID=%d(raw@meshOff=%d) %s",
          i, s.name.c_str(), s.native, s.boneCount, s.boneSizeRaw, s.boneData,
          s.meshId, s.meshIdRaw,
          !canMatch ? "?" : (s.meshIdRaw == s.meshId ? "OFFSET-OK" : "OFFSET-MISMATCH"));
    }
    L("[UNITY] offset check: %d / %d match (%s)", offMatch, offTotal,
      offTotal && offMatch == offTotal ? "PASS" : "FAIL");

    // =============================================================
    // 火力覆盖：全场景骨骼普查 + 玩家骨骼锚定
    //
    // 之前的问题：只采了一个 SMR 的名字（而且是 Face），躯干那套 BN_* 一条没有。
    // 现在：
    //   1) 遍历所有 SMR，每个都打一把名字（带 instanceID 和世界坐标）
    //   2) 从中挑“玩家主控”（激活 + 离主相机最近 + 骨骼数够）
    //   3) 把它的骨骼 Transform 全部 gchandle 钉住 —— 这就是锚点，
    //      后面翻译层直接用这份数组，不用每帧重新 get_bones
    // =============================================================
    census_and_anchor();

    if (!game::name_hook_installed()) {
        game::install_name_hook(64);
    }

    // ---- C 方案前置：自建渲染对象的 API 探测（只读，不改任何流程）----
    {
        Il2CppClass* go  = find_class_any("UnityEngine", "GameObject");
        Il2CppClass* mf  = find_class_any("UnityEngine", "MeshFilter");
        Il2CppClass* mr  = find_class_any("UnityEngine", "MeshRenderer");
        L("[RENDERAPI] GameObject=%p MeshFilter=%p MeshRenderer=%p SMR=%p",
          (void*)go, (void*)mf, (void*)mr, (void*)g_smrClass);

        if (go) {
            void* iter = nullptr;
            const MethodInfo* m;
            int n = 0;
            while ((m = g_api.class_get_methods(go, &iter))) {
                const char* nm = g_api.method_get_name(m);
                if (!nm || strcmp(nm, "AddComponent") != 0) continue;
                n++;
                uint32_t pc = g_api.method_get_param_count(m);
                const char* p0 = "?";
                if (pc >= 1) {
                    const Il2CppType* t = g_api.method_get_param(m, 0);
                    Il2CppClass* k = t ? g_api.class_from_type(t) : nullptr;
                    if (k) p0 = g_api.class_get_name(k);
                }
                L("[RENDERAPI]   AddComponent #%d params=%u p0=%s", n, pc, p0 ? p0 : "?");
            }
            if (n == 0) L("[RENDERAPI]   AddComponent: 一个重载都没有（被 strip）");
        }
        if (mf) {
            L("[RENDERAPI] MeshFilter set_mesh=%p get_mesh=%p",
              uo::u_method(mf, "set_mesh", 1), uo::u_method(mf, "get_mesh", 0));
        }
        if (mr) {
            L("[RENDERAPI] MeshRenderer set_material=%p set_materials=%p",
              uo::u_method(mr, "set_material", 1), uo::u_method(mr, "set_sharedMaterials", 1));
            L("[RENDERAPI] MeshRenderer set_enabled=%p",
              uo::u_method(mr, "set_enabled", 1));
        }
        L("[RENDERAPI] SMR ctor(0)=%p ctor(1)=%p",
          uo::u_method_own(g_smrClass, ".ctor", 0),
          uo::u_method_own(g_smrClass, ".ctor", 1));
    }

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

    int32_t* pA = (int32_t*)((char*)A.native + game::layout().offMesh);
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

    void*  dataPtr = *(void**)(base + game::layout().offBoneData);
    int32_t sizeVal = *(int32_t*)(base + game::layout().offBoneSize);
    L("[BONE] data=%p  size@0x%x=%d", dataPtr, (unsigned)game::layout().offBoneSize, sizeVal);

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
    int32_t c2 = *(int32_t*)(base + game::layout().offBoneCap);
    int32_t c3 = *(int32_t*)(base + 0x1e8);
    // +0x1f8 是 int64 capacity（实测 144 = 2×72，标准 vector 扩容策略）
    int64_t cap64 = *(int64_t*)(base + game::layout().offBoneCap);
    L("[BONE] size@0x%x=%d  capacity@0x%x=%lld",
      (unsigned)game::layout().offBoneSize, sizeVal,
      (unsigned)game::layout().offBoneCap, (long long)cap64);
    L("[BONE] verdict: need 224, have cap %lld -> %s",
      (long long)cap64, cap64 >= 224 ? "ENOUGH" : "NEED-REALLOC");

    // ---- 零风险写测试：把 size 顶到 capacity（不越界），读回，还原 ----
    int32_t* sizePtr = (int32_t*)(base + game::layout().offBoneSize);
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
    int32_t* oldData = (int32_t*)*(void**)(base + game::layout().offBoneData);
    int64_t  oldCap  = *(int64_t*)(base + game::layout().offBoneCap);
    int32_t  oldSize = orig;

    if (oldData && oldSize > 0 && oldSize < TARGET) {
        int32_t* newData = (int32_t*)malloc((size_t)TARGET * 4);
        if (newData) {
            memset(newData, 0, (size_t)TARGET * 4);
            for (int32_t i = 0; i < oldSize; i++) newData[i] = oldData[i];
            // 多出来的槽位先填第一个骨骼的 instanceID（合法非零，不会崩）
            for (int32_t i = oldSize; i < TARGET; i++) newData[i] = oldData[0];

            bonehook::set_ours(newData, oldData, oldSize, oldCap);

            *(void**)(base + game::layout().offBoneData) = newData;
            *(int32_t*)(base + game::layout().offBoneSize) = TARGET;
            *(int64_t*)(base + game::layout().offBoneCap) = TARGET;   // 正常偶数，析构交给 hook 还原

            Il2CppObject* eb = invoke(g_mGetBones, s.obj, nullptr);
            int32_t eN = eb ? (int32_t)((Il2CppArray*)eb)->max_length : -1;
            L("[BONE] PERSISTENT expand: %d -> %d  data=%p (orig %p)  get_bones()=%d  %s",
              oldSize, TARGET, (void*)newData, oldData, eN,
              eN == TARGET ? "EXPAND-OK" : "EXPAND-FAIL");

            if (eN != TARGET) {
                // 读回不对就立刻回滚，别留半吊子状态
                *(void**)(base + game::layout().offBoneData) = oldData;
                *(int32_t*)(base + game::layout().offBoneSize) = oldSize;
                *(int64_t*)(base + game::layout().offBoneCap) = oldCap;
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
    ST_IDLE,       // 待命：等命令
    ST_LOAD,       // 正在加载模型
    ST_ACTIVE,     // 模型已生效
    ST_DORMANT     // 睡死：等 UI 发「初始化」，进去什么都不碰
};

// 初始化完成后要不要接着加载（玩家点了「加载模型」但还没初始化时的意图）
static volatile int g_pendingLoad = 0;

static Stage     g_stage = ST_DORMANT;   // 默认不自动做事，等 UI 发「初始化」
static long long g_t0 = 0;
static int       g_ticks = 0;
static char      g_status[256] = "init";
static bool      g_traced = false;   // 有人在 ptrace 我们（调试器/反作弊机器人）

// ---- 线程调度 ----
// tick_body 只能在 UnityMain 线程跑：Unity 的 native 对象创建
// （Mesh..ctor 等）在 Java 主线程会直接静默退出。
static long long     g_lastRenderTick = 0;   // 上次 render_tick 时间
static volatile int  g_inTick = 0;            // 防重入

// ---- UI 下发的配置（mmd_get_api 的 setter 写进来，tick 消费）----
struct UiCmd {
    int   enableReplace = 0;
    float modelScale    = 0.08f;   // 必须与 loader 的 kModelScale 一致，否则首帧就把它覆盖成 1.0（模型放大 12.5 倍）
    char  activeModel[160] = {0};
    int   cmdRescan  = 0;
    int   cmdReapply = 0;
    int   cmdUnload  = 0;
    int   cmdInit    = 0;   // UI 的「初始化」按钮：绑 il2cpp + 找类 + 扫 SMR
};

static pthread_mutex_t g_cfgMtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_statMtx = PTHREAD_MUTEX_INITIALIZER;
static UiCmd     g_ui;
static bool      g_hasUi = false;
static MmdStatus g_stat;      // 最近一帧的状态快照（给 mmd_get_api 读）
static int       g_statVer = 0;
static game::LoadResult  g_load;       // 最近一次加载结果
static mmd::Model        g_model;      // 解析出来的模型（常驻，别重复解析）
static mmd::Model& model_ref() { return g_model; }
static game::LoadResult& load_ref() { return g_load; }
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

// ---- 挑目标 SMR：只认身体，找不到就等 ----
//
// 之前找不到 11055_Body 时退而求其次拿「第一根骨骼>=40 的」，
// 结果在登录界面把 Face / Ship_xxx 当成了目标（build-57 实测
// 选中 #3 '15043_Face'）。拿 UI 模型的骨架套 MMD，既不对又白费。
//
// 现在只认名字里带 Body 且骨骼够多的，找不到就返回 -1，
// 让上层回到 IDLE 继续等（进入主城后才会扫到真正的角色）。
// 取离主相机最近的 Body。
//
// 历史：以前是「骨骼最多的 Body」——主城里 NPC/换装件多、战斗里对面也可能
// 骨骼更多，结果模型被挂到了对面角色身上（用户实测）。
// 玩家角色永远离相机最近，改按距离选（ModelMod v92 也是这么做的）。
static int pick_target_smr() {
    // The census already identified the player character; attach the model to
    // that exact SMR instead of re-guessing by distance (build-147 attached
    // the model to the opponent because this function had its own logic).
    if (g_anchorIdx >= 0 && g_anchorIdx < (int)g_smrs.size() &&
        g_smrs[(size_t)g_anchorIdx].obj) {
        L("[PICK] use anchored player #%d '%s' bones=%d", g_anchorIdx,
          g_smrs[(size_t)g_anchorIdx].name.c_str(), g_smrs[(size_t)g_anchorIdx].boneCount);
        return g_anchorIdx;
    }

    // 相机位置
    float cam[3] = {0, 0, 0};
    bool haveCam = false;
    {
        Il2CppClass* camCls = find_class_any("UnityEngine", "Camera");
        Il2CppClass* trCls  = find_class_any("UnityEngine", "Transform");
        const MethodInfo* mMain = camCls ? find_method_any(camCls, "get_main") : nullptr;
        const MethodInfo* mTr   = camCls ? find_method_any(camCls, "get_transform") : nullptr;
        const MethodInfo* mPos  = trCls  ? find_method_any(trCls, "get_position") : nullptr;
        void* c = mMain ? invoke(mMain, nullptr, nullptr) : nullptr;
        void* t = (c && mTr) ? invoke(mTr, c, nullptr) : nullptr;
        void* p = (t && mPos) ? invoke(mPos, t, nullptr) : nullptr;
        if (p) {
            float* f = (float*)((char*)p + 16);
            cam[0] = f[0]; cam[1] = f[1]; cam[2] = f[2];
            haveCam = true;
        }
    }

    Il2CppClass* trCls = find_class_any("UnityEngine", "Transform");
    const MethodInfo* mGetTr  = find_method_any(g_smrClass, "get_transform");
    const MethodInfo* mPos    = trCls ? find_method_any(trCls, "get_position") : nullptr;

    // 过滤掉「父级没激活」的候选：上一版选中了对象池里的备用角色，
    // 结果 goActive=0 / parActive=0，模型挂在了一个关掉的角色下面看不见。
    Il2CppClass* compCls = find_class_any("UnityEngine", "Component");
    Il2CppClass* goCls   = find_class_any("UnityEngine", "GameObject");
    const MethodInfo* mCompGetGO = compCls ? find_method_any(compCls, "get_gameObject") : nullptr;
    const MethodInfo* mActive    = goCls ? find_method_any(goCls, "get_activeInHierarchy") : nullptr;

    int   best = -1;
    float bestD2 = 0.0f;
    for (size_t i = 0; i < g_smrs.size(); i++) {
        const SmrInfo& s = g_smrs[i];
        if (s.name.find("Body") == std::string::npos) continue;
        if (s.boneCount < 40) continue;

        // 必须在激活的层级里，否则就是池子里的死对象
        if (mCompGetGO && mActive) {
            void* go = invoke(mCompGetGO, s.obj, nullptr);
            void* r  = go ? invoke(mActive, go, nullptr) : nullptr;
            const bool act = r ? (*(uint8_t*)((char*)r + 16) != 0) : true;
            if (!act) {
                L("[PICK] #%zu '%s' 父级未激活，跳过", i, s.name.c_str());
                continue;
            }
        }

        float d2 = 0.0f;
        bool have = false;
        if (haveCam && mGetTr && mPos) {
            void* tr = invoke(mGetTr, s.obj, nullptr);
            void* p  = tr ? invoke(mPos, tr, nullptr) : nullptr;
            if (p) {
                float* f = (float*)((char*)p + 16);
                const float dx = f[0] - cam[0];
                const float dy = f[1] - cam[1];
                const float dz = f[2] - cam[2];
                d2 = dx * dx + dy * dy + dz * dz;
                have = true;
            }
        }
        L("[PICK] #%zu '%s' bones=%d dist=%.2f%s", i, s.name.c_str(), s.boneCount,
          have ? sqrtf(d2) : -1.0f, have ? "" : "(无位置)");

        if (!have) {   // 拿不到位置就退回骨骼最多
            if (best < 0 || s.boneCount > g_smrs[(size_t)best].boneCount) best = (int)i;
            continue;
        }
        if (best < 0 || !haveCam || d2 < bestD2) { best = (int)i; bestD2 = d2; }
    }

    if (best >= 0) {
        L("[PICK] 选中 #%d '%s' bones=%d（主相机附近）", best,
          g_smrs[(size_t)best].name.c_str(), g_smrs[(size_t)best].boneCount);
    }
    return best;
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
    smr.meshObj   = g_smrs[(size_t)idx].meshObj;

    // 目标角色的 Transform：模型根节点要挂在它下面，才能跟着角色走
    void* roleRoot = nullptr;
    {
        const MethodInfo* mGetTr = find_method_any(g_smrClass, "get_transform");
        if (mGetTr) roleRoot = invoke(mGetTr, g_smrs[(size_t)idx].obj, nullptr);
        L("[LOAD] roleRoot=%p (SMR #%d '%s')", roleRoot, idx,
          g_smrs[(size_t)idx].name.c_str());
    }

    g_load = game::LoadResult{};
    if (!game::load_model_into_smr(g_model, pmx.c_str(), smr, roleRoot, g_load)) {
        LERR("[LOAD] 失败: %s", g_load.error);
        return false;
    }

    g_lastSmrIndex = idx;
    build_retarget();   // mapping is ready only after both anchor + model exist
    game::skin::init(g_model, g_load.mesh);   // CPU skinning arrays + pinned vertex array
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
        case ST_DORMANT: s.gameState = MMD_GS_UNKNOWN;  break;
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
    s.modelScale   = hasUi ? ui.modelScale : 0.08f;
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

// 真正的每帧逻辑。只能在 UnityMain 线程调。
// =====================================================================
// 信号处理器：不要碰 SIG_DFL。
//
// build-63 实测：把 SIGSEGV/SIGABRT 设成 SIG_DFL，Android 的 libsigchain
// 每次都打 `E/libsigchain: Setting SIGSEGV to SIG_DFL` + 一整条调用栈
// （#01 sigaction → #02 tick_body），30 次刷屏，而且它并不会真的让
// crash_dump64 接管—— ART 自己的 fault handler 被搅乱，tombstone 依然没有。
//
// 结论：这条路不通。想拿栈只能注册自己的 handler（挂在 sigchain 链首，
// 记完寄存器再返回让链继续），或者从 NPTH 自己的 dump 里找。
// =====================================================================

static void tick_body() {
    if (g_inTick) {
        // 上次进来没出去 = 卡住了。别无限沉默，报一下。
        static int stuck = 0;
        if (++stuck % 600 == 1) L("[PERF] tick 重入 %d 次，上一次可能卡死了", stuck);
        return;
    }
    g_inTick = 1;

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    if (g_t0 == 0) {
        g_t0 = now_ms();
        L("[PERF] tick thread tid=%d", (int)syscall(SYS_gettid));
        g_inTick = 0;
        return;
    }
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

    case ST_DORMANT: {
        // 默认状态：不对游戏做任何事，等玩家在 UI 里点「初始化」。
        // 为什么：自动加载会赶在玩家还没进场景时摸 Unity 对象，
        // build-121 实测会把游戏画面搞卡，所以改成手动两步。
        bool init = false, apply = false;
        pthread_mutex_lock(&g_cfgMtx);
        init  = g_ui.cmdInit    != 0;
        apply = g_ui.cmdReapply != 0;
        if (init)  g_ui.cmdInit    = 0;
        if (apply) { g_ui.cmdReapply = 0; g_pendingLoad = 1; }
        if (init && !apply) g_pendingLoad = 0;   // 只初始化，不跟加载
        pthread_mutex_unlock(&g_cfgMtx);

        if (init || apply) {
            snprintf(g_status, sizeof(g_status), "init requested");
            L("[DORMANT] 收到指令 init=%d load=%d -> 开始初始化", (int)init, (int)apply);
            g_stage = ST_BIND;
        }
        break;
    }

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
            if (g_pendingLoad) {
                g_pendingLoad = 0;
                snprintf(g_status, sizeof(g_status), "init done -> loading");
                L("[SCAN] 初始化完成，接着加载模型");
                g_stage = ST_LOAD;
            } else {
                snprintf(g_status, sizeof(g_status), "ready (%zu SMR)", g_smrs.size());
                g_stage = ST_IDLE;
            }
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
        bool reinit  = g_ui.cmdInit    != 0;
        g_ui.cmdRescan  = 0;
        g_ui.cmdReapply = 0;
        g_ui.cmdInit    = 0;
        pthread_mutex_unlock(&g_cfgMtx);

        // 注：以前这里有两个自动加载（15 秒无 UI 兜底、打开总开关就加载），
        // build-121 实测会把游戏画面搞卡，现在全部拆掉——只有玩家点按钮才动。

        if (rescan || reinit) {
            g_smrs.clear();
            snprintf(g_status, sizeof(g_status), "rescan requested");
            g_stage = ST_SCAN;
            break;
        }

        if (reapply) {
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
    // 把锤定的角色给摄像机（复位时拉回它身边）
    game::camera_set_follow(g_anchorTr);
    // 消费 UI 侧的开关请求：AddComponent / Destroy 要建/析 native 对象，
    // 只能在 UnityMain 跑（Java 主线程跑会静默失败）
    game::camera_pump();
    if (game::g_camera.enabled) game::g_camera.update();

    // Re-census trigger: create <gamedir>/mmd_recensus to re-run the census +
    // anchor at any time (e.g. after entering a battle scene).
    if (access("/data/data/com.hermes.h1game/files/mmd_recensus", F_OK) == 0) {
        unlink("/data/data/com.hermes.h1game/files/mmd_recensus");
        L("[CENSUS] manual trigger (mmd_recensus found) -- force re-anchor");
        census_and_anchor(true);   // true = ignore existing anchor
        build_retarget();
    }

    // Recompute bind offsets only (keep the anchor). Point here when the
    // character is standing still, so the pose captured at startup isn't
    // baked in as "bind" while it was mid-run.
    if (access("/data/data/com.hermes.h1game/files/mmd_recalib", F_OK) == 0) {
        unlink("/data/data/com.hermes.h1game/files/mmd_recalib");
        L("[RT] manual recalib (mmd_recalib found)");
        build_retarget();
    }

    // Drive our own MMD skeleton from the anchored game bones.
    retarget_update();

    // 跨帧观测自建 SMR 的 isVisible / bounds（每 60 帧一次）
    game::probe_self_tick();

    // layer 守卫（ModelMod v88 的 ensure_model_visible）：
    // 每 200ms 检查一次模型层还在不在、相机 cullingMask 还开着没。
    // 切场景时引擎会改 cullingMask，补一次不够。
    game::layer_guard::tick();

    // 崩溃探测器保活：
    //   sigaltstack 是线程级的，tick 跑的 UnityMain 要单独装；
    //   处理器可能被 libnpth_wrapper 之类的顶掉，每 3 秒查一次、掉了就重装。
    {
        static bool altDone = false;
        if (!altDone) { altDone = true; crash_report::prepare_thread(); }
        static long long lastCrashChk = 0;
        if (now_ms() - lastCrashChk > 3000) {
            lastCrashChk = now_ms();
            crash_report::check();
        }
    }

    {
        struct timespec ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        const long long ns = (long long)(ts1.tv_sec - ts0.tv_sec) * 1000000000LL
                           + (ts1.tv_nsec - ts0.tv_nsec);
        // 超过 8ms 就报：60fps 是 16.6ms/帧，我们的逻辑吃掉一半就不正常了
        if (ns > 8LL * 1000 * 1000) L("[PERF] tick 耗时 %.1fms", (double)ns / 1e6);
    }

    refresh_status();   // 状态快照每帧刷一次，UI 侧随时能读
    g_inTick = 0;
}

// UI so 在 eglSwapBuffers hook 里每帧调（UnityMain 线程）
static void api_render_tick() {
    g_lastRenderTick = now_ms();
    tick_body();
}

extern "C" void native_tick() {
    // Java 主线程。
    //
    // **不能在这里跑 tick_body**：里面全是 Unity 调用，在非 UnityMain 线程
    // 碰 Unity 会和真正的 UnityMain 抢内部锁，表现是整进程卡死——
    // build-164 实测：hook 没装上、走这个兜底，日志 17 分钟不写一行，
    // UnityMain CPU 45% 在那儿转圈，用户看到的就是「UI 非常卡」。
    //
    // 这里只做纯数据的事（刷新状态快照），不碰任何 Unity 对象。
    refresh_status();
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

// 摄像机触摸。
//
// action 可能被 Java 侧编过 pointer index（多指时 base | (i << 8)），
// 这里先拆开再交给 camera_touch —— 它按 pointerId 维护每个手指的状态。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeCameraTouch(JNIEnv*, jclass,
                                                     jint action, jint pointerId,
                                                     jfloat x, jfloat y,
                                                     jfloat vw, jfloat vh) {
    if (!game::g_camera.enabled) return JNI_FALSE;

    int base = action & 0xFF;

    // 多指的第二根及以后，MotionEvent 给的是 ACTION_POINTER_DOWN(5) /
    // ACTION_POINTER_UP(6)，不是 DOWN(0) / UP(1)。
    // 不归一化的话：先按左半屏摇杆、再按右半屏想转视角 →
    // 第二根手指直接进 default 分支还给游戏，双指永远玩不了。
    if (base == 5) base = 0;        // ACTION_POINTER_DOWN -> DOWN
    else if (base == 6) base = 1;   // ACTION_POINTER_UP   -> UP

    return game::camera_touch(base, (int)pointerId, x, y, vw, vh) ? JNI_TRUE : JNI_FALSE;
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
        out->modelScale  = 0.08f;
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
    if (!(s > 0.0f)) s = 0.08f;
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.modelScale = s;
    pthread_mutex_unlock(&g_cfgMtx);
}

// 「初始化」：只绑 il2cpp + 找 Unity 类 + 扫 SMR，不建模型。
// 玩家手动点的第一步（为什么不一进游戏就自动跑：见 ST_DORMANT 的注释）。
static void api_cmd_init() {
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdInit = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    L("[API] cmd_init");
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
    pthread_mutex_lock(&g_cfgMtx);
    g_ui.cmdReapply = 1;
    g_hasUi = true;
    pthread_mutex_unlock(&g_cfgMtx);
    g_forceReparse = 1;   // 主线程的 do_load_model 会看清这个再重新读盘
    L("[API] cmd_reparse");
}

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

static void api_camera_motion(int ascend, int descend, int boost) {
    game::camera_set_motion(ascend, descend, boost);
    L("[API] camera_motion(up=%d down=%d boost=%d)", ascend, descend, boost);
}

static void api_camera_config(float speed, float sens, float radius) {
    game::camera_set_config(speed, sens, radius);
    L("[API] camera_config(speed=%.2f sens=%.3f radius=%.0f)", speed, sens, radius);
}

static int api_camera_stick(float* out) {
    return game::camera_stick(out);
}

static void api_camera_get_config(float* out) {
    game::camera_get_config(out);
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
        api_render_tick,
        api_cmd_init,
        api_camera_motion,
        api_camera_config,
        api_camera_stick,
        api_camera_get_config,
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

    // 自己的崩溃现场记录：游戏的 libnpth_wrapper 抢了信号链，
    // tombstone 写不出来，只能自己接 SIGSEGV。
    pick_dir();
    {
        std::string lp = g_dir + "/mmdruntime.log";
        crash_report::install(lp.c_str());
    }

    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);

    // UI 走 App 自己的悬浮窗（libimgui_overlay.so），游戏侧一律不碰 GL
    return JNI_VERSION_1_6;
}
