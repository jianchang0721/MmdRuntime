// =====================================================================
// bone_hook.h —— m_Bones 扩容的 hook 层
//
// 为什么 hook：把 SMR 的 m_Bones.data 换成我们的数组后，Unity 在 SMR
// 析构时会 free 这个指针。我们自己 malloc 的指针被它 free 会崩，所以
// 在容器析构执行前把三个字段还原成 Unity 原来的值，让它 free 自己的内存。
//
// hook 点（相对 libunity.so）：m_Bones 那个 dynamic_array 的析构函数。
// 不硬编码偏移，用 7 条指令的特征码扫出来，换游戏版本只要重扫即可。
// =====================================================================
#pragma once
#include <cstdint>

namespace bonehook {

// 扫 libunity.so 的 .text，找到容器析构函数并挂 hook。
// 返回 true 表示 hook 已生效（重复调用直接返回 true）。
bool install();

// 登记"我们的数组"和"原来的三字段"，析构 hook 里靠它判断是不是我们的。
// data 为 nullptr 时表示解除登记（析构时不再干预）。
void set_ours(void* ourData, void* origData, int32_t origSize, int64_t origCap);

bool installed();

// 诊断：返回扫到的函数地址（找不到为 0）
void* found_address();

}  // namespace bonehook
