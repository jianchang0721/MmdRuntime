#include "ImGuiRuntime.h"

#include <mutex>

#include "AppState.h"
#include "imgui.h"

namespace ImGuiRuntime {
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const float scale = 2.6f;
    if (!g_BodyFontPath.empty()) g_BodyFont = io.Fonts->AddFontFromFileTTF(g_BodyFontPath.c_str(), 13.0f * scale);
    if (!g_HeadingFontPath.empty()) g_HeadingFont = io.Fonts->AddFontFromFileTTF(g_HeadingFontPath.c_str(), 16.0f * scale);
    if (!g_BodyFont) g_BodyFont = io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Regular.ttf", 13.0f * scale);
    if (!g_HeadingFont) g_HeadingFont = io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Bold.ttf", 16.0f * scale);
    if (!g_BodyFont) g_BodyFont = io.Fonts->AddFontDefault();
    if (!g_HeadingFont) g_HeadingFont = g_BodyFont;
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
}
