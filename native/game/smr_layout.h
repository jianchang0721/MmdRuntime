// =====================================================================
// smr_layout.h —— SkinnedMeshRenderer native 字段偏移的运行时探测
//
// 为什么要探测：0x1d4 / 0x1e0 / 0x1f0 / 0x1f8 这组值是拿 Unity 2018.4.13f1
// 的 libunity.so 反汇编挖出来的。游戏一更新、Unity 一升级，这组数就可能变，
// 而硬编码的后果是**静默写错内存**——比崩溃更难查。
//
// 探测原理（不依赖任何硬编码地址）：
//   1. 拿一批 SMR 样本，每个样本有 native 指针、以及两个「真实值」：
//        · meshId   = Object.GetInstanceID(smr.get_sharedMesh())
//        · boneCnt  = smr.get_bones().Length
//   2. 在 native 对象前 0x400 字节里，每 4 字节扫一遍，
//      找「对所有样本都等于 meshId」的偏移 → 那就是 m_Mesh
//      同理找等于 boneCnt 的 → 那是 m_Bones.size
//   3. m_Bones.data 在 size 前面（当前版本差 0x10），capacity 在 size 后面
//      这两个是推断值，会跟 size 一起验证
//
// 安全策略：
//   · 至少 2 个样本才敢探测，单样本无法排除巧合
//   · 必须「唯一匹配」才采信，多处匹配直接放弃、退回默认值
//   · 探测结果跟默认值一起打日志，对不上就人工介入
// =====================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace game {

struct SmrSample {
    void*   native   = nullptr;   // C++ SkinnedMeshRenderer*
    int32_t meshId   = 0;         // GetInstanceID(get_sharedMesh())
    int32_t boneCount = 0;        // get_bones().Length
};

struct SmrLayout {
    int32_t offMesh     = 0x1d4;   // m_Mesh 的 int32 instanceID
    int32_t offBoneData = 0x1e0;   // m_Bones.data
    int32_t offBoneSize = 0x1f0;   // m_Bones.size (int32)
    int32_t offBoneCap  = 0x1f8;   // m_Bones.capacity (int64)

    bool detected = false;         // true = 探测成功，值来自运行时
    int  samples  = 0;             // 参与探测的样本数
    char note[128] = "default";    // 探测结果说明
};

// 全局布局（默认是 Unity 2018.4.13f1 的已知值）
SmrLayout& layout();

// 用样本反推偏移。成功返回 true 并写入 layout()。
// 失败时 layout() 保持原值，note 里写原因。
bool detect_layout(const std::vector<SmrSample>& samples);

// 用单个 SMR 校验当前 layout 是否仍然有效。
// 返回 true = meshId 和 size 都对得上。
bool verify_layout(const SmrSample& s);

}  // namespace game
