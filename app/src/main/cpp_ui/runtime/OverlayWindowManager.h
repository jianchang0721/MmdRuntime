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
void GoTo(Screen next);
void RequestClose();
// 折叠：主面板收起成可拖动横栏；展开：横栏恢复主面板
void RequestCollapse();
void RequestExpand();
void BeginWindow(const char* id, const char* title = nullptr);
void EndWindow();
}
