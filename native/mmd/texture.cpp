// =====================================================================
// texture.cpp —— stb_image 解码 + 运行时 Texture2D
//
// Texture2D 的创建路径（都是实测/记录过的 API）：
//   new Texture2D(w, h, TextureFormat.RGBA32=4, mipChain=false)
//   LoadRawTextureData(byte[])       ← 整块 RGBA 直接灌，最快
//   Apply(updateMipmaps=false, makeNoLongerReadable=false)
//
// LoadRawTextureData 如果被 strip 了，就退回 SetPixels32(Color32[])；
// 两个都没有就只建个纯色贴图，不让整个加载流程挂在这里。
// =====================================================================
#include "logsink.h"
#include "texture.h"

#include <android/log.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_PNM
#include "stb_image.h"

#include "unity/unity_objects.h"

#define TTAG "MmdTexture"
#define TLOG(...) logsink::Write(__VA_ARGS__)

namespace mmd {
namespace {

// TextureFormat.RGBA32
const int kRGBA32 = 4;

struct TexClasses {
    void* texCls   = nullptr;
    void* ctor     = nullptr;   // (int, int, TextureFormat, bool)
    void* loadRaw  = nullptr;   // LoadRawTextureData(byte[])
    void* setPix32 = nullptr;   // SetPixels32(Color32[])
    void* apply    = nullptr;   // Apply(bool, bool)
    void* elemByte = nullptr;   // System.Byte
    void* elemC32  = nullptr;   // UnityEngine.Color32
};

bool init_tex_classes(TexClasses& tc, void (*logf)(const char*, ...)) {
    tc.texCls = uo::u_class("UnityEngine", "Texture2D");
    if (!tc.texCls) { if (logf) logf("[TEX] Texture2D class missing"); return false; }

    tc.ctor     = uo::u_method(tc.texCls, ".ctor", 4);
    tc.loadRaw  = uo::u_method(tc.texCls, "LoadRawTextureData", 1);
    tc.setPix32 = uo::u_method(tc.texCls, "SetPixels32", 1);
    tc.apply    = uo::u_method(tc.texCls, "Apply", 2);
    tc.elemByte = uo::u_class("System", "Byte");
    tc.elemC32  = uo::u_class("UnityEngine", "Color32");

    if (logf) {
        logf("[TEX] ctor=%p loadRaw=%p setPixels32=%p apply=%p byte=%p c32=%p",
             tc.ctor, tc.loadRaw, tc.setPix32, tc.apply, tc.elemByte, tc.elemC32);
    }
    if (!tc.ctor) {
        if (logf) logf("[TEX] Texture2D..ctor(int,int,TextureFormat,bool) missing");
        return false;
    }
    return true;
}

bool read_file(const char* path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    // 日志：文件是否真的打开（build-69 崩在 read 与 decode 之间，先分清）
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

std::string dir_of(const char* path) {
    const char* slash = strrchr(path, '/');
    if (!slash) return std::string(".");
    return std::string(path, (size_t)(slash - path));
}

void* make_texture(const TexClasses& tc,
                   const unsigned char* rgba, int w, int h,
                   void (*logf)(const char*, ...)) {
    void* tex = uo::u_new_object(tc.texCls);
    if (!tex) { if (logf) logf("[TEX] object_new failed"); return nullptr; }

    int32_t iw = w, ih = h, fmt = kRGBA32;
    uint8_t mip = 0;
    void* a4[4] = { &iw, &ih, &fmt, &mip };
    uo::u_call(tex, tc.ctor, a4);

    bool uploaded = false;

    if (tc.loadRaw && tc.elemByte) {
        const size_t bytes = (size_t)w * (size_t)h * 4;
        void* arr = uo::u_new_array(tc.elemByte, bytes);
        if (arr) {
            memcpy(uo::u_array_data(arr), rgba, bytes);
            // 数组参数是值传递（和 set_vertices 同一个坑）：args[0] = 数组对象指针
            void* a1[1] = { arr };
            uo::u_call_void(tex, tc.loadRaw, a1);
            uploaded = true;
        }
    }

    if (!uploaded && tc.setPix32 && tc.elemC32) {
        void* arr = uo::u_new_array(tc.elemC32, (size_t)w * (size_t)h);
        if (arr) {
            // Color32 内存布局：r,g,b,a 四个字节，跟 RGBA8888 一致
            memcpy(uo::u_array_data(arr), rgba, (size_t)w * (size_t)h * 4);
            void* a1[1] = { arr };
            uo::u_call_void(tex, tc.setPix32, a1);
            uploaded = true;
        }
    }

    if (!uploaded && logf) {
        logf("[TEX] no usable upload path, texture will be blank");
    }

    if (tc.apply) {
        uint8_t upd = 0, noRead = 0;
        void* a2[2] = { &upd, &noRead };
        uo::u_call_void(tex, tc.apply, a2);
    }
    return tex;
}

}  // namespace

void* make_checker_texture(int w, int h, int cell, void (*logf)(const char*, ...)) {
    if (w <= 0 || h <= 0) return nullptr;
    if (cell <= 0) cell = 16;
    TexClasses tc;
    if (!init_tex_classes(tc, logf)) return nullptr;

    std::vector<unsigned char> px((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const bool on = ((x / cell) & 1) ^ ((y / cell) & 1);
            unsigned char* p = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
            p[0] = on ? 235 : 30;
            p[1] = on ? 60  : 190;
            p[2] = on ? 60  : 30;
            p[3] = 255;
        }
    }
    void* t = make_texture(tc, px.data(), w, h, logf);
    if (logf) logf("[TEX] checker %dx%d cell=%d -> %p", w, h, cell, t);
    return t;
}

bool load_textures(const Model& model,
                   const char* pmxPath,
                   TextureSet& out,
                   void (*logf)(const char*, ...)) {
    out = TextureSet{};
    out.items.assign(model.textures.size(), nullptr);
    if (model.textures.empty()) return true;

    if (!pmxPath || !pmxPath[0]) {
        if (logf) logf("[TEX] pmxPath is empty, cannot resolve texture dir");
        return false;
    }

    if (logf) logf("[TEX] enter: %zu textures, init classes...", model.textures.size());
    TexClasses tc;
    if (!init_tex_classes(tc, logf)) return false;
    if (logf) logf("[TEX] classes ok");

    const std::string dir = dir_of(pmxPath);
    if (logf) logf("[TEX] base dir = %s, %zu textures", dir.c_str(), model.textures.size());

    for (size_t i = 0; i < model.textures.size(); i++) {
        std::string rel = model.textures[i];
        if (rel.empty()) continue;

        // Windows 风格路径 → 统一成 /
        for (char& c : rel) if (c == '\\') c = '/';
        // 去掉可能的 "./" 前缀
        while (rel.size() > 2 && rel[0] == '.' && rel[1] == '/') rel.erase(0, 2);

        const std::string full = dir + "/" + rel;

        std::vector<unsigned char> raw;
        if (!read_file(full.c_str(), raw)) {
            out.failed++;
            if (logf) logf("[TEX] #%zu MISSING %s", i, full.c_str());
            continue;
        }
        if (logf) logf("[TEX] #%zu read %zu bytes", i, raw.size());

        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(raw.data(), (int)raw.size(),
                                                  &w, &h, &comp, 4);
        if (logf) logf("[TEX] #%zu decode px=%p w=%d h=%d comp=%d",
                       i, (void*)px, w, h, comp);
        if (!px || w <= 0 || h <= 0) {
            out.failed++;
            if (logf) logf("[TEX] #%zu DECODE FAIL %s (comp=%d)", i, full.c_str(), comp);
            if (px) stbi_image_free(px);
            continue;
        }

        // 垂直翻转：MMD 贴图左下为原点，Unity 左上
        std::vector<unsigned char> flipped((size_t)w * (size_t)h * 4);
        const size_t row = (size_t)w * 4;
        for (int y = 0; y < h; y++) {
            memcpy(flipped.data() + (size_t)y * row,
                   px + (size_t)(h - 1 - y) * row,
                   row);
        }
        stbi_image_free(px);

        void* tex = make_texture(tc, flipped.data(), w, h, logf);
        if (tex) {
            out.items[i] = tex;
            out.loaded++;
            if (logf) logf("[TEX] #%zu ok %dx%d %s", i, w, h, full.c_str());
        } else {
            out.failed++;
        }
    }

    if (logf) logf("[TEX] done: loaded=%d failed=%d", out.loaded, out.failed);
    return out.loaded > 0 || out.failed == 0;
}

}  // namespace mmd
