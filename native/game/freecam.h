// =====================================================================
// freecam.h —— 新建相机接管画面（而不是抢游戏相机）
//
// 用户给的 C# 参考实现的核心思路：
//   GameObject go = new GameObject("FreeCamera");
//   freeCamera = go.AddComponent<Camera>();
//   freeCamera.CopyFrom(originalCamera);   // 拷全部参数
//   go.transform.SetParent(null);          // 脱离父级
//   freeCamera.depth = originalCamera.depth + 1f;  // 排在游戏相机之后
//   originalCamera.enabled = false;        // 关掉原相机渲染
//
// 为什么这条路能成而我们之前不行：
//   我们之前是每帧改游戏相机的 Transform，游戏每帧又写回去 → 谁后写谁赢，
//   多数是游戏赢，于是「UI 动相机不动」。
//   新建相机之后，游戏爱怎么写原相机就怎么写 —— 它已经不渲染了。
//   我们的相机没人碰，写多少就是多少。
//
// 我们的额外收获：camera_ctrl 的 freeFly 逻辑一行不用改，
// 只要把它的写入目标从「游戏相机」换成「我们的相机」。
// =====================================================================
#pragma once

namespace game {
namespace freecam {

// 用 srcCam（游戏相机）新建一台接管渲染的相机。
// 成功返回新相机 Transform 的 managed 指针（Il2CppObject*），失败返回 nullptr。
void* enable(void* srcCam);

// 销毁新相机，恢复原相机的渲染。幂等。
void disable();

bool  active();
void* camera();       // 新 Camera 的 managed 指针
void* transform();    // 新 Camera 的 Transform
int   depth();

}  // namespace freecam
}  // namespace game
