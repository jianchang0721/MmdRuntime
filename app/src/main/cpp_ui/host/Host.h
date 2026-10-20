// =====================================================================
// Host.h —— UI 侧宿主（跑在游戏进程里，hook eglSwapBuffers 画 ImGui）
//
// 生命周期：
//   HookEntry 加载 libimgui_overlay.so
//     → JNI_OnLoad 里调 Host::Install()（只装 hook，不碰 GL）
//     → 游戏第一次 eglSwapBuffers 时才有 GL 上下文，这时才建 ImGui
//     → 之后每帧在 swap 之前画 UI
//
// 为什么不能在 JNI_OnLoad 里建 ImGui：
//   ImGui_ImplOpenGL3_Init 会立刻编 shader，那时候当前线程没有
//   EGL 上下文，GL 调用全是空操作 → 后端初始化半死。
// =====================================================================
#pragma once

namespace Host {

// 装 eglSwapBuffers hook。幂等，可以重复调。
void Install();

// UI 是否已经能画（ImGui 上下文 + GL 后端都就绪）
bool IsReady();

// 面板是否吃某个屏幕坐标的触摸（surface 像素坐标）
bool WantsTouch(float x, float y);


}  // namespace Host
