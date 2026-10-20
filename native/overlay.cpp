// =====================================================================
// overlay.cpp —— ImGui 悬浮菜单实现
//
// 关键点（都踩过）：
//  1. ImGui 初始化必须在 GL 线程、有 current context 的时候做 —— 也就是
//     第一次 eglSwapBuffers 里。不能在 JNI_OnLoad 里做。
//  2. Dobby hook eglSwapBuffers 拿原函数指针，退出前必须调原函数，
//     否则游戏画面卡死。
//  3. 中文字体：系统里抓 NotoSansCJK，抓不到就退回内置英文字体。
//  4. 触摸事件用 ImGui 1.87+ 的 AddMousePosEvent 队列，不要直接改 io.MousePos。
// =====================================================================
#include "overlay.h"

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "dobby/dobby.h"

#define OTAG "MmdOverlay"

static void olog_impl(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, OTAG, fmt, ap);
    va_end(ap);
    // 同时落文件：游戏日志量大，logcat 环形缓冲会冲掉早期记录
    FILE* f = fopen("/data/data/com.hermes.h1game/files/mmdruntime.log", "a");
    if (f) {
        va_list ap2; va_start(ap2, fmt);
        vfprintf(f, fmt, ap2); fputc('\n', f);
        va_end(ap2);
        fclose(f);
    }
}
#define OLOG(...) olog_impl(__VA_ARGS__)

namespace overlay {

// ---------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------
static void (*g_drawFn)() = nullptr;
static bool g_ready = false;          // ImGui 已初始化
static bool g_hooked = false;
static bool g_visible = false;        // 菜单展开
static int  g_glW = 0, g_glH = 0;
static float g_viewW = 1, g_viewH = 1;
static float g_touchX = 0, g_touchY = 0;

static std::mutex g_statusMtx;
static std::string g_status = "booting";

// 菜单矩形（每帧更新，用于判断触摸是否落在菜单上）
static float g_menuX = 0, g_menuY = 0, g_menuW = 0, g_menuH = 0;
static float g_btnX = 0, g_btnY = 0, g_btnW = 0, g_btnH = 0;

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMtx);
    g_status = s;
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_statusMtx);
    return g_status;
}
bool ready() { return g_ready; }
bool visible() { return g_visible; }
void setVisible(bool v) { g_visible = v; }

// ---------------------------------------------------------------------
// Java 回调（重载 so 等按钮要用）
// ---------------------------------------------------------------------
static JavaVM* g_vm = nullptr;
static jobject g_hookCls = nullptr;
static jmethodID g_midDoLoad = nullptr;
static jmethodID g_midStatus = nullptr;

void bindJava(JavaVM* vm, jobject hookCls, void* doLoad, void* statusM) {
    g_vm = vm;
    g_hookCls = hookCls;
    g_midDoLoad = (jmethodID)doLoad;
    g_midStatus = (jmethodID)statusM;
}

static std::string callStatus() {
    if (!g_vm || !g_hookCls || !g_midStatus) return "";
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return "";
        attached = true;
    }
    std::string out;
    jstring js = (jstring)env->CallStaticObjectMethod((jclass)g_hookCls, g_midStatus);
    if (js) {
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c) { out = c; env->ReleaseStringUTFChars(js, c); }
        env->DeleteLocalRef(js);
    }
    if (attached) g_vm->DetachCurrentThread();
    return out;
}

static std::string callDoLoad() {
    if (!g_vm || !g_hookCls || !g_midDoLoad) return "no java bridge";
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return "attach fail";
        attached = true;
    }
    std::string out;
    jstring js = (jstring)env->CallStaticObjectMethod((jclass)g_hookCls, g_midDoLoad);
    if (js) {
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c) { out = c; env->ReleaseStringUTFChars(js, c); }
        env->DeleteLocalRef(js);
    }
    if (attached) g_vm->DetachCurrentThread();
    return out;
}

// ---------------------------------------------------------------------
// ImGui 初始化（必须在 GL 线程 + current context）
// ---------------------------------------------------------------------
static bool load_cjk_font(ImGuiIO& io) {
    const char* paths[] = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSansSC-Regular.ttf",
        "/system/fonts/MiSans-Regular.ttf",
        "/system/fonts/DroidSansFallback.ttf",
    };
    for (auto p : paths) {
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        ImFont* font = io.Fonts->AddFontFromFileTTF(p, 30.0f, nullptr,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font) { OLOG("[OVERLAY] font ok: %s", p); return true; }
        OLOG("[OVERLAY] font load failed: %s", p);
    }
    OLOG("[OVERLAY] no CJK font, fallback default");
    io.Fonts->AddFontDefault();
    return false;
}

static void setup_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;           // 不写 imgui.ini
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.DisplaySize = ImVec2((float)g_glW, (float)g_glH);

    const char* ver = (const char*)glGetString(GL_VERSION);
    bool es3 = ver && strstr(ver, "OpenGL ES 3") != nullptr;
    OLOG("[OVERLAY] GL_VERSION=%s  -> %s", ver ? ver : "?", es3 ? "ES3" : "ES2");
    if (!es3) {
        // 游戏跑 ES2 上下文时本后端编进来的是 ES3 符号，硬跑必炸。
        // 直接放弃这个设备上的 overlay，原生逻辑照常。
        OLOG("[OVERLAY] ES2 context, overlay disabled");
        ImGui::DestroyContext();
        return;
    }

    ImGui_ImplOpenGL3_Init("#version 300 es");
    load_cjk_font(io);

    // 手指操作，所有控件放大一点
    ImGui::GetStyle().ScaleAllSizes(2.2f);
    ImGui::GetStyle().WindowRounding = 12.0f;
    ImGui::GetStyle().FrameRounding = 8.0f;

    g_ready = true;
    OLOG("[OVERLAY] imgui ready  surface=%dx%d", g_glW, g_glH);
}

// ---------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------
static inline bool inRect(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

static void draw_frame() {
    if (!g_ready) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_glW, (float)g_glH);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // ---- 收起态：左上角一个小按钮 ----
    if (!g_visible) {
        ImGui::SetNextWindowPos(ImVec2(24, 220), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::Begin("##ball", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(124, 77, 255, 210));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(150, 110, 255, 235));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(90, 50, 200, 255));
        if (ImGui::Button("MMD", ImVec2(120, 84))) g_visible = true;
        ImGui::PopStyleColor(3);

        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 s = ImGui::GetWindowSize();
        g_btnX = p.x; g_btnY = p.y; g_btnW = s.x; g_btnH = s.y;
        g_menuX = g_menuY = g_menuW = g_menuH = 0;
        ImGui::End();
    } else {
        // ---- 展开态：主面板 ----
        ImGui::SetNextWindowSize(ImVec2(720, 900), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(40, 160), ImGuiCond_FirstUseEver);
        ImGui::Begin("MmdRuntime", &g_visible, ImGuiWindowFlags_NoSavedSettings);

        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 s = ImGui::GetWindowSize();
        g_menuX = p.x; g_menuY = p.y; g_menuW = s.x; g_menuH = s.y;
        g_btnX = g_btnY = g_btnW = g_btnH = 0;

        // 状态区
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(127, 212, 255, 255));
        ImGui::TextWrapped("%s", status().c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // 按钮
        if (ImGui::Button("刷新状态", ImVec2(200, 0))) {
            setStatus(callStatus());
        }
        ImGui::SameLine();
        if (ImGui::Button("重载 so", ImVec2(200, 0))) {
            setStatus(callDoLoad());
        }
        ImGui::SameLine();
        if (ImGui::Button("收起", ImVec2(160, 0))) {
            g_visible = false;
        }

        ImGui::Separator();
        if (g_drawFn) g_drawFn();     // 模块自己的内容（后面放骨骼/模型控制）

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------------------------------------------------------------
// eglSwapBuffers hook
// ---------------------------------------------------------------------
using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
static SwapFn g_origSwap = nullptr;

static EGLBoolean hook_swap(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) { g_glW = w; g_glH = h; }

    if (!g_ready && g_glW > 0 && g_glH > 0) {
        setup_imgui();
    }
    if (g_ready) {
        draw_frame();
    }
    return g_origSwap(dpy, surf);
}

static void setup_imgui();
static void draw_frame();

using SwapDamageFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint);
static SwapDamageFn g_origSwapDamage = nullptr;

static EGLBoolean hook_swap_damage(EGLDisplay dpy, EGLSurface surf, EGLint* rects, EGLint n) {
    // 复用同一个绘制路径，再走原函数（带 damage 的版本）
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) { g_glW = w; g_glH = h; }
    if (!g_ready && g_glW > 0 && g_glH > 0) setup_imgui();
    if (g_ready) draw_frame();
    return g_origSwapDamage(dpy, surf, rects, n);
}

// dlsym(RTLD_DEFAULT) 在 app 的 classloader-namespace 里不保证能看到 libEGL
// （libil2cpp 那次就是这么翻车的 → 手工解析 ELF .dynsym）
static uintptr_t maps_base_of(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    static char line[2048];
    uintptr_t lowest = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        uintptr_t a = strtoull(line, nullptr, 16);
        uint8_t* p = (uint8_t*)a;
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') { fclose(f); return a; }
        if (!lowest || a < lowest) lowest = a;
    }
    fclose(f);
    return lowest;
}

static uintptr_t elf_sym_of(uintptr_t base, const char* name) {
    if (!base) return 0;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;
    uintptr_t dyn = 0;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = base + ph[i].p_vaddr; break; }
    if (!dyn) return 0;
    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    for (ElfW(Dyn)* d = (ElfW(Dyn)*)dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
            case DT_STRTAB: strtab = d->d_un.d_ptr; break;
            case DT_HASH:   hash = d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu = d->d_un.d_ptr; break;
        }
    }
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    if (hash && hash < base) hash += base;
    if (gnu && gnu < base) gnu += base;
    if (!symtab || !strtab) return 0;
    uint32_t nsym = 0;
    if (hash) nsym = ((uint32_t*)hash)[1];
    else if (gnu) {
        uint32_t* g = (uint32_t*)gnu;
        uint32_t nb = g[0], off = g[1], bloom = g[2];
        uint32_t* buckets = &g[4 + bloom * 2];
        uint32_t* chain = &buckets[nb];
        uint32_t last = 0;
        for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= off) { while (!(chain[last - off] & 1)) last++; nsym = last + 1; }
    }
    if (!nsym || nsym > 200000) return 0;
    ElfW(Sym)* syms = (ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    size_t n = strlen(name);
    for (uint32_t i = 0; i < nsym; i++) {
        uint32_t o = syms[i].st_name;
        if (!o || o > 0xFFFFFF) continue;
        const char* s = strs + o;
        size_t k = 0;
        while (k <= n && s[k] == name[k]) k++;
        if (k == n + 1) return base + syms[i].st_value;
    }
    return 0;
}

static bool hook_swap_buffers() {
    void* sym = nullptr;
    uintptr_t eglbase = maps_base_of("libEGL.so");
    if (eglbase) {
        uintptr_t a = elf_sym_of(eglbase, "eglSwapBuffers");
        if (a) sym = (void*)a;
    }
    if (!sym) {
        sym = dlsym(RTLD_DEFAULT, "eglSwapBuffers");   // 兜底
    }
    if (!sym) {
        static bool logged = false;
        if (!logged) { logged = true; OLOG("[OVERLAY] eglSwapBuffers not found (eglbase=%p)", (void*)eglbase); }
        return false;
    }
    int rc = DobbyHook(sym, (void*)hook_swap, (void**)&g_origSwap);
    if (rc != 0 || !g_origSwap) {
        OLOG("[OVERLAY] DobbyHook eglSwapBuffers failed rc=%d", rc);
        return false;
    }
    g_hooked = true;
    OLOG("[OVERLAY] hooked eglSwapBuffers @%p  orig=%p", sym, (void*)g_origSwap);

    // Unity 某些路径走 KHR 版本，一并挂上（挂不上也无所谓）
    void* sym2 = dlsym(RTLD_DEFAULT, "eglSwapBuffersWithDamageKHR");
    if (sym2) {
        if (DobbyHook(sym2, (void*)hook_swap_damage, (void**)&g_origSwapDamage) == 0)
            OLOG("[OVERLAY] hooked eglSwapBuffersWithDamageKHR @%p", sym2);
    }
    return true;
}

static void* hook_thread(void*) {
    // 等 libEGL 出现（游戏可能还没起来）
    for (int i = 0; i < 300 && !g_hooked; i++) {
        if (hook_swap_buffers()) break;
        usleep(300 * 1000);
    }
    if (!g_hooked) OLOG("[OVERLAY] gave up hooking");
    return nullptr;
}

void init(void (*drawFn)()) {
    g_drawFn = drawFn;
    static pthread_t t;
    pthread_create(&t, nullptr, hook_thread, nullptr);
    pthread_detach(t);
}

// ---------------------------------------------------------------------
// 触摸
// ---------------------------------------------------------------------
bool onTouch(int action, float x, float y, float viewW, float viewH) {
    if (!g_ready) return false;
    if (viewW > 1) g_viewW = viewW;
    if (viewH > 1) g_viewH = viewH;

    // 把 view 坐标映射到 surface 坐标
    float sx = x * ((float)g_glW / g_viewW);
    float sy = y * ((float)g_glH / g_viewH);

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(sx, sy);

    bool down = false;
    switch (action) {
        case 0: /*DOWN*/   down = true;  break;
        case 1: /*UP*/     down = false; break;
        case 2: /*MOVE*/   return g_visible && inRect(sx, sy, g_menuX, g_menuY, g_menuW, g_menuH);
        case 3: /*CANCEL*/ down = false; break;
        default: return false;
    }
    io.AddMouseButtonEvent(0, down);

    // 收起态：点在球上 → 展开，吃掉事件
    if (!g_visible) {
        if (action == 0 && inRect(sx, sy, g_btnX, g_btnY, g_btnW, g_btnH)) return true;
        return false;
    }
    // 展开态：点在面板上 → 吃掉
    return inRect(sx, sy, g_menuX, g_menuY, g_menuW, g_menuH);
}

}  // namespace overlay
