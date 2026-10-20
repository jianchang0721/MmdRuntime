#include "ImGuiRuntime.h"

#include <cstdio>
#include <mutex>

#include "AppState.h"
#include "imgui.h"

namespace ImGuiRuntime {

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const float scale = 2.6f;

    // 只认 Java 侧拷进来的字体（assets/fonts/cjk.ttf，单体 TTF）。
    // 系统里的 NotoSansCJK-Regular.ttc 是五套字体的集合包，stb_truetype
    // 在 ImGui::NewFrame 建图集时会崩（tombstone 落在 NewFrame+908），
    // 所以系统字体一个都不探测。
    ImFont* body = nullptr;
    ImFont* heading = nullptr;
    if (!g_BodyFontPath.empty()) {
        body = io.Fonts->AddFontFromFileTTF(g_BodyFontPath.c_str(), 13.0f * scale, nullptr,
                   io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        heading = io.Fonts->AddFontFromFileTTF(g_BodyFontPath.c_str(), 16.0f * scale, nullptr,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    if (!body) {
        body = io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Regular.ttf", 13.0f * scale);
    }
    if (!body) body = io.Fonts->AddFontDefault();
    if (!heading) heading = body;

    g_BodyFont = body;
    g_HeadingFont = heading;
    io.FontDefault = g_BodyFont;
}

void BeginFrameInput() {
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
