#include "UIComponents.h"

#include <algorithm>

#include "AppState.h"
#include "Theme.h"

namespace UI {
static const char* IconSymbol(IconId icon) {
    switch (icon) {
        case IconId::Logo: return "A";
        case IconId::Game: return "G";
        case IconId::Search: return "S";
        case IconId::Refresh: return "R";
        case IconId::Close: return "X";
        case IconId::Main: return "M";
        case IconId::Visuals: return "V";
        case IconId::Players: return "P";
        case IconId::World: return "W";
        case IconId::Misc: return "*";
        case IconId::Configs: return "C";
        case IconId::Settings: return "S";
        case IconId::Key: return "K";
        case IconId::Shield: return "#";
        case IconId::Play: return ">";
        case IconId::Status: return ".";
        case IconId::Info: return "i";
        case IconId::Warning: return "!";
        case IconId::Error: return "!";
        case IconId::Success: return "+";
        case IconId::Save: return "S";
        case IconId::Reset: return "R";
        case IconId::Back: return "<";
        case IconId::Module: return "M";
        case IconId::None:
        default: return "";
    }
}

void Icon(IconId icon, float size, ImVec4 color) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                      ImGui::GetColorU32(Theme::WithAlpha(color, 0.14f)), size * 0.30f);
    dl->AddRect(p, ImVec2(p.x + size, p.y + size),
                ImGui::GetColorU32(Theme::WithAlpha(color, 0.32f)), size * 0.30f, 0, 1.0f);
    const char* symbol = IconSymbol(icon);
    ImVec2 textSize = ImGui::CalcTextSize(symbol);
    dl->AddText(ImVec2(p.x + (size - textSize.x) * 0.5f, p.y + (size - textSize.y) * 0.5f),
                ImGui::GetColorU32(color), symbol);
    ImGui::Dummy(ImVec2(size, size));
}

void IconText(IconId icon, const char* text, ImVec4 color) {
    Icon(icon, Theme::Sizes::IconSize, color);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void DrawIconAt(ImDrawList* dl, IconId icon, ImVec2 min, float size, ImVec4 color) {
    dl->AddRectFilled(min, ImVec2(min.x + size, min.y + size),
                      ImGui::GetColorU32(Theme::WithAlpha(color, 0.14f)), size * 0.30f);
    dl->AddRect(min, ImVec2(min.x + size, min.y + size),
                ImGui::GetColorU32(Theme::WithAlpha(color, 0.32f)), size * 0.30f, 0, 1.0f);
    const char* symbol = IconSymbol(icon);
    ImVec2 textSize = ImGui::CalcTextSize(symbol);
    dl->AddText(ImVec2(min.x + (size - textSize.x) * 0.5f, min.y + (size - textSize.y) * 0.5f),
                ImGui::GetColorU32(color), symbol);
}

void IconPlaceholder(ImDrawList* dl, ImVec2 min, ImVec2 size, bool gamepad) {
    ImVec2 max(min.x + size.x, min.y + size.y);
    dl->AddRectFilled(min, max, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextPrimary, 0.06f)), 8.0f);
    dl->AddRect(min, max, ImGui::GetColorU32(Theme::Colors::Border), 8.0f, 0, 1.0f);
    if (gamepad) {
        ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        dl->AddCircle(ImVec2(c.x - 10, c.y), 7.0f,
                      ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextSecondary, 0.70f)), 16, 2.0f);
        dl->AddCircle(ImVec2(c.x + 10, c.y), 7.0f,
                      ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextSecondary, 0.70f)), 16, 2.0f);
    } else {
        dl->AddLine(min, max, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextSecondary, 0.55f)), 1.0f);
        dl->AddLine(ImVec2(max.x, min.y), ImVec2(min.x, max.y),
                    ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextSecondary, 0.55f)), 1.0f);
    }
}

void StatusDot(ImDrawList* dl, ImVec2 center, bool online) {
    ImVec4 color = online ? Theme::Colors::Success : Theme::Colors::Offline;
    dl->AddCircleFilled(center, 4.5f, ImGui::GetColorU32(color), 16);
    dl->AddCircle(center, 8.0f, ImGui::GetColorU32(Theme::WithAlpha(color, 0.18f)), 16, 1.0f);
}

bool Button(const char* label, ImVec2 size) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(pos.x - 3, pos.y - 3),
        ImVec2(pos.x + size.x + 3, pos.y + size.y + 3),
        ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.16f)), 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::AccentActive);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::LerpColor(Theme::Colors::AccentActive, Theme::Colors::Accent, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Colors::Accent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return clicked;
}

bool IconButton(const char* label, ImVec2 size) {
    return Button(label, size);
}

bool IconButton(IconId icon, const char* id) {
    ImVec2 size(Theme::Sizes::ButtonHeight, Theme::Sizes::ButtonHeight);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##icon_button", size);
    ImVec4 fill = ImGui::IsItemHovered() ? Theme::WithAlpha(Theme::Colors::Accent, 0.18f) : Theme::WithAlpha(Theme::Colors::Background, 0.18f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(fill), 9.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(Theme::Colors::Border), 9.0f, 0, 1.0f);
    DrawIconAt(dl, icon, ImVec2(pos.x + 10.0f, pos.y + 10.0f), 20.0f, Theme::Colors::TextPrimary);
    ImGui::PopID();
    return clicked;
}

bool InputText(const char* id, char* buffer, size_t bufferSize, const char* hint, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::WithAlpha(Theme::Colors::Background, 0.44f));
    ImGui::PushStyleColor(ImGuiCol_Border, Theme::Colors::Border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::SetNextItemWidth(size.x);
    bool edited = ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return edited;
}

bool SearchInput(char* buffer, size_t bufferSize, float width) {
    return InputText("##search", buffer, bufferSize, "Search games...", ImVec2(width, Theme::Sizes::GameInputHeight));
}

bool Toggle(const char* id, bool* value) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size = Theme::Sizes::SwitchSize;
    bool clicked = ImGui::InvisibleButton(id, size);
    if (clicked) *value = !*value;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = *value ? Theme::Colors::AccentActive : Theme::WithAlpha(Theme::Colors::TextSecondary, 0.22f);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bg), 13.0f);
    float knobX = *value ? pos.x + size.x - 19.0f : pos.x + 7.0f;
    dl->AddCircleFilled(ImVec2(knobX + 6.0f, pos.y + 13.0f), 8.0f, ImGui::GetColorU32(Theme::Colors::TextPrimary), 20);
    return clicked;
}

bool Slider(const char* id, float* value, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::SliderFloat(id, value, 0, 100, "%.0f%%");
}

bool Dropdown(const char* id, int* index, float width) {
    const char* options[] = {"Option 1", "Option 2", "Option 3"};
    ImGui::SetNextItemWidth(width);
    return ImGui::Combo(id, index, options, IM_ARRAYSIZE(options));
}

bool Dropdown(const char* id, int* index, const std::vector<std::string>& options, float width) {
    if (options.empty()) {
        ImGui::BeginDisabled();
        Button("No options", ImVec2(width, Theme::Sizes::ButtonHeight));
        ImGui::EndDisabled();
        return false;
    }
    std::vector<const char*> items;
    items.reserve(options.size());
    for (const auto& option : options) items.push_back(option.c_str());
    *index = std::max(0, std::min(*index, (int)items.size() - 1));
    ImGui::SetNextItemWidth(width);
    return ImGui::Combo(id, index, items.data(), (int)items.size());
}

bool Keybind(const char* label, ImVec2 size) {
    return Button(label, size);
}

bool Badge(const char* label, ImVec2 size) {
    return Button(label, size);
}

void Section(const char* title, const char* subtitle) {
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Accent);
    if (g_HeadingFont) ImGui::PushFont(g_HeadingFont);
    ImGui::TextUnformatted(title);
    if (g_HeadingFont) ImGui::PopFont();
    ImGui::PopStyleColor();
    if (subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
}

void LabelBlock(const char* label, const char* desc) {
    ImGui::TextUnformatted(label);
    if (desc) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
        ImGui::TextUnformatted(desc);
        ImGui::PopStyleColor();
    }
}

ImVec2 Card(const char* label, const char* desc, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::ChildPanel, 0.58f)), 10.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + height), ImGui::GetColorU32(Theme::Colors::Border), 10.0f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 18.0f, p.y + 12.0f));
    LabelBlock(label, desc);
    return p;
}

ImVec2 CardSized(const char* label, const char* desc, float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                      ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::ChildPanel, 0.58f)), 10.0f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(Theme::Colors::Border), 10.0f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 18.0f, p.y + 12.0f));
    LabelBlock(label, desc);
    return p;
}

bool SidebarTab(const FeatureTab& tab, int index, int* activeTab, ImVec2 min, ImVec2 max) {
    bool active = index == *activeTab;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Accent, 0.24f)), 8.0f);
        dl->AddRectFilled(min, ImVec2(min.x + 4.0f, max.y), ImGui::GetColorU32(Theme::Colors::Accent), 8.0f);
    }
    DrawIconAt(dl, tab.icon, ImVec2(min.x + 12.0f, min.y + 11.0f), Theme::Sizes::IconSize,
               active ? Theme::Colors::Accent : Theme::Colors::TextSecondary);
    dl->AddText(ImVec2(min.x + 44.0f, min.y + 12.0f),
                ImGui::GetColorU32(active ? Theme::Colors::TextPrimary : Theme::Colors::TextSecondary),
                tab.title.c_str());
    ImGui::SetCursorScreenPos(min);
    bool clicked = ImGui::InvisibleButton((std::string("##tab_") + tab.id).c_str(), ImVec2(max.x - min.x, max.y - min.y));
    if (clicked) *activeTab = index;
    return clicked;
}

void FooterStatus(const char* left, const char* right, float y, float width, bool online) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x + 42.0f, wp.y + y - 18.0f), ImVec2(wp.x + width - 42.0f, wp.y + y - 18.0f),
                ImGui::GetColorU32(Theme::Colors::Border), 1.0f);
    StatusDot(dl, ImVec2(wp.x + 52.0f, wp.y + y + 6.0f), online);
    ImGui::SetCursorPos(ImVec2(68.0f, y - 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextSecondary);
    ImGui::TextUnformatted(left);
    ImVec2 rightSize = ImGui::CalcTextSize(right);
    ImGui::SetCursorPos(ImVec2(width - rightSize.x - 42.0f, y - 4.0f));
    ImGui::TextUnformatted(right);
    ImGui::PopStyleColor();
}

bool ErrorPanel(const char* title, const char* message, const char* optionalButtonText) {
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = optionalButtonText ? 164.0f : 118.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::ChildPanel, 0.66f)), 12.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Error, 0.28f)), 12.0f, 0, 1.0f);
    DrawIconAt(dl, IconId::Error, ImVec2(p.x + 18.0f, p.y + 18.0f), 28.0f, Theme::Colors::Error);
    dl->AddText(ImVec2(p.x + 58.0f, p.y + 18.0f), ImGui::GetColorU32(Theme::Colors::TextPrimary), title);
    dl->AddText(ImVec2(p.x + 58.0f, p.y + 48.0f), ImGui::GetColorU32(Theme::Colors::TextSecondary), message);
    if (!optionalButtonText) {
        ImGui::Dummy(ImVec2(w, h));
        return false;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 58.0f, p.y + 96.0f));
    bool clicked = Button(optionalButtonText, ImVec2(std::min(190.0f, w - 76.0f), Theme::Sizes::ButtonHeight));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    return clicked;
}

bool GameRow(const GameProfile& game, float y, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 rowMin(wp.x + 28.0f, wp.y + y);
    ImVec2 rowMax(wp.x + width - 28.0f, wp.y + y + 64.0f);
    bool hovered = ImGui::IsMouseHoveringRect(rowMin, rowMax);
    dl->AddRectFilled(rowMin, rowMax,
                      ImGui::GetColorU32(hovered ? Theme::Colors::ChildPanel : Theme::WithAlpha(Theme::Colors::ChildPanel, 0.48f)), 10.0f);
    dl->AddRect(rowMin, rowMax, ImGui::GetColorU32(Theme::Colors::Border), 10.0f, 0, 1.0f);
    DrawIconAt(dl, IconId::Game, ImVec2(rowMin.x + 12.0f, rowMin.y + 10.0f), 44.0f, Theme::Colors::Accent);
    dl->AddText(ImVec2(rowMin.x + 72.0f, rowMin.y + 20.0f), ImGui::GetColorU32(Theme::Colors::TextPrimary), game.displayName.c_str());

    const char* statusText = game.online ? "ONLINE" : "OFFLINE";
    float buttonW = 42.0f;
    float buttonX = rowMax.x - buttonW - 16.0f;
    ImVec2 statusSize = ImGui::CalcTextSize(statusText);
    float statusTextX = buttonX - statusSize.x - 22.0f;
    StatusDot(dl, ImVec2(statusTextX - 14.0f, rowMin.y + 32.0f), game.online);
    dl->AddText(ImVec2(statusTextX, rowMin.y + 20.0f),
                ImGui::GetColorU32(game.online ? Theme::Colors::Success : Theme::Colors::Offline), statusText);

    ImGui::SetCursorScreenPos(ImVec2(buttonX, rowMin.y + 14.0f));
    return IconButton(IconId::Play, ("launch_" + game.gameId).c_str());
}
}
