#include "OverlayWindowManager.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
bool collapsed = false;

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
    if (collapsed) {
        // 横栏：高度固定 56，宽度按屏宽收缩
        return ImVec2(std::min(520.0f, (float)std::max(240, screenW - 48)), 56.0f);
    }
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

void RequestCollapse() { collapsed = true; }

void RequestExpand() { collapsed = false; }
// =====================================================================
// 面板拖拽
//
// 面板画在游戏的 GL 帧上，没有 Android 窗口可以依附，位置全由自己维护。
// 规则：
//   · 只有按在标题栏那一条上才拖（高度 = Theme::Sizes::TitleBarHeight）
//   · 拖动阈值 8px，避免误触
//   · 位置夹在屏幕内，不允许拖出可视区
//   · 拖动期间的事件不喂给 ImGui，否则会点到按钮
// =====================================================================
ImVec2 panelPos = ImVec2(0.0f, 0.0f);

namespace {

bool  g_dragging  = false;
float g_dragOffX  = 0.0f;
float g_dragOffY  = 0.0f;
float g_downX     = 0.0f;
float g_downY     = 0.0f;
bool  g_downValid = false;
const float kDragThreshold = 8.0f;

float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

bool IsDragging() { return g_dragging; }

void ProcessDrag(std::vector<TouchInputEvent>& events) {
    if (events.empty()) return;

    ImVec2 size = SizeFor(currentScreen);
    const float titleH = Theme::Sizes::TitleBarHeight;

    std::vector<TouchInputEvent> keep;
    keep.reserve(events.size());

    for (const TouchInputEvent& ev : events) {
        if (ev.action == 0) {          // DOWN
            // 只有按在标题栏上才算拖面板，但要排除右上角关闭按钮那 30x30
            // （不排除的话按钮永远收不到 DOWN，点了没反应）
            const bool inTitle =
                ev.x >= panelPos.x && ev.x <= panelPos.x + size.x &&
                ev.y >= panelPos.y && ev.y <= panelPos.y + titleH;
            const bool onClose =
                ev.x >= panelPos.x + size.x - 48.0f &&
                ev.y <= panelPos.y + 48.0f;
            if (inTitle && !onClose && !collapsed) {
                g_downX = ev.x;
                g_downY = ev.y;
                g_downValid = true;
                continue;               // 吃掉，不喂 ImGui
            }
            keep.push_back(ev);
            continue;
        }

        if (ev.action == 2) {          // MOVE
            if (g_downValid) {
                const float dx = ev.x - g_downX;
                const float dy = ev.y - g_downY;
                if (!g_dragging && (std::fabs(dx) > kDragThreshold ||
                                    std::fabs(dy) > kDragThreshold)) {
                    g_dragging = true;
                    g_dragOffX = panelPos.x - g_downX;
                    g_dragOffY = panelPos.y - g_downY;
                }
                if (g_dragging) {
                    float nx = ev.x + g_dragOffX;
                    float ny = ev.y + g_dragOffY;
                    // 夹在屏幕内，留 24px 让边缘还能抓住
                    nx = Clampf(nx, -size.x + 24.0f, (float)screenW - 24.0f);
                    ny = Clampf(ny, 0.0f, (float)screenH - titleH);
                    panelPos = ImVec2(nx, ny);
                }
                continue;               // 拖动期间全吃掉
            }
            keep.push_back(ev);
            continue;
        }

        if (ev.action == 1 || ev.action == 3) {   // UP / CANCEL
            if (g_downValid) {
                // 不管有没有真拖过，这次触摸都归面板，别让游戏收到
                g_downValid = false;
                g_dragging = false;
                continue;
            }
            keep.push_back(ev);
            continue;
        }

        keep.push_back(ev);
    }

    events.swap(keep);
}

void BeginWindow(const char* id, const char* title) {
    ImVec2 size = SizeFor(currentScreen);
    // 位置由 panelPos 决定（可拖），不再钉死在 (0,0)
    ImGui::SetNextWindowPos(panelPos, ImGuiCond_Always);
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

    if (collapsed) {
        // 折叠态：不画标题和关闭按钮，整个内容区交给横栏 UI
        return;
    }

    ImVec2 closePos(max.x - 42.0f, min.y + 16.0f);
    ImGui::SetCursorScreenPos(closePos);
    bool closeClicked = ImGui::InvisibleButton("##close", ImVec2(30.0f, 30.0f));
    ImU32 closeColor = ImGui::GetColorU32(ImGui::IsItemHovered() ? Theme::Colors::TextPrimary : Theme::Colors::TextSecondary);
    dl->AddLine(ImVec2(closePos.x + 9, closePos.y + 9), ImVec2(closePos.x + 21, closePos.y + 21), closeColor, 1.6f);
    dl->AddLine(ImVec2(closePos.x + 21, closePos.y + 9), ImVec2(closePos.x + 9, closePos.y + 21), closeColor, 1.6f);
    // 主面板的 X = 折叠成横栏，不是退出。横栏上另有真正的退出按钮。
    if (closeClicked) RequestCollapse();

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
