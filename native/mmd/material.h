// =====================================================================
// material.h —— PMX 材质 → UnityEngine.Material
//
// 每个 PMX 材质做出一个 Unity Material，贴图用 texture.h 解好的那张。
//
// Shader 回退链（游戏里 Shader.Find 能拿到哪一个不确定，从轻到重试）：
//   Unlit/Texture → Mobile/Diffuse → Sprites/Default → Standard → Diffuse
//
// 透明材质靠 renderQueue：alpha < 1 的排到 3000（Transparent），
// 其余 2000（Geometry）。不碰 _Mode / blend 那一套 —— 不同 shader
// 属性名不一样，写错了反而更花。
// =====================================================================
#pragma once

#include <vector>

#include "pmx.h"
#include "texture.h"

namespace mmd {

struct MaterialSet {
    std::vector<void*> items;     // 下标与 model.materials 对应，失败是 nullptr
    int created = 0;
    const char* shaderName = "";  // 实际用上的 shader 名
};

// 诊断开关：0=正常 6=只 ctor 7=跳过 renderQueue 8=跳过 SetColor
void set_material_mode(int mode);
// 诊断：强制用指定的 Shader（传游戏原材质的 shader，绕过 Shader.Find）
void set_shader_override(void* shader);

bool build_materials(const Model& model,
                     const TextureSet& tex,
                     MaterialSet& out,
                     void (*logf)(const char*, ...));

}  // namespace mmd
