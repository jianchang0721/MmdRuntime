// =====================================================================
// texture.h —— PMX 贴图 → UnityEngine.Texture2D
//
// PMX 里 textures[] 存的是一堆相对路径（一般是 tex/xxx.png）。
// 这里把它们相对 PMX 文件所在目录拼成绝对路径，读盘，stb_image 解码，
// 再灌进运行时创建的 Texture2D。
//
// 两个必须记住的坑：
//   1. MMD 贴图原点在左下（OpenGL 惯例），Unity 在左上 → 必须垂直翻转，
//      不然整张脸是倒的
//   2. 贴图路径里可能有反斜杠（Windows 上做的模型），要先统一成 /
// =====================================================================
#pragma once

#include <vector>

#include "pmx.h"

namespace mmd {

struct TextureSet {
    // 下标与 model.textures 一一对应，失败的是 nullptr
    std::vector<void*> items;
    int loaded = 0;
    int failed = 0;
};

// pmxPath：PMX 文件的绝对路径（用来推贴图所在目录）
// logf：printf 风格日志回调，可传 nullptr
// 诊断：造一张棋盘格贴图（验证 UV 对不对：方格在模型表面均匀 = UV 正常）
void* make_checker_texture(int w, int h, int cell, void (*logf)(const char*, ...));

bool load_textures(const Model& model,
                   const char* pmxPath,
                   TextureSet& out,
                   void (*logf)(const char*, ...));

}  // namespace mmd
