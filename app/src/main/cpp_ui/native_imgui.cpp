#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "AppState.h"
#include "Bridge.h"
#include "BridgeConfig.h"
#include "ConfigStorage.h"
#include "DebugPanel.h"
#include "ImGuiRuntime.h"
#include "Modules.h"
#include "OverlayWindowManager.h"
#include "RecoveryManager.h"
#include "RuntimeState.h"
#include "ScreenManager.h"
#include "Theme.h"
#include "ToastManager.h"

#define LOG_TAG "ImGuiTemplate"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {

static std::string GetJavaString(JNIEnv* env, jstring value) {
    if (!value) return std::string();
    const char* raw = env->GetStringUTFChars(value, nullptr);
    if (!raw) return std::string();
    std::string out(raw);
    env->ReleaseStringUTFChars(value, raw);
    return out;
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setFontPaths(JNIEnv* env, jobject,
                                                        jstring latinPath,
                                                        jstring cjkPath) {
    // latin = 基字体（含拉丁/数字/符号），cjk = 以 MergeMode 合并进去的中文字形
    g_LatinFontPath = GetJavaString(env, latinPath);
    g_CjkFontPath = GetJavaString(env, cjkPath);
    g_HeadingFontPath = g_LatinFontPath;
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setConfigDirectory(JNIEnv* env, jobject, jstring path) {
    const char* value = path ? env->GetStringUTFChars(path, nullptr) : nullptr;
    g_ConfigDirectory = value ? value : "";
    if (value) env->ReleaseStringUTFChars(path, value);
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_updateOverlayRuntimeState(JNIEnv*, jobject,
                                                              jboolean overlayPermission,
                                                              jboolean drawOverApps,
                                                              jboolean surfaceActive,
                                                              jboolean appForeground,
                                                              jint width,
                                                              jint height) {
    OverlayState::UpdateFromAndroid(overlayPermission == JNI_TRUE,
                                    drawOverApps == JNI_TRUE,
                                    surfaceActive == JNI_TRUE,
                                    appForeground == JNI_TRUE,
                                    (int)width,
                                    (int)height);
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setScreenSize(JNIEnv*, jobject, jint width, jint height) {
    WindowManager::SetScreenSize((int)width, (int)height);
    g_OverlayState.screenWidth = (int)width;
    g_OverlayState.screenHeight = (int)height;
    g_OverlayState.landscape = width > height;
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_initImGui(JNIEnv*, jobject) {
    if (g_Initialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_Initialized = false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    Theme::Apply();
    ImGuiRuntime::LoadFonts();
    Bridge::EnsureDirectory();
    Bridge::RefreshModelList();
    Bridge::FlushUiConfig();
    Config::LoadConfig(g_State);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ApplyBuildMode(g_BuildConfig.mode);
    g_ModuleManager = ModuleManager();
    if (g_BuildConfig.allowDemoModules) {
        g_ModuleManager.RegisterModule(CreateTestModule());
        g_ModuleManager.RegisterModule(CreateDemoModule());
        g_ModuleManager.RegisterModule(CreateBasicModule());
    }
    WindowManager::currentScreen = Screen::Menu;
    WindowManager::previousScreen = Screen::Login;
    WindowManager::showOverlay = true;
    OverlayState::SetRendererReady(true);
    OverlayState::SetSurfaceActive(true);
    OverlayState::SetImguiReady(true);
    Recovery::RunStartupRecovery();
    g_CloseRequested = false;
    g_Initialized = true;
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_resizeImGui(JNIEnv*, jobject, jint width, jint height) {
    // 这里是【悬浮窗自己】的尺寸，不是屏幕尺寸。
    // 旧代码把 WindowManager::SetScreenSize 也喂了这个值，于是：
    //   屏幕 := 上一帧窗口大小 → SizeFor 算出更小的目标 → 应用到 WindowManager
    //   → surface 变更 → 再喂回来 …… 几何级数收敛到 320x420 的下限。
    // 现在只更新 ImGui 的绘制区域，屏幕尺寸由 setScreenSize 独占。
    g_DisplayW = (int)width;
    g_DisplayH = (int)height;
    ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
    g_OverlayState.surfaceActive = true;
    glViewport(0, 0, width, height);
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_stepAndRenderImGui(JNIEnv*, jobject) {
    if (!g_Initialized) return;
    ImGuiRuntime::BeginFrameInput();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    Theme::Apply();
    // Android owns the actual WindowManager x/y and pass-through rectangle.
    // Native owns the active screen and desired window size reported to Kotlin.
    Recovery::RunFrameRecoveryIfNeeded();
    ScreenManager::RenderCurrentScreen();
    DebugPanel::Render();
    Toast::Render();

    ImGui::Render();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_onTouchEvent(JNIEnv*, jobject, jint action, jfloat x, jfloat y) {
    std::lock_guard<std::mutex> lock(g_InputMutex);
    g_Touches.push_back({(int)action, (float)x, (float)y});
}

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_onKeyEvent(JNIEnv*, jobject, jint unicodeChar, jint keyCode, jboolean isDown) {
    std::lock_guard<std::mutex> lock(g_InputMutex);
    g_Keys.push_back({(int)unicodeChar, (int)keyCode, isDown == JNI_TRUE});
}

JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_wantTextInput(JNIEnv*, jobject) {
    if (!g_Initialized) return JNI_FALSE;
    return ImGui::GetIO().WantTextInput ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_checkCloseRequested(JNIEnv*, jobject) {
    bool requested = g_CloseRequested;
    g_CloseRequested = false;
    return requested ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jfloatArray JNICALL
Java_com_moss_mmdruntime_service_JniBridge_getDesiredWindowSize(JNIEnv* env, jobject) {
    ImVec2 size = WindowManager::SizeFor(WindowManager::currentScreen);
    jfloatArray result = env->NewFloatArray(2);
    jfloat data[2] = { size.x, size.y };
    env->SetFloatArrayRegion(result, 0, 2, data);
    return result;
}

}
