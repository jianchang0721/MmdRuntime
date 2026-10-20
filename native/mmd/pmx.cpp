// =====================================================================
// pmx.cpp —— PMX 解析实现
//
// 一个原则：任何一处越界都直接失败，绝不带着半截数据往下走。
// =====================================================================
#include "pmx.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace mmd {
namespace {

// 带边界检查的顺序读取器
class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n), o_(0) {}

    bool   ok()   const { return ok_; }
    size_t off()  const { return o_; }
    size_t size() const { return n_; }
    void   fail()       { ok_ = false; }

    bool need(size_t k) {
        if (!ok_) return false;
        if (k > n_ || o_ > n_ - k) { ok_ = false; return false; }
        return true;
    }

    uint8_t  u8()  { if (!need(1)) return 0; return d_[o_++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = 0; memcpy(&v, d_ + o_, 2); o_ += 2; return v; }
    int32_t  i32() { if (!need(4)) return 0; int32_t  v = 0; memcpy(&v, d_ + o_, 4); o_ += 4; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; memcpy(&v, d_ + o_, 4); o_ += 4; return v; }
    float    f32() { if (!need(4)) return 0; float    v = 0; memcpy(&v, d_ + o_, 4); o_ += 4; return v; }

    const uint8_t* raw(size_t k) {
        if (!need(k)) return nullptr;
        const uint8_t* p = d_ + o_;
        o_ += k;
        return p;
    }

    // 带长度的字符串。enc: 0=UTF-16LE, 1=UTF-8
    std::string text(int enc) {
        uint32_t len = u32();
        if (!ok()) return std::string();
        if (len > n_ || o_ > n_ - len) { fail(); return std::string(); }
        const uint8_t* p = raw(len);
        std::string s;
        if (!p) return s;
        if (enc == 0) {
            s.reserve(len / 2);
            for (uint32_t i = 0; i + 1 < len; i += 2) {
                uint32_t c = (uint32_t)(p[i] | (p[i + 1] << 8));
                // 代理对：直接按 BMP 处理，日文用不到
                if (c < 0x80) s.push_back((char)c);
                else if (c < 0x800) {
                    s.push_back((char)(0xC0 | (c >> 6)));
                    s.push_back((char)(0x80 | (c & 0x3F)));
                } else {
                    s.push_back((char)(0xE0 | (c >> 12)));
                    s.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
                    s.push_back((char)(0x80 | (c & 0x3F)));
                }
            }
        } else {
            s.assign((const char*)p, len);
        }
        return s;
    }

    // PMX 的索引：1/2/4 字节，-1 表示无
    int idx(int sz, bool sign) {
        if (sz == 1) return sign ? (int)(int8_t)u8() : (int)u8();
        if (sz == 2) return sign ? (int)(int16_t)u16() : (int)u16();
        return i32();
    }

private:
    const uint8_t* d_;
    size_t n_;
    size_t o_;
    bool   ok_ = true;
};

}  // namespace

bool Load(const char* path, Model& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) { err = "cannot open file"; return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); err = "seek failed"; return false; }
    long sz = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); err = "seek failed"; return false; }
    if (sz <= 0 || sz > 256L * 1024 * 1024) { fclose(f); err = "bad file size"; return false; }

    std::vector<uint8_t> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) { err = "short read"; return false; }

    Reader r(buf.data(), buf.size());

    const uint8_t* magic = r.raw(4);
    if (!magic || memcmp(magic, "PMX ", 4) != 0) { err = "not a PMX file"; return false; }

    out.version = r.f32();
    if (out.version < 2.0f || out.version > 2.2f) { err = "unsupported PMX version"; return false; }

    int gc = r.u8();
    if (gc < 8) { err = "globals count < 8"; return false; }
    uint8_t g[8] = {0};
    for (int i = 0; i < gc; i++) {
        uint8_t v = r.u8();
        if (i < 8) g[i] = v;
    }
    out.encoding  = g[0];
    out.addUV     = g[1];
    out.vIdxSize  = g[2];
    out.tIdxSize  = g[3];
    out.mIdxSize  = g[4];
    out.bIdxSize  = g[5];
    out.moIdxSize = g[6];
    out.rIdxSize  = g[7];

    if (out.vIdxSize != 1 && out.vIdxSize != 2 && out.vIdxSize != 4) { err = "bad vertex index size"; return false; }
    if (out.bIdxSize != 1 && out.bIdxSize != 2 && out.bIdxSize != 4) { err = "bad bone index size"; return false; }
    if (out.tIdxSize != 1 && out.tIdxSize != 2 && out.tIdxSize != 4) { err = "bad texture index size"; return false; }
    if (out.addUV > 4) { err = "addUV > 4"; return false; }

    out.name   = r.text(out.encoding);
    out.nameEn = r.text(out.encoding);
    r.text(out.encoding);   // comment
    r.text(out.encoding);   // commentEn
    if (!r.ok()) { err = "header truncated"; return false; }

    // ---- 顶点 ----
    int32_t nv = r.i32();
    if (nv < 0 || nv > 2000000) { err = "bad vertex count"; return false; }
    out.verts.clear();
    out.verts.resize((size_t)nv);
    for (int32_t i = 0; i < nv; i++) {
        if (!r.ok()) break;
        Vertex& v = out.verts[(size_t)i];
        v.pos[0] = r.f32(); v.pos[1] = r.f32(); v.pos[2] = r.f32();
        v.nrm[0] = r.f32(); v.nrm[1] = r.f32(); v.nrm[2] = r.f32();
        v.uv[0]  = r.f32(); v.uv[1]  = r.f32();
        for (int a = 0; a < out.addUV; a++)
            for (int k = 0; k < 4; k++) v.addUV[a][k] = r.f32();

        uint8_t wt = r.u8();
        v.type = (int)wt;
        switch (wt) {
            case WT_BDEF1:
                v.bone[0] = r.idx(out.bIdxSize, true);
                v.weight[0] = 1.0f;
                break;
            case WT_BDEF2:
                v.bone[0] = r.idx(out.bIdxSize, true);
                v.bone[1] = r.idx(out.bIdxSize, true);
                v.weight[0] = r.f32();
                v.weight[1] = 1.0f - v.weight[0];
                break;
            case WT_BDEF4:
            case WT_QDEF:
                for (int k = 0; k < 4; k++) v.bone[k]   = r.idx(out.bIdxSize, true);
                for (int k = 0; k < 4; k++) v.weight[k] = r.f32();
                break;
            case WT_SDEF:
                v.bone[0] = r.idx(out.bIdxSize, true);
                v.bone[1] = r.idx(out.bIdxSize, true);
                v.weight[0] = r.f32();
                v.weight[1] = 1.0f - v.weight[0];
                for (int k = 0; k < 3; k++) v.sdefC[k]  = r.f32();
                for (int k = 0; k < 3; k++) v.sdefR0[k] = r.f32();
                for (int k = 0; k < 3; k++) v.sdefR1[k] = r.f32();
                break;
            default:
                err = "unknown weight type";
                return false;
        }
        v.edgeScale = r.f32();
        if (wt < WT_COUNT) out.weightCount[wt]++;
    }
    if (!r.ok()) { err = "vertex block truncated"; return false; }

    // ---- 面 ----
    int32_t nf = r.i32();   // 索引数量
    if (nf < 0 || nf > 20000000) { err = "bad face count"; return false; }
    out.faceVertex.clear();
    out.faceVertex.resize((size_t)nf);
    for (int32_t i = 0; i < nf; i++) {
        if (!r.ok()) break;
        out.faceVertex[(size_t)i] = r.idx(out.vIdxSize, false);
    }
    if (!r.ok()) { err = "face block truncated"; return false; }

    // ---- 贴图 ----
    int32_t nt = r.i32();
    if (nt < 0 || nt > 10000) { err = "bad texture count"; return false; }
    out.textures.clear();
    out.textures.reserve((size_t)nt);
    for (int32_t i = 0; i < nt; i++) {
        if (!r.ok()) break;
        out.textures.push_back(r.text(out.encoding));
    }
    if (!r.ok()) { err = "texture block truncated"; return false; }

    // ---- 材质 ----
    int32_t nm = r.i32();
    if (nm < 0 || nm > 10000) { err = "bad material count"; return false; }
    out.materials.clear();
    out.materials.resize((size_t)nm);
    for (int32_t i = 0; i < nm; i++) {
        if (!r.ok()) break;
        Material& m = out.materials[(size_t)i];
        m.name   = r.text(out.encoding);
        m.nameEn = r.text(out.encoding);
        for (int k = 0; k < 4; k++) m.diffuse[k]  = r.f32();
        for (int k = 0; k < 3; k++) m.specular[k] = r.f32();
        m.specularStrength = r.f32();
        for (int k = 0; k < 3; k++) m.ambient[k]  = r.f32();
        m.drawFlag = r.u8();
        for (int k = 0; k < 4; k++) m.edgeColor[k] = r.f32();
        m.edgeSize = r.f32();
        m.texIndex    = r.idx(out.tIdxSize, true);
        m.sphereIndex = r.idx(out.tIdxSize, true);
        m.sphereMode  = r.u8();
        m.toonFlag    = r.u8();
        // toonFlag=0 用 texture 索引宽度；=1 是一个字节
        if (m.toonFlag == 0) m.toonIndex = r.idx(out.tIdxSize, true);
        else                 m.toonIndex = (int)r.u8();
        m.memo = r.text(out.encoding);
        m.surfaceCount = r.u32();
    }
    if (!r.ok()) { err = "material block truncated"; return false; }

    // ---- 骨骼 ----
    int32_t nb = r.i32();
    if (nb < 0 || nb > 10000) { err = "bad bone count"; return false; }
    out.bones.clear();
    out.bones.resize((size_t)nb);
    for (int32_t i = 0; i < nb; i++) {
        if (!r.ok()) break;
        Bone& b = out.bones[(size_t)i];
        b.name   = r.text(out.encoding);
        b.nameEn = r.text(out.encoding);
        b.pos[0] = r.f32(); b.pos[1] = r.f32(); b.pos[2] = r.f32();
        b.parent = r.idx(out.bIdxSize, true);
        b.layer  = r.i32();
        b.flags  = r.u16();

        if (b.flags & 0x0001) {
            b.tailIndex = r.idx(out.bIdxSize, true);
        } else {
            b.tailOffset[0] = r.f32();
            b.tailOffset[1] = r.f32();
            b.tailOffset[2] = r.f32();
        }
        if (b.flags & 0x0100) { r.idx(out.bIdxSize, true); r.f32(); }        // 付与回転
        if (b.flags & 0x0200) { r.idx(out.bIdxSize, true); r.f32(); }        // 付与移動
        if (b.flags & 0x0400) { r.f32(); r.f32(); r.f32(); }                 // 軸固定
        if (b.flags & 0x0800) { for (int k = 0; k < 6; k++) r.f32(); }       // ローカル軸
        if (b.flags & 0x1000) { r.i32(); }                                   // 外部親
        if (b.flags & 0x0020) {                                              // IK
            b.hasIk    = true;
            b.ikTarget = r.idx(out.bIdxSize, true);
            b.ikLoop   = r.i32();
            b.ikLimit  = r.f32();
            int32_t linkCount = r.i32();
            if (linkCount < 0 || linkCount > 1000) { err = "bad ik link count"; return false; }
            for (int32_t k = 0; k < linkCount; k++) {
                if (!r.ok()) break;
                b.ikLinks.push_back(r.idx(out.bIdxSize, true));
                if (b.flags & 0x0080) { for (int q = 0; q < 6; q++) r.f32(); }   // 角度制限
            }
        }
    }
    if (!r.ok()) { err = "bone block truncated"; return false; }

    out.bytesRead = r.off();
    return true;
}

void DumpStats(const Model& m, void (*logf)(const char*, ...)) {
    logf("[PMX] name='%s' version=%.2f encoding=%s addUV=%d",
         m.name.c_str(), m.version, m.encoding == 0 ? "UTF16LE" : "UTF8", m.addUV);
    logf("[PMX] index sizes: vertex=%d texture=%d material=%d bone=%d morph=%d rigid=%d",
         m.vIdxSize, m.tIdxSize, m.mIdxSize, m.bIdxSize, m.moIdxSize, m.rIdxSize);
    logf("[PMX] verts=%zu faces=%zu (tris=%zu) textures=%zu materials=%zu bones=%zu",
         m.verts.size(), m.faceVertex.size(), m.TriangleCount(),
         m.textures.size(), m.materials.size(), m.bones.size());
    logf("[PMX] weights: BDEF1=%d BDEF2=%d BDEF4=%d SDEF=%d QDEF=%d",
         m.weightCount[WT_BDEF1], m.weightCount[WT_BDEF2], m.weightCount[WT_BDEF4],
         m.weightCount[WT_SDEF], m.weightCount[WT_QDEF]);
    logf("[PMX] parsed bytes=%zu", m.bytesRead);

    int maxDepth = 0, badParent = 0, ikCount = 0;
    for (size_t i = 0; i < m.bones.size(); i++) {
        int d = 0, cur = (int)i;
        while (cur >= 0) {
            if (cur >= (int)m.bones.size()) { badParent++; break; }
            cur = m.bones[(size_t)cur].parent;
            if (++d > 128) { badParent++; break; }
        }
        if (d > maxDepth) maxDepth = d;
        if (m.bones[i].isIk()) ikCount++;
    }
    logf("[PMX] bone tree: maxDepth=%d ik=%d badParent=%d", maxDepth, ikCount, badParent);

    for (size_t i = 0; i < m.bones.size() && i < 15; i++) {
        const Bone& b = m.bones[i];
        logf("[PMX]   bone[%2zu] %-18s pos=(%7.3f,%7.3f,%7.3f) parent=%3d flags=0x%04x",
             i, b.name.c_str(), b.pos[0], b.pos[1], b.pos[2], b.parent, b.flags);
    }
    for (size_t i = 0; i < m.materials.size() && i < 8; i++) {
        const Material& mt = m.materials[i];
        logf("[PMX]   mat[%zu] %-18s tex=%2d surf=%u alpha=%.2f",
             i, mt.name.c_str(), mt.texIndex, mt.surfaceCount, mt.diffuse[3]);
    }
    for (size_t i = 0; i < m.textures.size() && i < 8; i++) {
        logf("[PMX]   tex[%zu] %s", i, m.textures[i].c_str());
    }
}

}  // namespace mmd
