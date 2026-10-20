#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <mutex>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "AppState.h"
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

JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setFontPaths(JNIEnv* env, jobject, jstring bodyPath, jstring headingPath) {
    const char* body = bodyPath ? env->GetStringUTFChars(bodyPath, nullptr) : nullptr;
    const char* heading = headingPath ? env->GetStringUTFChars(headingPath, nullptr) : nullptr;
    g_BodyFontPath = body ? body : "";
    g_HeadingFontPath = heading ? heading : "";
    if (body) env->ReleaseStringUTFChars(bodyPath, body);
    if (heading) env->ReleaseStringUTFChars(headingPath, heading);
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
    g_DisplayW = (int)width;
    g_DisplayH = (int)height;
    ImGui::GetIO().DisplaySize = ImVec2((float)width, (float)height);
    WindowManager::SetScreenSize((int)width, (int)height);
    OverlayState::UpdateFromAndroid(g_OverlayState.overlayPermissionGranted,
                                    g_OverlayState.drawOverAppsEnabled,
                                    true,
                                    g_OverlayState.appInForeground,
                                    (int)width,
                                    (int)height);
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
