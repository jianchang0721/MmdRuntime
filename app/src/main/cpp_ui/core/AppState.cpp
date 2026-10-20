#include "AppState.h"

std::string g_BodyFontPath;
std::string g_HeadingFontPath;
ImFont* g_BodyFont = nullptr;
ImFont* g_HeadingFont = nullptr;
bool g_Initialized = false;
bool g_CloseRequested = false;
int g_DisplayW = 1080;
int g_DisplayH = 2400;
std::vector<TouchInputEvent> g_Touches;
std::vector<KeyInputEvent> g_Keys;
std::mutex g_InputMutex;
UIState g_State;
ModuleState g_ModuleState;
std::string g_GameStatusMessage;
std::string g_ConfigDirectory;
bool g_ShowDebugPanel = true;
bool g_TouchPassThroughEnabled = true;
std::string g_LastConfigStatus = "Not loaded";
RuntimeBuildConfig g_BuildConfig;
OverlayRuntimeState g_OverlayState;
std::string g_ActiveProfileId = "default";
ModuleManager g_ModuleManager;
