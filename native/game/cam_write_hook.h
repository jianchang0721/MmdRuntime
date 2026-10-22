// =====================================================================
// cam_write_hook.h —— hook libunity 原生 Transform 写入，接管相机
//
// 为什么不是 managed 那层：
//   build-155 实测，hook il2cpp 的 Transform.set_position_Injected 等
//   装了 7 个但一次都不命中。Unity 内部 C++ 代码调 Transform 走的是
//   原生直路（Transform::SetPositionAndRotation / ::SetPosition），
//   根本不经过 managed icall。
//
// 所以 hook 目标换成 libunity.so 里的原生函数。用特征码定位（符合
// 用户「不许硬编码偏移」的要求），且 hook 用 ShadowHook 而不是 Dobby：
//   build-88/92 已实证 Dobby 在 libunity 里二次 hook 会 abort。
// =====================================================================
#pragma once

namespace game {
namespace cam_hook {

// 装 hook（幂等）。camTr = 要接管的相机 Transform（managed 对象）。
bool install(void* camTr);

// 是否已装且已捕获
bool active();

// 设本帧期望的相机 pose
void set_desired(const float pos[3], const float quat[4]);

// 调试
int  hit_count();

}  // namespace cam_hook
}  // namespace game
