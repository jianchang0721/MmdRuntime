// =====================================================================
// bone_names.h —— 读游戏骨骼名 + 「获取骨骼名称」的 hook 点
//
// 为什么要这个：
//   1. MMD 的 Delta 局部重定向要按名字把 PMX 骨骼对到游戏骨骼，
//      没有名字就只能盲猜索引（游戏每套模型骨骼顺序都不一样）。
//   2. 游戏内部也有「按名字找骨骼」的逻辑（换装/挂点/特效），
//      hook 住读名字的入口就能反查到那个函数，以后可以精准挂它。
//
// hook 点定位（不写死任何地址）：
//   Unity 所有读名字的 C# 调用最终都进 UnityEngine.Object::get_name，
//   它的 MethodInfo->methodPointer 就是入口。MethodInfo 是 il2cpp 运行时
//   对象，指针从方法表拿 —— 游戏更新、Unity 升级都不影响定位方式。
//
// 注意：get_name 是高频函数（UI 每帧都在调），hook 只做极轻的记录，
//       超过配额直接透传；而且装之前会先确认 methodPointer 不是
//       「所有 icall 共用的 thunk」，是的话宁可不装。
// =====================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game {

struct BoneNameEntry {
    int32_t     instanceId = 0;
    std::string name;
};

// 读一个 SMR 的全部骨骼名（走 managed get_bones + Object.get_name）。
// 成功返回 true，out 按 bones[] 顺序填好。
bool collect_bone_names(void* smrManaged, std::vector<BoneNameEntry>& out);

// 「获取骨骼名称」的 hook 点：Object::get_name 的 methodPointer。
// 拿不到返回 nullptr。这个值随时可以重新取，不需要缓存到磁盘。
void* name_hook_target();

// 装 hook：记录前 maxRecords 次调用的 名字 + 调用者相对模块的偏移。
// 用来反查游戏内部哪个函数在读骨骼名（拿到偏移就能固化成基址定位）。
// 返回 false 时会打日志说明原因（方法指针共用 thunk / Dobby 失败）。
bool install_name_hook(int maxRecords);

bool name_hook_installed();

// 打印收集到的骨骼名（最多前 maxPrint 条，避免刷爆 logd）
void dump_bone_names(const std::vector<BoneNameEntry>& v, int maxPrint);

}  // namespace game
