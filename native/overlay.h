// =====================================================================
// overlay.h —— ImGui 悬浮窗（画在游戏自己的 GL 帧上，不碰任何 View）
//
// 机制抄 reveny/Android-ImGui-Mod-Menu（★165）：
//   Dobby hook eglSwapBuffers → ImGui NewFrame → 画菜单 → RenderDrawData
//   → 调原函数。没有 View、没有 WindowManager、不需要任何权限。
//
// 输入不走 libinput hook（Android 15+ 符号变了，且要跨 ns）：
//   Xposed 壳 hook dispatchTouchEvent → nativeTouch → ImGuiIO 队列
// =====================================================================
#pragma once
#include <jni.h>
#include <string>

namespace overlay {

// 启动 hook 线程（等 libEGL 就绪后挂 eglSwapBuffers）。drawFn 每帧在 GL 线程被调用。
void init(void (*drawFn)());

// Java → native 触摸转发。返回 true 表示这次事件被菜单吃掉，游戏不应收到。
bool onTouch(int action, float x, float y, float viewW, float viewH);

bool ready();

// Java 侧回调（重载 so / 取状态），由 module.cpp 在 nativeBind 里注入
void bindJava(JavaVM* vm, jobject hookCls, void* midDoLoad, void* midStatus);

// 状态文本（菜单里显示）
void setStatus(const std::string& s);
std::string status();

// 菜单开关
bool visible();
void setVisible(bool v);

}  // namespace overlay
