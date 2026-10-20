// =====================================================================
// unity_objects.cpp
// =====================================================================
#include "logsink.h"
#include "unity_objects.h"

#include <android/log.h>
#include <cstring>
#include <string>

#include "il2cpp_api.h"

#define UTAG "MmdUnityObj"
#define ULOG(...) logsink::Write(__VA_ARGS__)

// 来自 module.cpp
extern Il2CppApi g_api;

namespace uo {

namespace {

Il2CppClass* g_goClass      = nullptr;   // UnityEngine.GameObject
Il2CppClass* g_trClass      = nullptr;   // UnityEngine.Transform
Il2CppClass* g_objClass     = nullptr;   // UnityEngine.Object

const MethodInfo* g_mGoCtorString  = nullptr;  // GameObject::.ctor(string)
const MethodInfo* g_mGetTransform  = nullptr;  // GameObject::get_transform
const MethodInfo* g_mSetParent     = nullptr;  // Transform::set_parent(Transform)
const MethodInfo* g_mSetLocalPos   = nullptr;  // Transform::set_localPosition(Vector3)
const MethodInfo* g_mSetLocalRot   = nullptr;  // Transform::set_localRotation(Quaternion)
const MethodInfo* g_mSetLocalScale = nullptr;  // Transform::set_localScale(Vector3)

const MethodInfo* g_mGetInstanceID = nullptr;  // Object::GetInstanceID
const MethodInfo* g_mGetName       = nullptr;  // Object::get_name

int g_created = 0;
std::string g_lastException;   // 最近一次 il2cpp 异常的类名，诊断用
bool g_ready = false;

// 静态缓冲，给名字用（别长期持有）
char g_nameBuf[128];

Il2CppClass* find_class_any(const char* ns, const char* name) {
    if (!g_api.domain_get || !g_api.class_from_name) return nullptr;
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

// 只在 klass 本类里找，不沿父链。
//
// 为什么需要：find_method 会往上找父类。找 .ctor 时会出大问题 ——
// 子类自己没对应构造函数（被 strip 了）时，会撞到父类的 .ctor，
// 拿父类构造函数初始化子类对象是错的（native 侧初始化路径不一样）。
const MethodInfo* find_method_own(Il2CppClass* klass, const char* name,
                                  int paramCount, const char* p0Class) {
    if (!klass) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m;
    while ((m = g_api.class_get_methods(klass, &iter))) {
        const char* nm = g_api.method_get_name(m);
        if (!nm || strcmp(nm, name) != 0) continue;
        if (paramCount >= 0 && (int)g_api.method_get_param_count(m) != paramCount) continue;
        if (p0Class && g_api.method_get_param && g_api.class_from_type && g_api.class_get_name) {
            const Il2CppType* t = g_api.method_get_param(m, 0);
            if (!t) continue;
            Il2CppClass* pc = g_api.class_from_type(t);
            const char* cn = pc ? g_api.class_get_name(pc) : nullptr;
            if (!cn || strcmp(cn, p0Class) != 0) continue;
        }
        return m;
    }
    return nullptr;
}

// 按名字找方法；指定 paramCount 时按参数个数筛（区分重载）
const MethodInfo* find_method(Il2CppClass* klass, const char* name, int paramCount = -1) {
    if (!klass) return nullptr;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent ? g_api.class_get_parent(k) : nullptr) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (!nm || strcmp(nm, name) != 0) continue;
            if (paramCount >= 0 && g_api.method_get_param_count) {
                if ((int)g_api.method_get_param_count(m) != paramCount) continue;
            }
            return m;
        }
    }
    return nullptr;
}

// 按「名字 + 参数个数 + 第一个参数的类名」找方法。
//
// 为什么必须这样：Unity 的 .ctor / SetTexture / SetColor 有大量重载，
// 只按参数个数筛会撞错。实测撞错的后果是 runtime_invoke 不抛异常、
// 只是安安静静返回 nullptr，整条加载链在「看起来成功」中死掉。
const MethodInfo* find_method_typed(Il2CppClass* klass, const char* name,
                                    int paramCount, const char* p0Class) {
    if (!klass || !name) return nullptr;
    for (Il2CppClass* k = klass; k; k = g_api.class_get_parent ? g_api.class_get_parent(k) : nullptr) {
        void* iter = nullptr;
        const MethodInfo* m;
        while ((m = g_api.class_get_methods(k, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (!nm || strcmp(nm, name) != 0) continue;
            if (paramCount >= 0 && (int)g_api.method_get_param_count(m) != paramCount) continue;
            if (p0Class && g_api.method_get_param && g_api.class_from_type && g_api.class_get_name) {
                const Il2CppType* t = g_api.method_get_param(m, 0);
                if (!t) continue;
                Il2CppClass* pc = g_api.class_from_type(t);
                const char* cn = pc ? g_api.class_get_name(pc) : nullptr;
                if (!cn || strcmp(cn, p0Class) != 0) continue;
            }
            return m;
        }
    }
    return nullptr;
}

Il2CppObject* invoke(const MethodInfo* m, void* obj, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, obj, args, &exc);
    if (exc) {
        // 异常不清掉的话 il2cpp 的异常状态会一直挂着，
        // 下一次 runtime_invoke 直接走异常路径，表现就是整个加载链静默死掉。
        void* cl = *(void**)exc;
        const char* n = cl && g_api.class_get_name ? g_api.class_get_name((Il2CppClass*)cl) : "?";
        ULOG("[UNITYOBJ] invoke threw %s (method=%p obj=%p)",
             n ? n : "?", (void*)m, obj);
        g_lastException = n ? n : "?";
        return nullptr;
    }
    return r;
}

// 调可能返回 void 的方法，只看有没有抛异常。
//
// 坑（build-49 亲手踩的）：void 方法 runtime_invoke 照样返回 nullptr。
// 用「返回值 != null」判断成功会把所有成功调用误判成失败 ——
// 当时表现是 224 根骨骼一根建不出来，而实际上 ctor 一直是好的。
bool invoke_void(const MethodInfo* m, void* obj, void** args) {
    if (!m) return false;
    Il2CppException* exc = nullptr;
    g_api.runtime_invoke(m, obj, args, &exc);
    if (exc) {
        void* cl = *(void**)exc;
        const char* n = cl && g_api.class_get_name ? g_api.class_get_name((Il2CppClass*)cl) : "?";
        ULOG("[UNITYOBJ] invoke threw %s (method=%p obj=%p)",
             n ? n : "?", (void*)m, obj);
        g_lastException = n ? n : "?";
        return false;
    }
    return true;
}

int32_t invoke_int(const MethodInfo* m, void* obj, void** args, int32_t dflt) {
    Il2CppObject* r = invoke(m, obj, args);
    if (!r) return dflt;
    return *(int32_t*)((char*)r + 16);   // boxed int32 的 value 在 +0x10
}

}  // namespace

bool init() {
    if (g_ready) return true;

    g_goClass  = find_class_any("UnityEngine", "GameObject");
    g_trClass  = find_class_any("UnityEngine", "Transform");
    g_objClass = find_class_any("UnityEngine", "Object");
    if (!g_goClass || !g_trClass) {
        ULOG("[UNITYOBJ] class missing go=%p tr=%p", (void*)g_goClass, (void*)g_trClass);
        return false;
    }

    // .ctor(string)：GameObject 有多个单参 ctor，必须按参数类型筛，
    // 否则撞上 .ctor(string, Type[]) 那个，传进去静默失败返回 nullptr。
    g_mGoCtorString  = find_method_typed(g_goClass, ".ctor", 1, "String");
    if (!g_mGoCtorString) g_mGoCtorString = find_method(g_goClass, ".ctor", 1);

    g_mGetTransform  = find_method(g_goClass, "get_transform", 0);
    g_mSetParent     = find_method_typed(g_trClass, "set_parent", 1, "Transform");
    if (!g_mSetParent) g_mSetParent = find_method(g_trClass, "set_parent", 1);

    g_mSetLocalPos   = find_method_typed(g_trClass, "set_localPosition", 1, "Vector3");
    if (!g_mSetLocalPos) g_mSetLocalPos = find_method(g_trClass, "set_localPosition", 1);
    g_mSetLocalRot   = find_method(g_trClass, "set_localRotation", 1);
    g_mSetLocalScale = find_method(g_trClass, "set_localScale", 1);
    if (g_objClass) {
        g_mGetInstanceID = find_method(g_objClass, "GetInstanceID", 0);
        g_mGetName       = find_method(g_objClass, "get_name", 0);
    }

    ULOG("[UNITYOBJ] ctor=%p getTransform=%p setParent=%p setLocalPos=%p setLocalRot=%p setLocalScale=%p",
         (void*)g_mGoCtorString, (void*)g_mGetTransform, (void*)g_mSetParent,
         (void*)g_mSetLocalPos, (void*)g_mSetLocalRot, (void*)g_mSetLocalScale);
    // 把 GameObject 所有单参 .ctor 列出来，确认到底有几个、选中的是哪个
    {
        void* iter = nullptr;
        const MethodInfo* m;
        int idx = 0;
        while ((m = g_api.class_get_methods(g_goClass, &iter))) {
            const char* nm = g_api.method_get_name(m);
            if (!nm || strcmp(nm, ".ctor") != 0) continue;
            uint32_t pc = g_api.method_get_param_count(m);
            char buf[64] = {0};
            if (pc > 0 && g_api.method_get_param && g_api.class_from_type && g_api.class_get_name) {
                const Il2CppType* t = g_api.method_get_param(m, 0);
                Il2CppClass* pcl = t ? g_api.class_from_type(t) : nullptr;
                const char* cn = pcl ? g_api.class_get_name(pcl) : nullptr;
                if (cn) snprintf(buf, sizeof(buf), "%s", cn);
            }
            ULOG("[UNITYOBJ]   ctor#%d params=%u p0=%s %s", idx++, pc, buf[0] ? buf : "-",
                 (m == g_mGoCtorString) ? "<== SELECTED" : "");
        }
    }

    if (!g_mGoCtorString || !g_mGetTransform || !g_mSetParent || !g_mSetLocalPos) {
        ULOG("[UNITYOBJ] essential method missing");
        return false;
    }

    g_ready = true;
    return true;
}

bool ready() { return g_ready; }

void* create_node(const char* name, void* parentTransform, float lx, float ly, float lz) {
    if (!g_ready) return nullptr;

    // 1) new GameObject
    //
    // il2cpp_object_new 只分配内存、填 klass/monitor，**不跑构造函数**。
    // GameObject 的 .ctor 里要做 native 侧注册（拿 m_CachedPtr），
    // 不跑的话这是个半成品：下一帧 Unity 遍历场景对象时踩到它就炸。
    //
    // 2026-10-20 实测：224 个半成品 GameObject 让游戏在加载到 50% 时
    // 整个进程消失，且不留 tombstone（走的是 Unity 自己的异常终止路径）。
    Il2CppObject* go = g_api.object_new ? g_api.object_new(g_goClass) : nullptr;
    if (!go) { ULOG("[UNITYOBJ] object_new failed"); return nullptr; }

    Il2CppString* s = g_api.string_new ? g_api.string_new(name ? name : "mmd") : nullptr;
    void* args[1] = { &s };
    // 构造函数返回 void —— 只能看异常，不能看返回值
    if (!invoke_void(g_mGoCtorString, go, args)) {
        static int ctorFails = 0;
        if (ctorFails++ < 10) {
            ULOG("[UNITYOBJ] GameObject..ctor 失败 (%s): %s",
                 name ? name : "?", g_lastException.c_str());
        }
        return nullptr;
    }

    // 2) 取 Transform
    Il2CppObject* tr = invoke(g_mGetTransform, go, nullptr);
    if (!tr) { ULOG("[UNITYOBJ] get_transform failed (go=%p)", go); return nullptr; }

    // 3) 挂父级
    if (parentTransform) {
        void* p[1] = { parentTransform };
        invoke(g_mSetParent, tr, p);
    }

    // 4) localPosition
    struct { float x, y, z; } v = { lx, ly, lz };
    void* a[1] = { &v };
    invoke(g_mSetLocalPos, tr, a);

    g_created++;
    return tr;
}

bool set_parent(void* transform, void* parentTransform) {
    if (!g_ready || !transform || !g_mSetParent) return false;
    void* a[1] = { parentTransform };
    return invoke_void(g_mSetParent, transform, a);
}

bool set_local_position(void* transform, float x, float y, float z) {
    if (!g_ready || !transform) return false;
    struct { float x, y, z; } v = { x, y, z };
    void* a[1] = { &v };
    return invoke_void(g_mSetLocalPos, transform, a);
}

bool set_local_rotation(void* transform, float x, float y, float z, float w) {
    if (!g_ready || !transform || !g_mSetLocalRot) return false;
    struct { float x, y, z, w; } q = { x, y, z, w };
    void* a[1] = { &q };
    return invoke_void(g_mSetLocalRot, transform, a);
}

bool set_local_scale(void* transform, float x, float y, float z) {
    if (!g_ready || !transform || !g_mSetLocalScale) return false;
    struct { float x, y, z; } v = { x, y, z };
    void* a[1] = { &v };
    return invoke_void(g_mSetLocalScale, transform, a);
}

int32_t instance_id(void* obj) {
    if (!obj || !g_mGetInstanceID) return 0;
    return invoke_int(g_mGetInstanceID, obj, nullptr, 0);
}

const char* object_name(void* obj) {
    if (!obj || !g_mGetName) return "";
    Il2CppObject* r = invoke(g_mGetName, obj, nullptr);
    if (!r) return "";
    // Il2CppString: klass(8) monitor(8) length(4) chars[utf16]
    char* p = (char*)r;
    int32_t len = *(int32_t*)(p + 16);
    const uint16_t* ch = (const uint16_t*)(p + 20);
    int n = 0;
    for (int32_t i = 0; i < len && n < (int)sizeof(g_nameBuf) - 1; i++) {
        uint16_t c = ch[i];
        g_nameBuf[n++] = (c < 0x80) ? (char)c : '?';
    }
    g_nameBuf[n] = 0;
    return g_nameBuf;
}

// ---- 通用桥接 ----
void* u_class(const char* ns, const char* name) { return find_class_any(ns, name); }
void* u_method(void* klass, const char* name, int paramCount) {
    return (void*)find_method((Il2CppClass*)klass, name, paramCount);
}
void* u_call(void* obj, void* method, void** args) {
    return invoke((const MethodInfo*)method, obj, args);
}
void* u_new_array(void* elemClass, size_t count) {
    if (!g_api.array_new || !elemClass) return nullptr;
    return g_api.array_new((Il2CppClass*)elemClass, count);
}
void* u_array_data(void* arr) { return (char*)arr + 32; }
void* u_new_object(void* klass) {
    if (!g_api.object_new || !klass) return nullptr;
    return g_api.object_new((Il2CppClass*)klass);
}

int created_nodes() { return g_created; }


bool u_call_void(void* obj, void* method, void** args) {
    return invoke_void((const MethodInfo*)method, obj, args);
}

void* u_method_own(void* klass, const char* name, int paramCount) {
    return (void*)find_method_own((Il2CppClass*)klass, name, paramCount, nullptr);
}

void* u_method_typed(void* klass, const char* name, int paramCount, const char* p0Class) {
    return (void*)find_method_typed((Il2CppClass*)klass, name, paramCount, p0Class);
}

void* u_array_data_from(void* arr, size_t idx) {
    if (!arr) return nullptr;
    return (char*)arr + 32 + sizeof(void*) * idx;
}
}  // namespace uo
