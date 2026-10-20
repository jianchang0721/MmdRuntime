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

ImVec2 SizeFor(Screen screen);
void SetScreenSize(int width, int height);
void GoTo(Screen next);
void RequestClose();
void BeginWindow(const char* id, const char* title = nullptr);
void EndWindow();
}
