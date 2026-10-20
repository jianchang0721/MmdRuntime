#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "Modules.h"
#include "TemplateModels.h"
#include "imgui.h"

extern std::string g_BodyFontPath;
extern std::string g_HeadingFontPath;
extern ImFont* g_BodyFont;
extern ImFont* g_HeadingFont;
extern bool g_Initialized;
extern bool g_CloseRequested;
extern int g_DisplayW;
extern int g_DisplayH;
extern std::vector<TouchInputEvent> g_Touches;
extern std::vector<KeyInputEvent> g_Keys;
extern std::mutex g_InputMutex;
extern UIState g_State;
extern ModuleState g_ModuleState;
extern std::string g_GameStatusMessage;
extern std::string g_ConfigDirectory;
extern bool g_ShowDebugPanel;
extern bool g_TouchPassThroughEnabled;
extern std::string g_LastConfigStatus;
extern RuntimeBuildConfig g_BuildConfig;
extern OverlayRuntimeState g_OverlayState;
extern std::string g_ActiveProfileId;
extern ModuleManager g_ModuleManager;
