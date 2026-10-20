// =====================================================================
// material.cpp
//
// 关键点：Material 的方法都有重载，光按「名字 + 参数个数」找会拿错。
//   SetTexture(string, Texture)  vs  SetTexture(int, Texture)
//   SetColor(string, Color)      vs  SetColor(int, Color)
// 所以这里自己遍历方法表，按「第一个参数是不是 System.String」筛。
// =====================================================================
#include "logsink.h"
#include "material.h"

#include <android/log.h>

#include <cstring>
#include <string>

#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define MATTAG "MmdMaterial"
#define MTLOG(...) logsink::Write(__VA_ARGS__)

namespace mmd {
namespace {

struct MatClasses {
    void* matCls     = nullptr;
    void* ctorShader = nullptr;   // Material(Shader)
    void* setTexture = nullptr;   // SetTexture(string, Texture)
    void* setColor   = nullptr;   // SetColor(string, Color)
    void* setFloat   = nullptr;   // SetFloat(string, float)
    void* setQueue   = nullptr;   // set_renderQueue
    void* shaderFind = nullptr;   // Shader.Find(string) 静态
};

const char* kShaderChain[] = {
    "Unlit/Texture",
    "Mobile/Diffuse",
    "Sprites/Default",
    "Standard",
    "Diffuse",
};

// 按名字 + 参数个数 + 第一个参数类名找方法
void* find_method_typed(void* klass, const char* name, int nparams, const char* p0Class) {
    if (!klass || !name) return nullptr;
    void* iter = nullptr;
    const MethodInfo* m = nullptr;
    while ((m = g_api.class_get_methods((Il2CppClass*)klass, &iter)) != nullptr) {
        const char* mn = g_api.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if ((int)g_api.method_get_param_count(m) != nparams) continue;

        if (p0Class == nullptr) return (void*)m;

        const Il2CppType* t = g_api.method_get_param(m, 0);
        if (!t) continue;
        Il2CppClass* pc = g_api.class_from_type(t);
        const char* cn = pc ? g_api.class_get_name(pc) : nullptr;
        if (cn && strcmp(cn, p0Class) == 0) return (void*)m;
    }
    return nullptr;
}

bool init_mat_classes(MatClasses& mc, void (*logf)(const char*, ...)) {
    mc.matCls = uo::u_class("UnityEngine", "Material");
    if (!mc.matCls) { if (logf) logf("[MAT] Material class missing"); return false; }

    mc.ctorShader = find_method_typed(mc.matCls, ".ctor", 1, "Shader");
    if (!mc.ctorShader) {
        // 类名可能带命名空间前缀，退一步按参数个数硬拿
        mc.ctorShader = uo::u_method(mc.matCls, ".ctor", 1);
    }
    mc.setTexture = find_method_typed(mc.matCls, "SetTexture", 2, "String");
    mc.setColor   = find_method_typed(mc.matCls, "SetColor",   2, "String");
    mc.setFloat   = find_method_typed(mc.matCls, "SetFloat",   2, "String");
    mc.setQueue   = uo::u_method(mc.matCls, "set_renderQueue", 1);

    void* shaderCls = uo::u_class("UnityEngine", "Shader");
    if (shaderCls) mc.shaderFind = uo::u_method(shaderCls, "Find", 1);

    if (logf) {
        logf("[MAT] ctor=%p setTex=%p setColor=%p setFloat=%p queue=%p find=%p",
             mc.ctorShader, mc.setTexture, mc.setColor, mc.setFloat,
             mc.setQueue, mc.shaderFind);
    }
    return mc.ctorShader != nullptr;
}

void* find_first_shader(MatClasses& mc, const char** used, void (*logf)(const char*, ...)) {
    if (!mc.shaderFind) return nullptr;
    for (const char* n : kShaderChain) {
        Il2CppString* s = g_api.string_new(n);
        if (!s) continue;
        void* args[1] = { s };
        void* sh = uo::u_call(nullptr, mc.shaderFind, args);
        if (logf) logf("[MAT] Shader.Find('%s') -> %p", n, sh);
        if (sh) { *used = n; return sh; }
    }
    return nullptr;
}

}  // namespace

bool build_materials(const Model& model,
                     const TextureSet& tex,
                     MaterialSet& out,
                     void (*logf)(const char*, ...)) {
    out = MaterialSet{};
    if (model.materials.empty()) {
        if (logf) logf("[MAT] model has no materials");
        return false;
    }

    MatClasses mc;
    if (!init_mat_classes(mc, logf)) return false;

    const char* used = "";
    void* shader = find_first_shader(mc, &used, logf);
    if (!shader) {
        if (logf) logf("[MAT] no shader available, all materials will be default");
        return false;
    }
    out.shaderName = used;
    if (logf) logf("[MAT] using shader '%s'", used);

    out.items.assign(model.materials.size(), nullptr);

    Il2CppString* texName = g_api.string_new("_MainTex");
    Il2CppString* colName = g_api.string_new("_Color");

    for (size_t i = 0; i < model.materials.size(); i++) {
        const Material& m = model.materials[i];

        void* mat = uo::u_new_object(mc.matCls);
        if (!mat) { if (logf) logf("[MAT] #%zu object_new failed", i); continue; }

        void* a1[1] = { &shader };
        uo::u_call(mat, mc.ctorShader, a1);

        // 贴图
        if (mc.setTexture && texName) {
            void* t = nullptr;
            if (m.texIndex >= 0 && (size_t)m.texIndex < tex.items.size()) {
                t = tex.items[(size_t)m.texIndex];
            }
            if (t) {
                // string / Texture 都是引用类型，runtime_invoke 要的是对象指针的地址
                void* a2[2] = { &texName, &t };
                uo::u_call(mat, mc.setTexture, a2);
            }
        }

        // 颜色（PMX 的 diffuse 是 RGBA，正好对上 Unity Color 的内存布局）
        if (mc.setColor && colName) {
            // Color 是值类型（struct），传栈上地址
            float col[4] = { m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3] };
            void* a2[2] = { &colName, col };
            uo::u_call(mat, mc.setColor, a2);
        }

        // 渲染队列：半透明排到 Transparent
        if (mc.setQueue) {
            int32_t q = (m.diffuse[3] < 0.999f) ? 3000 : 2000;
            void* a1q[1] = { &q };
            uo::u_call(mat, mc.setQueue, a1q);
        }

        out.items[i] = mat;
        out.created++;
    }

    if (logf) logf("[MAT] built %d/%zu materials with '%s'", out.created, model.materials.size(), used);
    return out.created > 0;
}

}  // namespace mmd
