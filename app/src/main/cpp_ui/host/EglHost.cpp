// =====================================================================
// EglHost.cpp —— UI so 的宿主：hook eglSwapBuffers，把 ImGui 画在游戏帧上
//
// 为什么选这条：
//   · 游戏进程里没有任何 Android 窗口，WindowManager 那条路（悬浮窗）
//     在 MIUI 上要权限、要通知、还会被后台限制，麻烦一堆
//   · eglSwapBuffers 是每帧必经之路，在这里画就是「零成本寄生」
//
// 必须守的规矩（血泪）：
//   1. 绝不在 JNI_OnLoad 里初始化 ImGui —— 那时没有 GL 上下文
//   2. 绝不 glClear —— 会把游戏画面擦掉
//   3. 画完要把 scissor / depth / program / texture / vao 清干净，
//      Unity 下一帧看到脏状态会画面错乱
//   4. 任何异常一律 try/catch 住，UI 挂了不能带崩游戏
// =====================================================================
#include <android/log.h>
#include <jni.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <dlfcn.h>
#include <pthread.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "dobby/dobby.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "AppState.h"
#include "Bridge.h"
#include "ConfigStorage.h"
#include "FeatureFactory.h"
#include "Host.h"
#include "ImGuiRuntime.h"
#include "Modules.h"
#include "RuntimeState.h"
#include "OverlayWindowManager.h"
#include "ScreenManager.h"
#include "Theme.h"
#include "ToastManager.h"
#include "elf_util.h"

#define LOG_TAG "MmdEglHost"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// ---------------------------------------------------------------------
// 宿主状态
// ---------------------------------------------------------------------
struct HostState {
    bool      hooked   = false;
    bool      inited   = false;
    bool      broken   = false;   // 初始化失败过，别再反复试
    int       surfW    = 0;
    int       surfH    = 0;
    int       viewW    = 0;       // Java 侧 decor view 尺寸，用来做坐标映射
    int       viewH    = 0;
    int       frames   = 0;
    bool      lastDown = false;
    EGLBoolean (*origSwap)(EGLDisplay, EGLSurface) = nullptr;
};

HostState g_host;

// 面板当前占的屏幕矩形（上一帧的，够用）
struct PanelRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool  valid = false;
};
PanelRect g_panel;

float MapX(float x) {
    if (g_host.viewW <= 0 || g_host.surfW <= 0) return x;
    return x * (float)g_host.surfW / (float)g_host.viewW;
}
float MapY(float y) {
    if (g_host.viewH <= 0 || g_host.surfH <= 0) return y;
    return y * (float)g_host.surfH / (float)g_host.viewH;
}

// ---------------------------------------------------------------------
// 初始化（第一次 swap 时才有 GL 上下文）
// ---------------------------------------------------------------------
bool InitImGuiOnce(int w, int h) {
    if (g_host.inited) return true;
    if (g_host.broken) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // 别往游戏目录写 imgui.ini
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)w, (float)h);

    Theme::Apply();
    ImGuiRuntime::LoadFonts();    // 字体路径由 Java 侧 setFontPaths 提前喂好

    // 模板自带的那套配置/模块系统：Screens.cpp 里的「模块」页签靠它
    Config::LoadConfig(g_State);
    ApplyBuildMode(g_BuildConfig.mode);
    g_ModuleManager = ModuleManager();
    if (g_BuildConfig.allowDemoModules) {
        g_ModuleManager.RegisterModule(CreateTestModule());
        g_ModuleManager.RegisterModule(CreateDemoModule());
        g_ModuleManager.RegisterModule(CreateBasicModule());
    }

    // 主动连一次游戏侧 API；连不上就每秒重试
    Bridge::Bind();
    Bridge::PollStatus();

    const char* glsl = "#version 300 es";
    if (!ImGui_ImplOpenGL3_Init(glsl)) {
        LOGE("ImGui_ImplOpenGL3_Init(%s) failed", glsl);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_host.broken = true;
        return false;
    }

    WindowManager::SetScreenSize(w, h);
    WindowManager::currentScreen  = Screen::Menu;
    WindowManager::previousScreen = Screen::Login;
    WindowManager::showOverlay    = true;
    WindowManager::collapsed      = false;

    g_DisplayW    = w;
    g_DisplayH    = h;
    g_Initialized = true;
    g_CloseRequested = false;

    g_host.inited = true;
    LOGI("ImGui host ready %dx%d", w, h);
    return true;
}

// ---------------------------------------------------------------------
// 每帧渲染
// ---------------------------------------------------------------------
void RenderFrame(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surf, EGL_WIDTH, &w) != EGL_TRUE) return;
    if (eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) != EGL_TRUE) return;
    if (w <= 0 || h <= 0) return;

    if (!InitImGuiOnce(w, h)) return;

    if (w != g_host.surfW || h != g_host.surfH) {
        g_host.surfW = w;
        g_host.surfH = h;
        ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
        WindowManager::SetScreenSize(w, h);
        g_DisplayW = w;
        g_DisplayH = h;
        LOGI("surface resized %dx%d", w, h);
    }

    ImGuiRuntime::BeginFrameInput();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    Theme::Apply();
    if (WindowManager::showOverlay) {
        ScreenManager::RenderCurrentScreen();
        Toast::Render();
    }

    ImGui::Render();

    // 面板矩形：给「吃掉触摸」用。折叠时窗口很小，游戏照样能操作。
    ImVec2 pos = WindowManager::lastWindowPosition;
    ImVec2 sz  = WindowManager::lastWindowSize;
    g_panel.x = pos.x;
    g_panel.y = pos.y;
    g_panel.w = sz.x;
    g_panel.h = sz.y;
    g_panel.valid = WindowManager::showOverlay && sz.x > 1.0f && sz.y > 1.0f;

    glViewport(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // 把状态还干净，Unity 下一帧照旧
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    g_host.frames++;
    if (g_host.frames == 60 || g_host.frames == 600) {
        LOGI("frames=%d panel=%.0fx%.0f screen=%dx%d", g_host.frames,
             g_panel.w, g_panel.h, g_host.surfW, g_host.surfH);
    }
}

// ---------------------------------------------------------------------
// hook
// ---------------------------------------------------------------------
EGLBoolean HookSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_host.broken) {
        // 这里绝不能抛异常出来：异常穿过 Dobby 的 trampoline 会连累游戏
        try {
            RenderFrame(dpy, surf);
        } catch (...) {
            LOGE("RenderFrame threw, disabling UI");
            g_host.broken = true;
        }
    }
    return g_host.origSwap(dpy, surf);
}

}  // namespace

// =====================================================================
// 对外：Host
// =====================================================================
namespace Host {

void Install() {
    if (g_host.hooked) return;
    g_host.hooked = true;

    uintptr_t base = elfu::MapsBase("libEGL.so");
    uintptr_t addr = base ? elfu::ElfSym(base, "eglSwapBuffers") : 0;
    if (!addr) {
        void* h = dlopen("libEGL.so", RTLD_NOW);
        if (h) addr = (uintptr_t)dlsym(h, "eglSwapBuffers");
    }
    if (!addr) {
        LOGE("eglSwapBuffers not found (base=0x%lx)", (unsigned long)base);
        return;
    }
    LOGI("eglSwapBuffers @ 0x%lx (libEGL base=0x%lx)", (unsigned long)addr, (unsigned long)base);

    int rc = DobbyHook((void*)addr, (void*)HookSwapBuffers, (void**)&g_host.origSwap);
    LOGI("DobbyHook rc=%d orig=%p", rc, (void*)g_host.origSwap);
    if (rc != 0 || !g_host.origSwap) {
        LOGE("hook failed");
        g_host.broken = true;
    }
}

bool IsReady() { return g_host.inited; }

bool WantsTouch(float x, float y) {
    if (!g_host.inited || g_host.broken) return false;
    if (!g_panel.valid) return false;
    float sx = MapX(x);
    float sy = MapY(y);
    // 留 8px 手指容差，边缘点得动
    return sx >= g_panel.x - 8 && sx <= g_panel.x + g_panel.w + 8 &&
           sy >= g_panel.y - 8 && sy <= g_panel.y + g_panel.h + 8;
}

}  // namespace Host

// =====================================================================
// JNI（名字跟 JniBridge.kt 一一对应）
// =====================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setFontPaths(JNIEnv* env, jobject,
                                                        jstring latinPath,
                                                        jstring cjkPath) {
    auto toStr = [&](jstring s) {
        std::string out;
        if (s) {
            const char* raw = env->GetStringUTFChars(s, nullptr);
            if (raw) { out = raw; env->ReleaseStringUTFChars(s, raw); }
        }
        return out;
    };
    g_LatinFontPath   = toStr(latinPath);
    g_CjkFontPath     = toStr(cjkPath);
    g_HeadingFontPath = g_LatinFontPath;
    LOGI("fonts: latin='%s' cjk='%s'", g_LatinFontPath.c_str(), g_CjkFontPath.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setConfigDirectory(JNIEnv* env, jobject, jstring path) {
    if (!path) return;
    const char* raw = env->GetStringUTFChars(path, nullptr);
    if (raw) {
        g_ConfigDirectory = raw;
        env->ReleaseStringUTFChars(path, raw);
    }
}

// 返回 true = 这个触摸归 UI，游戏别收
extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_onTouchEvent(JNIEnv*, jobject,
                                                        jint action, jfloat x, jfloat y,
                                                        jfloat viewW, jfloat viewH) {
    if (viewW > 0 && viewH > 0) {
        g_host.viewW = (int)viewW;
        g_host.viewH = (int)viewH;
    }

    bool eat = Host::WantsTouch(x, y);
    if (!g_host.inited) return JNI_FALSE;

    float sx = MapX(x);
    float sy = MapY(y);

    std::lock_guard<std::mutex> lock(g_InputMutex);
    switch (action) {
        case 0:  // ACTION_DOWN
            g_Touches.push_back({0, sx, sy});
            g_host.lastDown = true;
            break;
        case 1:  // ACTION_UP
            g_Touches.push_back({1, sx, sy});
            g_host.lastDown = false;
            break;
        case 2:  // ACTION_MOVE
            g_Touches.push_back({2, sx, sy});
            break;
        case 3:  // ACTION_CANCEL
            if (g_host.lastDown) g_Touches.push_back({1, sx, sy});
            g_host.lastDown = false;
            break;
        default:
            break;
    }
    return eat ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_onKeyEvent(JNIEnv*, jobject,
                                                      jint unicodeChar, jint keyCode, jboolean isDown) {
    std::lock_guard<std::mutex> lock(g_InputMutex);
    g_Keys.push_back({(int)unicodeChar, (int)keyCode, isDown == JNI_TRUE});
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_wantTextInput(JNIEnv*, jobject) {
    if (!g_host.inited) return JNI_FALSE;
    return ImGui::GetIO().WantTextInput ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_isReady(JNIEnv*, jobject) {
    return Host::IsReady() ? JNI_TRUE : JNI_FALSE;
}

// so 一加载就把 hook 装上，剩下的等第一次 swap
JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    LOGI("libimgui_overlay loaded");
    Host::Install();
    return JNI_VERSION_1_6;
}
