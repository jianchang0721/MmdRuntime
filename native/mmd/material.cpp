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

// 诊断开关（必须在 find_first_shader 之前定义）
//   0 = 正常（贴图 + 颜色 + 渲染队列）
//   6 = 只 new Material(shader)，属性一个不设
//   7 = 跳过 set_renderQueue
//   8 = 跳过 SetColor
static int   g_matMode = 0;
static void* g_shaderOverride = nullptr;

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
    if (g_shaderOverride) {
        if (logf) logf("[MAT] 用游戏原材质的 shader（诊断）%p", g_shaderOverride);
        *used = "<game-shader>";
        return g_shaderOverride;
    }
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

void set_material_mode(int m) { g_matMode = m; }
void set_shader_override(void* shader) { g_shaderOverride = shader; }

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

    // 主贴图属性名候选：游戏角色 shader 不一定叫 _MainTex。
    // 先用 HasProperty 探一圈，哪个存在就用哪个；一个都没有才回退 _MainTex。
    static const char* kTexProps[] = {
        "_MainTex", "_BaseMap", "_BaseColorMap", "_MainTexture",
        "_Tex", "_DiffuseTex", "_Albedo", "_BodyTex",
    };
    const int kTexPropCount = (int)(sizeof(kTexProps) / sizeof(kTexProps[0]));
    Il2CppString* texPropStr[8] = { nullptr };
    for (int k = 0; k < kTexPropCount && k < 8; k++)
        texPropStr[k] = g_api.string_new(kTexProps[k]);

    void* mHasProp = uo::u_method(mc.matCls, "HasProperty", 1);

    Il2CppString* colName = g_api.string_new("_Color");

    for (size_t i = 0; i < model.materials.size(); i++) {
        const Material& m = model.materials[i];

        void* mat = uo::u_new_object(mc.matCls);
        if (!mat) { if (logf) logf("[MAT] #%zu object_new failed", i); continue; }

        // Shader 是引用类型：args[0] = 对象指针本身，不是 &shader
        // （build-68 就是死在这：Material..ctor 拿到栈地址当 Shader，fault addr 0）
        void* a1[1] = { shader };
        uo::u_call_void(mat, mc.ctorShader, a1);
        if (logf) logf("[MAT] #%zu ctor done mat=%p", i, mat);

        if (g_matMode == 6) {
            out.items[i] = mat;
            out.created++;
            continue;   // 只留 ctor，什么都不设
        }

        // 贴图
        if (mc.setTexture) {
            void* t = nullptr;
            if (m.texIndex >= 0 && (size_t)m.texIndex < tex.items.size()) {
                t = tex.items[(size_t)m.texIndex];
            }
            if (t) {
                // 探哪个属性名真存在
                int pick = -1;
                if (mHasProp) {
                    for (int k = 0; k < kTexPropCount; k++) {
                        if (!texPropStr[k]) continue;
                        void* a1p[1] = { texPropStr[k] };
                        void* r = uo::u_call(mat, mHasProp, a1p);
                        if (r && *(uint8_t*)((char*)r + 16)) { pick = k; break; }
                    }
                }
                if (pick < 0) pick = 0;   // 一个都没探到，硬上 _MainTex

                void* a2[2] = { texPropStr[pick], t };
                uo::u_call_void(mat, mc.setTexture, a2);

                // 读回验证（前 5 个材质打，别刷屏）
                if (i < 5) {
                    void* mGet = uo::u_method(mc.matCls, "get_texture", 1);
                    void* got = nullptr;
                    if (mGet) {
                        void* a1g[1] = { texPropStr[pick] };
                        got = uo::u_call(mat, mGet, a1g);
                    }
                    if (logf) logf("[MAT] #%zu 贴图 -> '%s' 设=%p 读回=%p %s",
                                   i, kTexProps[pick], t, got, got ? "OK" : "<-- 没绑上！");

                    // 第一次把候选名单全探一遍，把 shader 到底有哪些属性名记下来
                    if (i == 0 && mHasProp) {
                        char line[256]; int off = 0; line[0] = 0;
                        for (int k = 0; k < kTexPropCount; k++) {
                            if (!texPropStr[k]) continue;
                            void* a1p[1] = { texPropStr[k] };
                            void* r = uo::u_call(mat, mHasProp, a1p);
                            int has = (r && *(uint8_t*)((char*)r + 16)) ? 1 : 0;
                            off += snprintf(line + off, sizeof(line) - off, " %s=%d", kTexProps[k], has);
                        }
                        if (logf) logf("[MAT] 属性探测:%s", line);
                    }
                }
            }
        }

        // 颜色（PMX 的 diffuse 是 RGBA，正好对上 Unity Color 的内存布局）
        if (mc.setColor && colName && g_matMode != 8) {
            // Color 是值类型（struct），传栈上地址
            float col[4] = { m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3] };
            // colName 是引用类型传指针，col 是值类型数组名（本身就是地址）
            void* a2[2] = { colName, col };
            uo::u_call_void(mat, mc.setColor, a2);
        }

        // 渲染队列：半透明排到 Transparent
        if (mc.setQueue && g_matMode != 7) {
            int32_t q = (m.diffuse[3] < 0.999f) ? 3000 : 2000;
            void* a1q[1] = { &q };
            uo::u_call_void(mat, mc.setQueue, a1q);
        }

        out.items[i] = mat;
        out.created++;
    }

    if (logf) logf("[MAT] built %d/%zu materials with '%s'", out.created, model.materials.size(), used);
    return out.created > 0;
}

}  // namespace mmd
