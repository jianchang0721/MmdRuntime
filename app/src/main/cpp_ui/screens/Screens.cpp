#include "Screens.h"

#include "AppState.h"
#include "DynamicMenuRenderer.h"
#include "Modules.h"
#include "OverlayWindowManager.h"
#include "Theme.h"
#include "ToastManager.h"
#include "UIComponents.h"
#include "imgui.h"

namespace {
DynamicMenuRenderer g_DynamicMenuRenderer;
}

namespace Screens {
void DrawLoginScreen() {
    WindowManager::BeginWindow("##login_window");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float h = ImGui::GetWindowHeight();

    ImVec2 logoMin(w * 0.5f - 44.0f, 72.0f);
    UI::DrawIconAt(dl, IconId::Logo, ImVec2(wp.x + logoMin.x, wp.y + logoMin.y), 88.0f, Theme::Colors::Accent);
    dl->AddTriangleFilled(ImVec2(wp.x + w * 0.5f, wp.y + logoMin.y + 18.0f),
                          ImVec2(wp.x + logoMin.x + 18.0f, wp.y + logoMin.y + 70.0f),
                          ImVec2(wp.x + logoMin.x + 70.0f, wp.y + logoMin.y + 70.0f),
                          ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.78f)));
    ImGui::SetCursorPos(ImVec2(0, 176));
    if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
    ImGui::SetCursorPosX((w - ImGui::CalcTextSize("AURORA").x) * 0.5f);
    ImGui::TextUnformatted("AURORA");
    if (g_HeadingFont) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Accent);
    ImGui::SetCursorPosX((w - ImGui::CalcTextSize("EXECUTIVE").x) * 0.5f);
    ImGui::TextUnformatted("EXECUTIVE");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(42, 270));
    UI::LabelBlock("Enter License Key", "Your key is required to continue");
    ImGui::SetCursorPos(ImVec2(42, 342));
    UI::InputText("##license", g_State.licenseKey, sizeof(g_State.licenseKey), "License Key", ImVec2(w - 84.0f, 44.0f));
    ImGui::SetCursorPos(ImVec2(42, 412));
    if (UI::Button("AUTHENTICATE", ImVec2(w - 84.0f, 52.0f))) {
        WindowManager::GoTo(Screen::Games);
    }
    UI::FooterStatus("Secure Connection", "v1.0.0", h - 64.0f, w, true);
    WindowManager::EndWindow();
}

void DrawGamesScreen() {
    WindowManager::BeginWindow("##games_window", "GAMES");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    dl->AddCircleFilled(ImVec2(wp.x + 30.0f, wp.y + 34.0f), 10.0f, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.80f)), 20);

    ImGui::SetCursorPos(ImVec2(w - 86.0f, 24.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::WithAlpha(Theme::Colors::Accent, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::WithAlpha(Theme::Colors::Accent, 0.24f));
    if (UI::IconButton(IconId::Refresh, "refresh_games")) Toast::Info("Game list refreshed");
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPos(ImVec2(28.0f, 84.0f));
    UI::SearchInput(g_State.searchText, sizeof(g_State.searchText), w - 56.0f);

    float y = 154.0f;
    for (const GameProfile& game : TemplateData::Games()) {
        if (UI::GameRow(game, y, w)) {
            if (g_ModuleManager.LoadModuleForGame(game.gameId)) {
                g_GameStatusMessage = "Loaded " + game.displayName;
                Toast::Success("Module loaded successfully");
                WindowManager::GoTo(Screen::Menu);
            } else {
                g_GameStatusMessage = g_ModuleManager.GetLastError();
                if (g_GameStatusMessage == "Game offline") Toast::Warning("Game offline");
                else Toast::Error(g_GameStatusMessage.empty() ? "Module load failed" : g_GameStatusMessage);
            }
        }
        y += 76.0f;
    }
    ImGui::SetCursorPos(ImVec2(28.0f, ImGui::GetWindowHeight() - 54.0f));
    UI::IconButton(IconId::Info, "games_status");
    if (!g_GameStatusMessage.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted(g_GameStatusMessage.c_str());
        ImGui::PopStyleColor();
    }
    WindowManager::EndWindow();
}

void DrawMenuScreen() {
    WindowManager::BeginWindow("##mmd_window", "MMD RUNTIME");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float h = ImGui::GetWindowHeight();

    // 顶部标题
    dl->AddCircleFilled(ImVec2(wp.x + 30.0f, wp.y + 34.0f), 10.0f,
                        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.80f)), 20);
    ImGui::SetCursorPos(ImVec2(50.0f, 22.0f));
    if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
    ImGui::TextUnformatted("MMD RUNTIME");
    if (g_HeadingFont) ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(50.0f, 50.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted("模型 · 骨骼 · 动作");
    ImGui::PopStyleColor();

    // 状态卡
    ImGui::SetCursorPos(ImVec2(24.0f, 90.0f));
    ImGui::BeginChild("##status_card", ImVec2(w - 48.0f, 120.0f), false);
    ImGui::SetCursorPos(ImVec2(16.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Success);
    ImGui::TextUnformatted("运行状态");
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(16.0f, 40.0f));
    ImGui::TextWrapped("%s", g_GameStatusMessage.empty() ? "等待游戏连接…" : g_GameStatusMessage.c_str());
    ImGui::SetCursorPos(ImVec2(16.0f, 74.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted("PMX 解析 · il2cpp 绑定 · SMR 劫持");
    ImGui::PopStyleColor();
    ImGui::EndChild();

    // 功能开关区
    ImGui::SetCursorPos(ImVec2(24.0f, 224.0f));
    UI::LabelBlock("模型替换", "把角色的网格换成 MMD 模型");

    ImGui::SetCursorPos(ImVec2(24.0f, 280.0f));
    static bool enableReplace = false;
    if (UI::Toggle("##enable_replace", &enableReplace)) {
        Toast::Info(enableReplace ? "替换已开启" : "替换已关闭");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("启用模型替换");

    ImGui::SetCursorPos(ImVec2(24.0f, 330.0f));
    static float modelScale = 1.0f;
    ImGui::SetNextItemWidth(w - 120.0f);
    if (ImGui::SliderFloat("##scale", &modelScale, 0.1f, 3.0f, "缩放 %.2f")) { }
    ImGui::SameLine();
    ImGui::TextUnformatted("缩放");

    // 按钮行
    ImGui::SetCursorPos(ImVec2(24.0f, 392.0f));
    if (UI::Button("重新扫描", ImVec2((w - 60.0f) * 0.5f, 44.0f))) {
        Toast::Info("正在重新扫描 SMR…");
    }
    ImGui::SameLine();
    if (UI::Button("导入模型", ImVec2((w - 60.0f) * 0.5f, 44.0f))) {
        Toast::Success("用桌面 MmdRuntime 图标导入");
    }

    // 关闭按钮 + 底部状态
    ImGui::SetCursorPos(ImVec2(w - 46.0f, 18.0f));
    if (UI::IconButton(IconId::Close, "##close")) {
        WindowManager::RequestClose();
    }
    UI::FooterStatus("悬浮菜单 · 不碰游戏进程", "v1.0", h - 40.0f, w, true);

    WindowManager::EndWindow();
}
}
