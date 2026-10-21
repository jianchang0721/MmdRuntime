// =====================================================================
// smr_layout.cpp
//
// 探测顺序：先验证默认值，不对再扫描。
// 扫描要求「唯一匹配」——如果前 1KB 里有两处都等于 meshId，说明样本
// 区分度不够，宁可不采信也不瞎猜（写错内存是静默的，比崩溃难查十倍）。
// =====================================================================
#include "smr_layout.h"

#include <cstdio>
#include <cstring>

#include "logsink.h"

#define LTAG "MmdLayout"
#define LLOG(...) logsink::Write(__VA_ARGS__)

namespace game {

namespace {

SmrLayout g_layout;

const int kScanLimit = 0x400;   // 扫 native 对象前 1KB

// 在所有样本上找「唯一」的偏移，使得读出的 int32 == 每个样本的期望值
int find_unique_int32(const std::vector<SmrSample>& s,
                      int32_t (*want)(const SmrSample&)) {
    int hit = -1;
    int hitCount = 0;

    for (int off = 0; off < kScanLimit; off += 4) {
        bool all = true;
        for (const SmrSample& x : s) {
            if (!x.native) { all = false; break; }
            if (*(int32_t*)((char*)x.native + off) != want(x)) { all = false; break; }
        }
        if (all) {
            hit = off;
            hitCount++;
            if (hitCount > 1) break;   // 多于一处，直接放弃
        }
    }
    return (hitCount == 1) ? hit : -1;
}

}  // namespace

SmrLayout& layout() { return g_layout; }

bool detect_layout(const std::vector<SmrSample>& samples) {
    if (samples.size() < 2) {
        snprintf(g_layout.note, sizeof(g_layout.note),
                 "样本不足(%zu<2)", samples.size());
        LLOG("[LAYOUT] 探测跳过：%s", g_layout.note);
        return false;
    }

    // ---- 先看默认值还能不能用 ----
    int okMesh = 0, okSize = 0, valid = 0;
    for (const SmrSample& x : samples) {
        if (!x.native) continue;
        valid++;
        if (*(int32_t*)((char*)x.native + g_layout.offMesh) == x.meshId) okMesh++;
        if (*(int32_t*)((char*)x.native + g_layout.offBoneSize) == x.boneCount) okSize++;
    }
    if (valid >= 2 && okMesh == valid && okSize == valid) {
        g_layout.detected = true;
        g_layout.samples  = valid;
        snprintf(g_layout.note, sizeof(g_layout.note), "默认值验证通过");
        LLOG("[LAYOUT] 默认偏移全部命中 mesh=0x%x data=0x%x size=0x%x cap=0x%x (n=%d)",
             g_layout.offMesh, g_layout.offBoneData,
             g_layout.offBoneSize, g_layout.offBoneCap, valid);
        return true;
    }

    LLOG("[LAYOUT] 默认值失效 (mesh %d/%d, size %d/%d)，开始扫描",
         okMesh, valid, okSize, valid);

    // ---- 扫描 ----
    int meshOff = find_unique_int32(samples, [](const SmrSample& x) { return x.meshId; });
    int sizeOff = find_unique_int32(samples, [](const SmrSample& x) { return x.boneCount; });

    if (meshOff < 0 || sizeOff < 0) {
        snprintf(g_layout.note, sizeof(g_layout.note),
                 "扫描失败 mesh=%d size=%d（多处匹配或无匹配）", meshOff, sizeOff);
        LLOG("[LAYOUT] %s", g_layout.note);
        return false;
    }

    g_layout.offMesh     = meshOff;
    g_layout.offBoneSize = sizeOff;
    // data/capacity 按当前版本的结构规律推断：
    //   data @ size-0x10（中间隔一个末尾索引）
    //   cap  @ size+0x08（int64，紧跟 size）
    g_layout.offBoneData = sizeOff - 0x10;
    g_layout.offBoneCap  = sizeOff + 0x08;
    g_layout.detected    = true;
    g_layout.samples     = valid;
    snprintf(g_layout.note, sizeof(g_layout.note), "扫描命中");
    LLOG("[LAYOUT] 扫描结果 mesh=0x%x data=0x%x size=0x%x cap=0x%x (n=%d)",
         g_layout.offMesh, g_layout.offBoneData,
         g_layout.offBoneSize, g_layout.offBoneCap, valid);
    return true;
}

bool verify_layout(const SmrSample& s) {
    if (!s.native) return false;
    int32_t m = *(int32_t*)((char*)s.native + g_layout.offMesh);
    int32_t n = *(int32_t*)((char*)s.native + g_layout.offBoneSize);
    return m == s.meshId && n == s.boneCount;
}

}  // namespace game
