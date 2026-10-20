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

    // ---- 关键：保存当前 framebuffer / viewport ----
    //
    // 游戏完全可能正在渲染到自己的 FBO（后处理、UI 层、后缓冲）。
    // 我们为了把 UI 画到屏幕上会临时 bind 0，但**必须还原**——
    // 否则下一帧游戏把画面画到屏幕而不是它的 FBO，表现就是黑屏。
    GLint savedFbo = 0;
    GLint savedVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
    glGetIntegerv(GL_VIEWPORT, savedVp);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // 还原
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFbo);
    glViewport(savedVp[0], savedVp[1], savedVp[2], savedVp[3]);

    // 注意：不要再手动 glDisable/glBind 一大片。
    // ImGui 的 OpenGL3 后端自己在 RenderDrawData 结尾已经把 blend/depth/
    // cull/scissor/viewport 都恢复成进入前的值了，我们再覆盖一遍就是倒帮忙。
    // （黑屏那版就是在这里把 Unity 的 depth test 给关了）

    g_host.frames++;
    if (g_host.frames == 60 || g_host.frames == 600) {
        LOGI("frames=%d panel=%.0fx%.0f screen=%dx%d", g_host.frames,
             g_panel.w, g_panel.h, g_host.surfW, g_host.surfH);
    }
}

// ---------------------------------------------------------------------
// hook
// ---------------------------------------------------------------------
std::mutex g_renderMutex;   // 多 surface / 多线程 swap 时串行进 ImGui

EGLBoolean HookSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!g_host.broken) {
        // 这里绝不能抛异常出来：异常穿过 Dobby 的 trampoline 会连累游戏
        try {
            std::lock_guard<std::mutex> lock(g_renderMutex);
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
// 对外的 C 接口
//
// 为什么还要这一层：libimgui_overlay.so 现在是游戏侧 so 用
// android_dlopen_ext 从 memfd 加载的，ART 根本不认识它，
// Java_com_xxx 形式的 JNI 符号没人会去解析。
//
// 所以真正的入口放在 libmmdruntime.so，它 dlsym("ui_touch") 找到这里。
// System.load 兜底路径下 JNI 符号也还在（同一批实现，只是多包一层）。
// =====================================================================
extern "C" __attribute__((visibility("default"))) void ui_set_fonts(const char* latin, const char* cjk) {
    if (latin) g_LatinFontPath = latin;
    if (cjk)   g_CjkFontPath = cjk;
    g_HeadingFontPath = g_LatinFontPath;
    LOGI("fonts: latin='%s' cjk='%s'", g_LatinFontPath.c_str(), g_CjkFontPath.c_str());
}

extern "C" __attribute__((visibility("default"))) int ui_ready(void) {
    return Host::IsReady() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int ui_want_text_input(void) {
    if (!g_host.inited) return 0;
    return ImGui::GetIO().WantTextInput ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) const char* ui_build_tag(void) {
    return "imgui-overlay v1.2";
}

// 装 eglSwapBuffers hook。
//
// 必须自己调：android_dlopen_ext 加载的 so 不会被 ART 当模块看待，
// JNI_OnLoad 根本不会被调。主路径下这一步由 mmdruntime 显式调。
extern "C" __attribute__((visibility("default"))) void ui_init(void) {
    LOGI("ui_init");
    Host::Install();
}

extern "C" __attribute__((visibility("default")))
int ui_touch(int action, float x, float y, float viewW, float viewH) {
    if (viewW > 0 && viewH > 0) {
        g_host.viewW = (int)viewW;
        g_host.viewH = (int)viewH;
    }

    bool eat = Host::WantsTouch(x, y);
    if (!g_host.inited) return 0;

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
    return eat ? 1 : 0;
}

extern "C" __attribute__((visibility("default")))
void ui_key(int unicodeChar, int keyCode, int isDown) {
    std::lock_guard<std::mutex> lock(g_InputMutex);
    g_Keys.push_back({unicodeChar, keyCode, isDown != 0});
}

// =====================================================================
// JNI 壳子（只在 System.load 兜底路径下使用）
//
// 主路径是 mmdruntime 用 android_dlopen_ext 加载本 so，那时 ART 不认识
// 这个模块，下面这堆符号根本没人解析 —— 真正被调的是上面的 ui_*。
// =====================================================================
static std::string JStr(JNIEnv* env, jstring s) {
    std::string out;
    if (s) {
        const char* raw = env->GetStringUTFChars(s, nullptr);
        if (raw) { out = raw; env->ReleaseStringUTFChars(s, raw); }
    }
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_setFontPaths(JNIEnv* env, jobject, jstring latin, jstring cjk) {
    std::string l = JStr(env, latin), c = JStr(env, cjk);
    ui_set_fonts(l.c_str(), c.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_service_JniBridge_onKeyEvent(JNIEnv*, jobject, jint u, jint k, jboolean down) {
    ui_key((int)u, (int)k, down == JNI_TRUE ? 1 : 0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_wantTextInput(JNIEnv*, jobject) {
    return ui_want_text_input() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_moss_mmdruntime_service_JniBridge_isReady(JNIEnv*, jobject) {
    return ui_ready() ? JNI_TRUE : JNI_FALSE;
}

// 两条加载路径都要装 hook：
//   · System.load（兜底）→ 这里
//   · android_dlopen_ext → 同样会调 JNI_OnLoad（且不要求 JavaVM 有效）
JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    LOGI("libimgui_overlay loaded via System.load (%s)", ui_build_tag());
    ui_init();
    return JNI_VERSION_1_6;
}
