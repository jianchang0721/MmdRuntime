// =====================================================================
// pmx.h —— PMX 2.0 / 2.1 数据结构
//
// 只做"读文件 + 校验"，不碰 Unity、不碰任何渲染。
// 所有字段都按 PMX 规范定义，不做 MMD2BIP 之类的业务映射（那是上层的事）。
// =====================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mmd {

enum WeightType {
    WT_BDEF1 = 0,
    WT_BDEF2 = 1,
    WT_BDEF4 = 2,
    WT_SDEF  = 3,
    WT_QDEF  = 4,
    WT_COUNT = 5,
};

struct Vertex {
    float pos[3] = {0, 0, 0};
    float nrm[3] = {0, 0, 0};
    float uv[2]  = {0, 0};
    float addUV[4][4] = {{0}};
    int   type = WT_BDEF1;
    int   bone[4]   = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    float sdefC[3]  = {0, 0, 0};
    float sdefR0[3] = {0, 0, 0};
    float sdefR1[3] = {0, 0, 0};
    float edgeScale = 1.0f;
};

struct Bone {
    std::string name;
    std::string nameEn;
    float pos[3] = {0, 0, 0};
    int   parent = -1;
    int   layer = 0;
    uint16_t flags = 0;

    int   tailIndex = -1;
    float tailOffset[3] = {0, 1, 0};

    bool  hasIk = false;
    int   ikTarget = -1;
    int   ikLoop = 0;
    float ikLimit = 0;
    std::vector<int> ikLinks;

    bool isIk() const       { return (flags & 0x0020) != 0; }
    bool hasTailIndex() const { return (flags & 0x0001) != 0; }
};

struct Material {
    std::string name;
    std::string nameEn;
    float diffuse[4] = {1, 1, 1, 1};
    float specular[3] = {0, 0, 0};
    float specularStrength = 0;
    float ambient[3] = {0, 0, 0};
    uint8_t drawFlag = 0;
    float edgeColor[4] = {0, 0, 0, 1};
    float edgeSize = 0;
    int   texIndex = -1;
    int   sphereIndex = -1;
    uint8_t sphereMode = 0;
    uint8_t toonFlag = 0;
    int   toonIndex = -1;
    std::string memo;
    uint32_t surfaceCount = 0;   // 这个材质占用的索引数量（不是三角形数）
};

struct Model {
    float version = 0;
    int encoding = 0;
    int addUV = 0;
    // 各类索引的字节宽度（1/2/4）
    int vIdxSize = 2, tIdxSize = 1, mIdxSize = 1, bIdxSize = 2, moIdxSize = 1, rIdxSize = 2;

    std::string name;
    std::string nameEn;

    std::vector<Vertex>       verts;
    std::vector<int>          faceVertex;   // 索引数组，每 3 个一组
    std::vector<std::string>  textures;
    std::vector<Material>     materials;
    std::vector<Bone>         bones;

    int  weightCount[WT_COUNT] = {0, 0, 0, 0, 0};
    size_t bytesRead = 0;

    size_t TriangleCount() const { return faceVertex.size() / 3; }
};

// 读文件并解析。失败时 err 里是可读原因，out 内容不可用。
bool Load(const char* path, Model& out, std::string& err);

// 把解析结果打成日志（logf 是 printf 风格的回调）
void DumpStats(const Model& m, void (*logf)(const char*, ...));

}  // namespace mmd
