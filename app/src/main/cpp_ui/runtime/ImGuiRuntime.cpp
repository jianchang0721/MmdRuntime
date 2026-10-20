#include "ImGuiRuntime.h"

#include <android/log.h>

#include <cstdio>
#include <mutex>

#include "AppState.h"
#include "OverlayWindowManager.h"
#include "imgui.h"

#define LOG_TAG "ImGuiRuntime"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print((int)ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace ImGuiRuntime {

// ------------------------------------------------------------------
// 字体：Nunito 打底 + CJK 合并
//
// 背景：stb_truetype 处理不了 TTC 集合包（NotoSansCJK-Regular.ttc 是 5 套
// 字体的集合），ImGui::NewFrame 建图集时 null deref 崩。用户给了单体 TTF
// gkai00mp-2.ttf（AR PL KaitiM GB），这次用它。
//
// 关键点：ImGui 的 MergeMode 依赖「前一个 AddFont 返回的指针」，所以
// *必须先有基字体*。若 CJK 直接当第一份，拉丁字符取不到字形会变方块。
// ------------------------------------------------------------------
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();

    // 悬浮窗像素密度高，26px 起才够看
    const float baseSize = 26.0f;

    ImFontConfig baseConfig;
    baseConfig.SizePixels = baseSize;

    ImFont* base = nullptr;
    // 基字体优先用拉丁字体（CJK 字体里的拉丁字形偏窄且缺常规符号）
    if (!g_LatinFontPath.empty()) {
        base = io.Fonts->AddFontFromFileTTF(g_LatinFontPath.c_str(), baseSize, &baseConfig);
    }
    if (!base) base = io.Fonts->AddFontDefault();
    if (!base) {
        LOGW("LoadFonts: no base font available");
        g_BodyFont = nullptr;
        g_HeadingFont = nullptr;
        return;
    }

    ImFontConfig mergeConfig;
    mergeConfig.MergeMode = true;
    mergeConfig.SizePixels = baseSize;
    // CJK 字形密度高，oversample 给足才不会糊
    mergeConfig.OversampleH = 2;
    mergeConfig.OversampleV = 2;
    mergeConfig.PixelSnapH = true;

    bool cjkMerged = false;
    if (!g_CjkFontPath.empty()) {
        // 自定义 range：常用汉字 + 标点 + 假名 + 全角。
        // 用默认的 GetGlyphRangesChineseSimplifiedCommon() 够用，但缺全角标点，
        // 这里显式补全免得界面上出现方块。
        static ImWchar ranges[] = {
            0x0020, 0x00FF,  // 拉丁 + 常用符号
            0x2000, 0x206F,  // 通用标点
            0x3000, 0x303F,  // CJK 标点
            0x3040, 0x309F,  // 平假名
            0x30A0, 0x30FF,  // 片假名
            0x4E00, 0x9FFF,  // 常用汉字
            0xFF00, 0xFFEF,  // 全角/半角
            0,
        };
        ImFont* merged = io.Fonts->AddFontFromFileTTF(g_CjkFontPath.c_str(), baseSize,
                                                      &mergeConfig, ranges);
        cjkMerged = merged != nullptr;
        if (cjkMerged) LOGI("LoadFonts: CJK merged from %s", g_CjkFontPath.c_str());
        else LOGW("LoadFonts: failed to load CJK %s", g_CjkFontPath.c_str());
    }

    io.Fonts->Build();

    g_BodyFont = base;
    g_HeadingFont = base;
    io.FontDefault = base;

    LOGI("LoadFonts: base=%p cjkMerged=%d atlas=%dx%d glyphs=%d", (void*)base,
         cjkMerged ? 1 : 0, io.Fonts->TexWidth, io.Fonts->TexHeight, base->Glyphs.Size);
}

void BeginFrameInput() {
    // 先做拖拽：属于面板标题栏的触摸在这里被吃掉，
    // 剩下的才进 ImGui。顺序不能反，否则会点到按钮。
    {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        WindowManager::ProcessDrag(g_Touches);
    }

    std::lock_guard<std::mutex> lock(g_InputMutex);
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& ev : g_Touches) {
        io.AddMousePosEvent(ev.x, ev.y);
        if (ev.action == 0) io.AddMouseButtonEvent(0, true);
        if (ev.action == 1) io.AddMouseButtonEvent(0, false);
    }
    g_Touches.clear();
    for (const auto& ev : g_Keys) {
        if (ev.unicodeChar > 0 && ev.isDown) io.AddInputCharacter((unsigned int)ev.unicodeChar);
        if (ev.keyCode == 67) io.AddKeyEvent(ImGuiKey_Backspace, ev.isDown);
        if (ev.keyCode == 66) io.AddKeyEvent(ImGuiKey_Enter, ev.isDown);
    }
    g_Keys.clear();
}

}  // namespace ImGuiRuntime
