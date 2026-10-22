// =====================================================================
// cam_override.h —— 自由相机：hook 写入侧而不是主动写
//
// 老做法（camera_ctrl.cpp）为什么「UI 动相机不动」：
//   我们在 eglSwapBuffers 里主动 invoke set_position/set_rotation，
//   而游戏在自己的 Update 里每帧也会写相机。谁在后谁赢——
//   游戏的写在渲染前，我们的写在 swap 前，多数情况是游戏的赢。
//   表现就是：UI 上参数明明变了，画面里的相机不动。
//
// 新做法（参考 gakumas-localify 的 freecam 修复）：
//   hook Transform 的 *_Injected 写入接口，在游戏自己写相机的那一刻
//   把参数改成我们的值。游戏随后用它自己的原生路径应用，
//   我们一个字都不用写，也永远不会被覆盖。
//
// 相机对象靠 hook Camera.get_main / Component.get_transform 被动捕获：
//   主动 invoke get_main 拿到的不一定是游戏真正在用的那台。
// =====================================================================
#pragma once

namespace game {
namespace cam_override {

// 装 hook。幂等；失败返回 false 并说明原因。
bool install();

// 每帧算出我们想要的相机 pose。内部会做 NaN 保护。
// 返回 false = 暂不可用（没有捕获到相机 Transform）。
// 由外部（camera_ctrl）每帧填 yaw/pitch/pos，这里只负责搬运。
void set_desired_pose(const float pos[3], const float quat[4]);

// 当前是否已经捕获到相机 Transform
bool  captured();
void* camera();
void* transform();

}  // namespace cam_override
}  // namespace game
