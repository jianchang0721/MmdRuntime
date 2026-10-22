// =====================================================================
// Screens.cpp —— MmdRuntime 悬浮面板（完全重写）
//
// 旧版用 ImGui::SetCursorPos(ImVec2(24, 90)) 钉死坐标：
//   · 窗口尺寸一变，元素全飞出/重叠
//   · 中英文字宽不同，一行文字直接溢出右边
//   · 增删一个控件，下面的 y 全部要手改
//
// 新版规则：
//   1. 只用 CursorScreenPos / ContentRegionAvail / SameLine 做布局
//   2. 宽度一律 SetNextItemWidth(-1)（吃满剩余）
//   3. 每个 Tab 一个可滚动 Child，内容超高就滚，不裁也不挤
// =====================================================================
#include "Screens.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "AppState.h"
#include "Bridge.h"
#include "Modules.h"
#include "OverlayWindowManager.h"
#include "Theme.h"
#include "ToastManager.h"
#include "UIComponents.h"
#include "imgui.h"

namespace {

using Bridge::Shared;

// 摄像机面板的本地状态
bool  g_camBound    = false;
float g_camLastDist = 4.0f;

inline void StatusDot(ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted("●");
    ImGui::PopStyleColor();
}

inline void BeginSectionTitle(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextPrimary);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

}  // namespace

namespace Screens {

void DrawLoginScreen() {
    WindowManager::BeginWindow("##login_window");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();

    ImGui::SetCursorPosY(Theme::Sizes::TitleBarHeight + 24.0f);
    UI::DrawIconAt(dl, IconId::Logo, ImVec2(wp.x + (w - 88.0f) * 0.5f, wp.y + ImGui::GetCursorPosY()),
                   88.0f, Theme::Colors::Accent);
    ImGui::Dummy(ImVec2(88.0f, 88.0f));

    ImGui::Spacing();
    if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
    ImGui::SetCursorPosX((w - ImGui::CalcTextSize("AURORA").x) * 0.5f);
    ImGui::TextUnformatted("AURORA");
    if (g_HeadingFont) ImGui::PopFont();

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Accent);
    ImGui::SetCursorPosX((w - ImGui::CalcTextSize("EXECUTIVE").x) * 0.5f);
    ImGui::TextUnformatted("EXECUTIVE");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.52f);
    ImGui::SetCursorPosX(42.0f);
    UI::LabelBlock("Enter License Key", "Your key is required to continue");
    ImGui::SetCursorPosX(42.0f);
    UI::InputText("##license", g_State.licenseKey, sizeof(g_State.licenseKey), "License Key",
                  ImVec2(w - 84.0f, 44.0f));

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 124.0f);
    ImGui::SetCursorPosX(42.0f);
    if (UI::Button("AUTHENTICATE", ImVec2(w - 84.0f, 52.0f))) {
        WindowManager::GoTo(Screen::Games);
    }
    UI::FooterStatus("Secure Connection", "v1.0.0", ImGui::GetWindowHeight() - 64.0f, w, true);
    WindowManager::EndWindow();
}

void DrawGamesScreen() {
    WindowManager::BeginWindow("##games_window", "GAMES");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    dl->AddCircleFilled(ImVec2(wp.x + 30.0f, wp.y + 34.0f), 10.0f,
                        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.80f)), 20);

    ImGui::SetCursorPos(ImVec2(w - 86.0f, 24.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::WithAlpha(Theme::Colors::Accent, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::WithAlpha(Theme::Colors::Accent, 0.24f));
    if (UI::IconButton(IconId::Refresh, "refresh_games")) Toast::Info("Game list refreshed");
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPos(ImVec2(28.0f, 84.0f));
    UI::SearchInput(g_State.searchText, sizeof(g_State.searchText), w - 56.0f);

    ImGui::SetCursorPosY(154.0f);
    for (const GameProfile& game : TemplateData::Games()) {
        ImGui::SetCursorPosX(28.0f);
        if (UI::GameRow(game, ImGui::GetCursorPosY(), w)) {
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
        ImGui::Spacing();
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

// =====================================================================
// 折叠横栏：可拖动（Kotlin 拖拽区整条都是）、点击展开、右侧 X 真正退出
// =====================================================================
void DrawCollapsedBar(Bridge::BridgeModel& Model) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float h = ImGui::GetWindowHeight();

    // 胶囊底
    dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                      ImGui::GetColorU32(Theme::Colors::Panel), Theme::Sizes::WindowRounding);
    dl->AddRect(wp, ImVec2(wp.x + w, wp.y + h),
                ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.5f)),
                Theme::Sizes::WindowRounding, 0, 1.4f);

    // 左侧状态点：connected=绿，否则=黄
    const bool connected = Model.state == Bridge::GameState::Attached ||
                           Model.state == Bridge::GameState::Active;
    dl->AddCircleFilled(ImVec2(wp.x + 30.0f, wp.y + h * 0.5f), 7.0f,
                        ImGui::GetColorU32(connected ? Theme::Colors::Success
                                                     : Theme::Colors::Warning), 24);

    // 标题
    ImGui::SetCursorPos(ImVec2(48.0f, (h - ImGui::GetTextLineHeight()) * 0.5f));
    if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
    ImGui::TextUnformatted("MMD RUNTIME");
    if (g_HeadingFont) ImGui::PopFont();

    // 中间状态文字（折叠时也显示游戏连接状态）
    ImGui::SetCursorPos(ImVec2(210.0f, (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted(Bridge::GameStateName(Model.state));
    ImGui::PopStyleColor();

    // 右侧：真正退出按钮（小 X）
    const float exitSize = 30.0f;
    ImVec2 exitPos(wp.x + w - exitSize - 14.0f, wp.y + (h - exitSize) * 0.5f);
    ImGui::SetCursorScreenPos(exitPos);
    bool exitClicked = ImGui::InvisibleButton("##bar_exit", ImVec2(exitSize, exitSize));
    ImU32 exitColor = ImGui::GetColorU32(
        ImGui::IsItemHovered() ? Theme::Colors::Error : Theme::Colors::TextSecondary);
    dl->AddLine(ImVec2(exitPos.x + 8, exitPos.y + 8), ImVec2(exitPos.x + 22, exitPos.y + 22),
                exitColor, 1.8f);
    dl->AddLine(ImVec2(exitPos.x + 22, exitPos.y + 8), ImVec2(exitPos.x + 8, exitPos.y + 22),
                exitColor, 1.8f);
    if (exitClicked) WindowManager::RequestClose();

    // 剩余区域整个可点 = 展开。留出 X 的位置，其余全算展开热区。
    ImVec2 expandMin(wp.x + 8.0f, wp.y + 6.0f);
    ImVec2 expandMax(wp.x + w - exitSize - 22.0f, wp.y + h - 6.0f);
    ImGui::SetCursorScreenPos(expandMin);
    bool expandClicked = ImGui::InvisibleButton("##bar_expand",
                                                ImVec2(expandMax.x - expandMin.x,
                                                       expandMax.y - expandMin.y));
    if (expandClicked) WindowManager::RequestExpand();
}

// =====================================================================
// 主面板
// =====================================================================
void DrawMenuScreen() {
    WindowManager::BeginWindow("##mmd_window", "MMD RUNTIME");

    Bridge::PollStatus();
    Bridge::BridgeModel& Model = Shared();

    if (WindowManager::collapsed) {
        DrawCollapsedBar(Model);
        WindowManager::EndWindow();
        return;
    }

    if (ImGui::BeginTabBar("##Tabs")) {
        // 每个 Tab 包一层 BeginChild：
        //   · 高度 0 = 吃满 Tab 剩下的空间，内容超高时出现滚动条
        //   · 之前只写了注释没真包，内容一超出面板高度就被裁掉，
        //     手机上手划也滑不到（设置页里那个「自由视角」就是这么看不见的）
        #define MMD_TAB_BEGIN(idstr) \
            if (ImGui::BeginTabItem(idstr)) { \
                if (ImGui::BeginChild("##scroll_" idstr, ImVec2(0, 0), false)) {
        #define MMD_TAB_END \
                } \
                ImGui::EndChild(); \
                ImGui::EndTabItem(); \
            }

        MMD_TAB_BEGIN("概览")
            DrawOverviewTab(Model);
        MMD_TAB_END

        MMD_TAB_BEGIN("渲染")
            DrawRenderTab(Model);
        MMD_TAB_END

        MMD_TAB_BEGIN("骨骼")
            DrawBoneTab(Model);
        MMD_TAB_END

        MMD_TAB_BEGIN("诊断")
            DrawDiagnosticTab(Model);
        MMD_TAB_END

        MMD_TAB_BEGIN("设置")
            DrawSettingsTab(Model);
        MMD_TAB_END

        #undef MMD_TAB_BEGIN
        #undef MMD_TAB_END

        ImGui::EndTabBar();
    }

    WindowManager::EndWindow();
}

// ------------------------------------------------------------------
// 概览
// ------------------------------------------------------------------
void DrawOverviewTab(Bridge::BridgeModel& Model) {
    BeginSectionTitle("游戏状态");
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("Unity");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::TextUnformatted(Model.gameVersion.c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("SkinnedMeshRenderer");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::Text("%d / %d", Model.smrReplaced, Model.smrTotal);

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("替换进度");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        if (Model.replaceProgress > 0.0f) ImGui::ProgressBar(Model.replaceProgress, ImVec2(-1.0f, 0.0f));
        else ImGui::TextUnformatted("--");
    }

    ImGui::Spacing();
    BeginSectionTitle("当前模型");
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("名称");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::TextUnformatted(Model.activeModel.c_str());

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("预算");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::Text("%d verts / %d bones", Model.vertexCount, Model.boneCount);

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("路径");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextWrapped("%s", Model.modelPath.empty() ? "（未选择）" : Model.modelPath.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 手动两步：① 初始化（绑 il2cpp + 找类 + 扫 SMR）② 加载模型。
    // 进游戏默认睡死什么都不做——build-121 的自动加载把游戏画面搞卡过。
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (UI::Button("① 初始化", ImVec2(half, 0.0f))) {
        Bridge::RequestInit();
        Toast::Info("已请求初始化：绑 il2cpp + 扫 SMR");
    }
    ImGui::SameLine();
    if (UI::Button("② 加载模型", ImVec2(half, 0.0f))) {
        Bridge::RequestReapply();
        Toast::Info("已请求加载模型");
    }

    ImGui::Spacing();
    if (UI::Button("重扫 SMR", ImVec2(-1.0f, 0.0f))) {
        Bridge::RequestRescan();
        Toast::Info("已请求重新扫描 SMR");
    }
}

// ------------------------------------------------------------------
// 渲染
// ------------------------------------------------------------------
void DrawRenderTab(Bridge::BridgeModel& Model) {
    BeginSectionTitle("渲染设置");

    bool enable = Model.enableReplace;
    if (ImGui::Checkbox("启用模型替换", &enable)) {
        Model.enableReplace = enable;
        Bridge::SetEnableReplace(enable);
        Toast::Info(enable ? "替换已开启" : "替换已关闭");
    }

    bool bounds = Model.showBounds;
    if (ImGui::Checkbox("显示骨骼辅助线", &bounds)) {
        Model.showBounds = bounds;
        Bridge::SetShowBounds(bounds);
    }

    ImGui::Spacing();

    ImGui::TextUnformatted("模型缩放");
    float scale = Model.modelScale;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##scale", &scale, 0.01f, 3.0f, "%.2fx")) {
        Model.modelScale = scale;
        Bridge::SetModelScale(scale);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("模型列表");
    const std::vector<std::string>& models = Model.models;
    if (models.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextWrapped("没有找到模型。用法：设置 → 更改目录，把 PMX 拷进该目录后点「重新导入」。");
        ImGui::PopStyleColor();
    } else {
        if (Model.selectedModel >= (int)models.size()) Model.selectedModel = 0;
        for (int i = 0; i < (int)models.size(); ++i) {
            if (ImGui::Selectable(models[i].c_str(), Model.selectedModel == i)) {
                Bridge::SelectModel(models[i]);
                Toast::Success(("已选 " + models[i]).c_str());
            }
        }
    }
}

// ------------------------------------------------------------------
// 骨骼
// ------------------------------------------------------------------
void DrawBoneTab(Bridge::BridgeModel& Model) {
    BeginSectionTitle("骨骼层级");

    if (Model.boneCount <= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextWrapped("游戏侧还没上报骨骼数据（bridge 未连通，或还没完成一次替换）。");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextWrapped("当前模型 %d 根骨骼，已重映射到玩家 %d 个槽位。", Model.boneCount,
                           Model.smrReplaced > 0 ? Model.smrReplaced : Model.smrTotal);
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // 平铺 list 而不是假树：真实树要游戏侧把层级结构一并发过来，
        // 现在 bridge 协议里没有 father 字段，不造假数据。
        ImGui::Text("bone[0..%d]", Model.boneCount - 1);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("手动偏移");
    static float x = 0.0f, y = 0.0f, z = 0.0f;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("X", &x, -2.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Y", &y, -2.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Z", &z, -2.0f, 2.0f, "%.2f");
    if (ImGui::Button("应用偏移", ImVec2(-1.0f, 0.0f))) {
        Bridge::SetManualOffset(x, y, z);
        Toast::Success("偏移已写入配置");
    }
}

// ------------------------------------------------------------------
// 诊断
// ------------------------------------------------------------------
void DrawDiagnosticTab(Bridge::BridgeModel& Model) {
    BeginSectionTitle("Bridge 状态");

    const bool connected = Model.state == Bridge::GameState::Attached ||
                           Model.state == Bridge::GameState::Active;
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted("连接");
    ImGui::PopStyleColor();
    ImGui::SameLine(156.0f);
    StatusDot(connected ? Theme::Colors::Success : Theme::Colors::Warning);
    ImGui::SameLine();
    ImGui::TextUnformatted(Bridge::GameStateName(Model.state));

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted("最后一次结果");
    ImGui::PopStyleColor();
    ImGui::SameLine(156.0f);
    ImGui::Text("%d (%s)", Model.lastResult, Model.lastResult ? "OK" : "FAIL");

    if (Model.lastErrorCode != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted("错误码");
        ImGui::PopStyleColor();
        ImGui::SameLine(156.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Error);
        ImGui::Text("%d %s", Model.lastErrorCode, Model.lastErrorMessage.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted("消息");
    ImGui::PopStyleColor();
    ImGui::SameLine(156.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextWrapped("%s", Model.statusMessage.c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("模块加载状态");
    const FeatureManifest* manifest = g_ModuleManager.GetActiveManifest();
    if (!manifest) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextWrapped("没有活动模块。");
        ImGui::PopStyleColor();
    } else {
        ImGui::Text("%s v%s", manifest->moduleName.c_str(), manifest->version.c_str());
        ImGui::Text("tabs=%d", (int)manifest->tabs.size());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("连不上游戏时自查");
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextWrapped(
        "1. LSPosed 里勾选 MmdRuntime\n"
        "2. 作用域勾选 com.hermes.h1game\n"
        "3. 杀掉游戏重进\n"
        "4. 仍然连不上：面板上方写着「游戏侧 so 还没加载」，\n"
        "   说明 libmmdruntime.so 没起来（壳没加载或不在 games/files 下）\n"
        "5. 日志（需要 root 才能看）：%s",
        "/data/data/com.hermes.h1game/files/mmdruntime.log");
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------
// 设置
// ------------------------------------------------------------------
void DrawSettingsTab(Bridge::BridgeModel& Model) {
    BeginSectionTitle("模型目录");

    // InputText 需要固定可写缓冲；直接用 std::string::data() 会因重分配失效
    static char dirBuffer[512] = "";
    static bool dirPrimed = false;
    if (!dirPrimed) {
        snprintf(dirBuffer, sizeof(dirBuffer), "%s", Model.storageDir.c_str());
        dirPrimed = true;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##storage", dirBuffer, sizeof(dirBuffer))) {
        Model.storageDir = dirBuffer;
    }
    if (UI::Button("更改目录", ImVec2(-1.0f, 0.0f))) {
        Bridge::SetStorageDir(Model.storageDir);
        Toast::Success("已切换到该目录");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("主题");
    static const char* names[] = {"Default Purple", "Blue", "Red", "Green", "Monochrome"};
    int preset = (int)Theme::GetPreset();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##theme", &preset, names, 5)) Theme::SetPreset((ThemePreset)preset);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("操作");
    if (UI::Button("重新导入模型列表", ImVec2(-1.0f, 0.0f))) {
        Bridge::RefreshModelList();
        Toast::Info("已刷新模型列表");
    }
    ImGui::Spacing();
    if (UI::Button("重载 UI 配置", ImVec2(-1.0f, 0.0f))) {
        Bridge::ImportModelsFromConfig();
        Toast::Info("已重载");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    BeginSectionTitle("自由视角");
    static bool  camOn      = false;
    static float camDist    = 4.0f;
    static float camFov     = 55.0f;

    if (!g_camBound) {
        g_camBound = true;
        camOn = Bridge::CameraEnabled() == 1;
        g_camLastDist = camDist;
    }

    if (UI::Toggle("启用自由视角", &camOn)) {
        const int r = Bridge::CameraEnable(camOn ? 1 : 0);
        camOn = (r == 1);
        if (r != 1) Toast::Error("摄像机开启失败，看日志 [CAM]");
        else Toast::Success(camOn ? "自由视角已开" : "自由视角已关");
    }
    ImGui::Spacing();

    if (camOn) {
        ImGui::TextDisabled("左半屏拖动 = 平移，右半屏拖动 = 旋转");
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##camdist", &camDist, 0.5f, 30.0f, "距离 %.1f")) {
            Bridge::CameraZoom(camDist - g_camLastDist);
            g_camLastDist = camDist;
        }
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##camfov", &camFov, 20.0f, 110.0f, "视野 %.0f 度");
        ImGui::Spacing();

        if (UI::Button("复位视角", ImVec2(-1.0f, 0.0f))) {
            Bridge::CameraReset();
            camDist = 4.0f;
            g_camLastDist = 4.0f;
            Toast::Info("视角已复位");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextWrapped("连接方式：同进程直调 mmd_get_api()（无文件、无 root）\n"
                       "状态：%s",
                       Bridge::Connected() ? "已直连游戏侧 so" : "未连上（等游戏侧 so 加载）");
    ImGui::PopStyleColor();
}

}  // namespace Screens
