// =====================================================================
// layer_guard.h —— 模型层 + 相机 cullingMask 守卫
//
// 来历（ModelMod 时代踩出来的，v84 → v88 摸了五个版本才定形）：
//   v84  拷 layer：不设 layer 的话，相机 cullingMask 里没有默认层就什么都不渲染
//   v85  加 layer 指示器：悬浮窗显示 camMask / 模型层 / 角色层
//   v87  记住自建 GO，为下一步做准备
//   v88  ensure_model_visible()：周期检查，相机 mask 没我们的层就 OR 进去
//        原因写得很清楚：切场景时引擎会改 cullingMask，补一次不够
//
// MmdRuntime 之前只做了 v84（拷 layer），v88 这一步一直没搬过来。
// 这个模块就是补它：模型层设成角色同层 + 每台相机都保证开着这一层。
//
// 另外：layerCullDistances 是「按 layer 设最大渲染距离」，非 0 的层超过
// 距离整块不画，症状和「挂角色身上看不见」一模一样，所以一并 dump。
// =====================================================================
#pragma once

namespace game {
namespace layer_guard {

// 把 sourceSmr 所在 GameObject 的 layer 拷到 go 上，读写都打印。
// 返回读回值（真值）；-1 = 读失败。
int apply(void* go, void* sourceSmr);

// 登记当前活动的模型 GO（周期守卫只认这一个）
void register_model(void* go, int layer);
void unregister_model();

// 立刻检查并补一次 cullingMask（加载完成时调一次）
void ensure_once();

// tick 里调，内部自己 200ms 节流
void tick();

}  // namespace layer_guard
}  // namespace game
