#include "OverlayWindowManager.h"

#include <algorithm>

#include "AppState.h"
#include "Theme.h"

namespace WindowManager {
Screen currentScreen = Screen::Login;
Screen previousScreen = Screen::Login;
int screenW = 1080;
int screenH = 2400;
ImVec2 lastWindowPosition = ImVec2(0, 0);
ImVec2 lastWindowSize = ImVec2(0, 0);
bool showOverlay = true;
bool isDragging = false;

static float ResponsiveWindowWidth(float percent, float minValue, float maxValue) {
    float available = std::max(320.0f, (float)screenW - Theme::Spacing::WindowSafeMargin);
    float desired = ClampFloat((float)screenW * percent, minValue, maxValue);
    return std::min(desired, available);
}

static float ResponsiveWindowHeight(float percent, float minValue, float maxValue) {
    float available = std::max(420.0f, (float)screenH - 48.0f);
    float desired = ClampFloat((float)screenH * percent, minValue, maxValue);
    return std::min(desired, available);
}

ImVec2 SizeFor(Screen screen) {
    if (screen == Screen::Login) {
        return ImVec2(ResponsiveWindowWidth(0.86f, 430.0f, 590.0f),
                      ResponsiveWindowHeight(0.44f, 560.0f, 720.0f));
    }
    if (screen == Screen::Games) {
        return ImVec2(ResponsiveWindowWidth(0.90f, 500.0f, 700.0f),
                      ResponsiveWindowHeight(0.48f, 610.0f, 800.0f));
    }
    return ImVec2(ResponsiveWindowWidth(0.96f, 760.0f, 1080.0f),
                  ResponsiveWindowHeight(0.50f, 720.0f, 900.0f));
}

void SetScreenSize(int width, int height) {
    screenW = std::max(1, width);
    screenH = std::max(1, height);
}

void GoTo(Screen next) {
    previousScreen = currentScreen;
    currentScreen = next;
}

void RequestClose() {
    showOverlay = false;
    g_CloseRequested = true;
}

void BeginWindow(const char* id, const char* title) {
    ImVec2 size = SizeFor(currentScreen);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    lastWindowPosition = ImGui::GetWindowPos();
    lastWindowSize = ImGui::GetWindowSize();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetWindowPos();
    ImVec2 max(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(min, max, ImGui::GetColorU32(Theme::Colors::Background), Theme::Sizes::WindowRounding);
    dl->AddRectFilled(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1),
                      ImGui::GetColorU32(Theme::Colors::Panel), Theme::Sizes::WindowRounding);
    dl->AddRectFilledMultiColor(
        min, ImVec2(max.x, min.y + Theme::Sizes::TitleBarHeight),
        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.16f)),
        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Panel, 0.0f)),
        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Panel, 0.0f)),
        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.16f)));

    for (int i = 4; i >= 1; --i) {
        float spread = i * 3.0f;
        dl->AddRect(ImVec2(min.x - spread, min.y - spread),
                    ImVec2(max.x + spread, max.y + spread),
                    ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.035f * i)),
                    Theme::Sizes::WindowRounding + spread, 0, 1.0f);
    }
    dl->AddRect(min, max, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.36f)),
                Theme::Sizes::WindowRounding, 0, 1.0f);
    dl->AddRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1),
                ImGui::GetColorU32(Theme::Colors::Border), Theme::Sizes::WindowRounding - 1.0f, 0, 1.0f);

    float cx = (min.x + max.x) * 0.5f;
    dl->AddRectFilled(ImVec2(cx - 18.0f, min.y + 15.0f), ImVec2(cx + 18.0f, min.y + 19.0f),
                      ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextSecondary, 0.55f)), 3.0f);

    ImVec2 closePos(max.x - 42.0f, min.y + 16.0f);
    ImGui::SetCursorScreenPos(closePos);
    bool closeClicked = ImGui::InvisibleButton("##close", ImVec2(30.0f, 30.0f));
    ImU32 closeColor = ImGui::GetColorU32(ImGui::IsItemHovered() ? Theme::Colors::TextPrimary : Theme::Colors::TextSecondary);
    dl->AddLine(ImVec2(closePos.x + 9, closePos.y + 9), ImVec2(closePos.x + 21, closePos.y + 21), closeColor, 1.6f);
    dl->AddLine(ImVec2(closePos.x + 21, closePos.y + 9), ImVec2(closePos.x + 9, closePos.y + 21), closeColor, 1.6f);
    if (closeClicked) RequestClose();

    if (title && title[0]) {
        ImGui::SetCursorPos(ImVec2(56.0f, 34.0f));
        if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
        ImGui::TextUnformatted(title);
        if (g_HeadingFont) ImGui::PopFont();
    }
}

void EndWindow() {
    ImGui::End();
}
}
