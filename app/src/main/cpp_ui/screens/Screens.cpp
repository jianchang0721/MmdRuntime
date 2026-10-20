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
    WindowManager::BeginWindow("##main_window");
    if (g_ModuleManager.IsModuleLoaded()) {
        const FeatureManifest* manifest = g_ModuleManager.GetActiveManifest();
        g_DynamicMenuRenderer.Render(*manifest, g_ModuleState, g_ModuleManager.GetActiveModule());
    } else {
        ImGui::SetCursorPos(ImVec2(42.0f, 96.0f));
        if (UI::ErrorPanel("No Module Loaded", "Return to Games and select an online game.", "Return to Games")) {
            WindowManager::GoTo(Screen::Games);
        }
    }
    WindowManager::EndWindow();
}
}
