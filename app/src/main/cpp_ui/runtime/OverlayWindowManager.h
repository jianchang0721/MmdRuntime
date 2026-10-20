#pragma once

#include "TemplateModels.h"
#include "imgui.h"

namespace WindowManager {
extern Screen currentScreen;
extern Screen previousScreen;
extern int screenW;
extern int screenH;
extern ImVec2 lastWindowPosition;
extern ImVec2 lastWindowSize;
extern bool showOverlay;
extern bool isDragging;
extern bool collapsed;

ImVec2 SizeFor(Screen screen);
void SetScreenSize(int width, int height);

// ---- 面板拖拽 ----
// 面板画在游戏的 GL 帧上，没有 Android 窗口可以依附，
// 位置完全由我们自己维护。
extern ImVec2 panelPos;

// 从触摸事件列表里挑出属于「拖拽面板」的那些，吃掉并不再传给 ImGui，
// 剩下的原样留在 events 里。
void ProcessDrag(std::vector<TouchInputEvent>& events);

bool IsDragging();
void GoTo(Screen next);
void RequestClose();
// 折叠：主面板收起成可拖动横栏；展开：横栏恢复主面板
void RequestCollapse();
void RequestExpand();
void BeginWindow(const char* id, const char* title = nullptr);
void EndWindow();
}
