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
// C# 里没写、但 Android + 这个游戏上必须补的三件事：
//   1) DontDestroyOnLoad —— 这游戏切场景从不重启进程；不钉住的话场景一换
//      我们的 GameObject 就被 Unity 回收，g_cam 变野指针，下一帧写就是踩内存
//   2) gchandle 钉住 managed 包装对象 —— 否则 GC 回收后指针野掉
//   3) alive() 存活检查 —— 切场景后原相机也换了，要能发现并重建
// =====================================================================
#pragma once

namespace game {
namespace freecam {

// 用 srcCam（游戏相机）新建一台接管渲染的相机。
// 成功返回新相机 Transform 的 managed 指针（Il2CppObject*），失败返回 nullptr。
void* enable(void* srcCam);

// 销毁新相机，恢复原相机的渲染。幂等。
void disable();

// 只清内部指针，不碰 Unity 对象。切场景后对象已经被 Unity 回收时用这个，
// 免得对野指针调 Object.Destroy。
void forget();

// managed 对象还活着吗（Object.op_Equality(x, null)）。
// 查不动（拿不到 op_Equality）时返回 true，宁可多写一帧也别把相机判死。
bool alive(void* managedObj);

bool  active();
void* camera();       // 新 Camera 的 managed 指针
void* transform();    // 新 Camera 的 Transform
void* gameCamera();   // 被我们关掉的游戏相机
int   depth();

}  // namespace freecam
}  // namespace game
