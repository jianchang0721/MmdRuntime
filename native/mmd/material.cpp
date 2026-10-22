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

    // ★ 老项目 ModelMod 验证过的路：mainTexture / color / shader 都是"属性"，
    //   不依赖具体 shader 的贴图属性名（_MainTex / _BaseMap / _Tex 都行）。
    //   build-137 实测：用 SetTexture("_MainTex") 时八个候选属性探测全是 0，
    //   材质读回也是 null —— 游戏 shader 根本不认这个名字。
    void* setMainTex  = nullptr;  // set_mainTexture(Texture)
    void* getMainTex  = nullptr;  // get_mainTexture()
    void* setColorP   = nullptr;  // set_color(Color)
    void* getShader   = nullptr;  // get_shader()
};

// 顺序跟 ModelMod 一致：Mobile/Diffuse 当首选（它读标准 _MainTex，兼容性最好），
// Unlit/Texture 放后面——本 build 实测它一建材质就崩渲染命令流。
const char* kShaderChain[] = {
    "Mobile/Diffuse",
    "Unlit/Texture",
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

    // 属性式（不依赖 shader 的贴图属性名）——ModelMod 那边就是靠这个出图的
    mc.setMainTex = uo::u_method(mc.matCls, "set_mainTexture", 1);
    mc.getMainTex = uo::u_method(mc.matCls, "get_mainTexture", 0);
    mc.setColorP  = uo::u_method(mc.matCls, "set_color", 1);
    mc.getShader  = uo::u_method(mc.matCls, "get_shader", 0);

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

    // HasProperty 有 (string) 和 (int) 两个重载。build-137 用 u_method 按参数个数乱抓，
    // 抓到 int 版，拿字符串指针当 int 用 -> 八个属性全报 0，白得了一次误判。
    // 必须按第一个参数类型筛。
    void* mHasProp = find_method_typed(mc.matCls, "HasProperty", 1, "String");
    void* mGetTextureStr = find_method_typed(mc.matCls, "get_texture", 1, "String");
    if (logf) logf("[MAT] 探测方法 HasProperty(string)=%p get_texture(string)=%p",
                   mHasProp, mGetTextureStr);

    Il2CppString* colName = g_api.string_new("_Color");

    for (size_t i = 0; i < model.materials.size(); i++) {
        const Material& m = model.materials[i];

        void* mat = uo::u_new_object(mc.matCls);
        if (!mat) { if (logf) logf("[MAT] #%zu object_new failed", i); continue; }

        // Shader 是引用类型：args[0] = 对象指针本身，不是 &shader
        // （build-68 就是死在这：Material..ctor 拿到栈地址当 Shader，fault addr 0）
        void* a1[1] = { shader };
        bool ctorOk = uo::u_call_void(mat, mc.ctorShader, a1);
        void* shBack = mc.getShader ? uo::u_call(mat, mc.getShader, nullptr) : nullptr;
        if (logf) logf("[MAT] #%zu ctor done mat=%p 无异常=%d shader读回=%p %s",
                       i, mat, (int)ctorOk, shBack,
                       shBack ? "OK" : "<-- 材质是空壳！");

        if (g_matMode == 6) {
            out.items[i] = mat;
            out.created++;
            continue;   // 只留 ctor，什么都不设
        }

        // 贴图：★ 用 mainTexture 属性（ModelMod 验证过的路），
        // 不管 shader 内部把主图叫什么名字都能绑上。
        {
            void* t = nullptr;
            if (m.texIndex >= 0 && (size_t)m.texIndex < tex.items.size()) {
                t = tex.items[(size_t)m.texIndex];
            }
            if (t) {
                // 先探这个 shader 的主图属性叫什么名字（string 重载）
                int pick = -1;
                if (mHasProp) {
                    for (int k = 0; k < kTexPropCount; k++) {
                        if (!texPropStr[k]) continue;
                        void* a1p[1] = { texPropStr[k] };
                        void* r = uo::u_call(mat, mHasProp, a1p);
                        if (r && *(uint8_t*)((char*)r + 16)) { pick = k; break; }
                    }
                }

                if (pick >= 0 && mc.setTexture) {
                    void* a2[2] = { texPropStr[pick], t };
                    uo::u_call_void(mat, mc.setTexture, a2);
                } else if (mc.setMainTex) {
                    // 一个候选名都不存在就上 mainTexture（Unity 会自己映射到主图属性）
                    void* am[1] = { t };
                    uo::u_call_void(mat, mc.setMainTex, am);
                }

                if (i < 3) {
                    void* got = nullptr;
                    if (pick >= 0 && mGetTextureStr) {
                        void* a1g[1] = { texPropStr[pick] };
                        got = uo::u_call(mat, mGetTextureStr, a1g);
                    } else if (mc.getMainTex) {
                        got = uo::u_call(mat, mc.getMainTex, nullptr);
                    }
                    if (logf) logf("[MAT] #%zu 贴图 -> %s 设=%p 读回=%p %s",
                                   i, pick >= 0 ? kTexProps[pick] : "<mainTexture>", t, got,
                                   got == t ? "OK" : (got ? "指针不同" : "<-- 没绑上！"));

                    // 把候选名单全探一遍，把 shader 真有的属性名记下来
                    if (i == 0 && mHasProp) {
                        char line[300]; int off = 0; line[0] = 0;
                        for (int k = 0; k < kTexPropCount; k++) {
                            if (!texPropStr[k]) continue;
                            void* a1p[1] = { texPropStr[k] };
                            void* r = uo::u_call(mat, mHasProp, a1p);
                            int has = (r && *(uint8_t*)((char*)r + 16)) ? 1 : 0;
                            off += snprintf(line + off, sizeof(line) - off, " %s=%d", kTexProps[k], has);
                        }
                        if (logf) logf("[MAT] 属性探测(string版):%s", line);
                    }
                }
            }
        }

        // 颜色：先用 color 属性（不依赖 _Color 这个属性名），再兜底 SetColor("_Color")
        if (mc.setColorP && g_matMode != 8) {
            float col[4] = { m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3] };
            void* ap[1] = { col };
            uo::u_call_void(mat, mc.setColorP, ap);
        } else if (mc.setColor && colName && g_matMode != 8) {
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
