// =====================================================================
// unity_objects.cpp
// =====================================================================
#include "unity_objects.h"

#include <android/log.h>
#include <cstring>
#include <string>

#include "il2cpp_api.h"

#define UTAG "MmdUnityObj"
#define ULOG(...) __android_log_print(ANDROID_LOG_INFO, UTAG, __VA_ARGS__)

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

Il2CppObject* invoke(const MethodInfo* m, void* obj, void** args) {
    if (!m) return nullptr;
    Il2CppException* exc = nullptr;
    Il2CppObject* r = g_api.runtime_invoke(m, obj, args, &exc);
    if (exc) {
        void* cl = *(void**)exc;
        const char* n = cl && g_api.class_get_name ? g_api.class_get_name((Il2CppClass*)cl) : "?";
        ULOG("[UNITYOBJ] invoke threw %s", n ? n : "?");
        return nullptr;
    }
    return r;
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

    g_mGoCtorString  = find_method(g_goClass, ".ctor", 1);
    g_mGetTransform  = find_method(g_goClass, "get_transform", 0);
    g_mSetParent     = find_method(g_trClass, "set_parent", 1);
    g_mSetLocalPos   = find_method(g_trClass, "set_localPosition", 1);
    g_mSetLocalRot   = find_method(g_trClass, "set_localRotation", 1);
    g_mSetLocalScale = find_method(g_trClass, "set_localScale", 1);
    if (g_objClass) {
        g_mGetInstanceID = find_method(g_objClass, "GetInstanceID", 0);
        g_mGetName       = find_method(g_objClass, "get_name", 0);
    }

    ULOG("[UNITYOBJ] ctor=%p getTransform=%p setParent=%p setLocalPos=%p setLocalRot=%p setLocalScale=%p",
         (void*)g_mGoCtorString, (void*)g_mGetTransform, (void*)g_mSetParent,
         (void*)g_mSetLocalPos, (void*)g_mSetLocalRot, (void*)g_mSetLocalScale);

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
    Il2CppObject* go = g_api.object_new ? g_api.object_new(g_goClass) : nullptr;
    if (!go) { ULOG("[UNITYOBJ] object_new failed"); return nullptr; }

    Il2CppString* s = g_api.string_new ? g_api.string_new(name ? name : "mmd") : nullptr;
    void* args[1] = { s };
    invoke(g_mGoCtorString, go, args);

    // 2) 取 Transform
    Il2CppObject* tr = invoke(g_mGetTransform, go, nullptr);
    if (!tr) { ULOG("[UNITYOBJ] get_transform failed"); return nullptr; }

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
    return invoke(g_mSetParent, transform, a) != nullptr;
}

bool set_local_position(void* transform, float x, float y, float z) {
    if (!g_ready || !transform) return false;
    struct { float x, y, z; } v = { x, y, z };
    void* a[1] = { &v };
    return invoke(g_mSetLocalPos, transform, a) != nullptr;
}

bool set_local_rotation(void* transform, float x, float y, float z, float w) {
    if (!g_ready || !transform || !g_mSetLocalRot) return false;
    struct { float x, y, z, w; } q = { x, y, z, w };
    void* a[1] = { &q };
    return invoke(g_mSetLocalRot, transform, a) != nullptr;
}

bool set_local_scale(void* transform, float x, float y, float z) {
    if (!g_ready || !transform || !g_mSetLocalScale) return false;
    struct { float x, y, z; } v = { x, y, z };
    void* a[1] = { &v };
    return invoke(g_mSetLocalScale, transform, a) != nullptr;
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

}  // namespace uo
