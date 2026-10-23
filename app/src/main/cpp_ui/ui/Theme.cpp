#include "Theme.h"

namespace Theme {
namespace Colors {
ImVec4 Background    = ImVec4(0x09 / 255.0f, 0x0B / 255.0f, 0x14 / 255.0f, 0.96f);
ImVec4 Panel         = ImVec4(0x10 / 255.0f, 0x13 / 255.0f, 0x1D / 255.0f, 0.88f);
ImVec4 ChildPanel    = ImVec4(0x17 / 255.0f, 0x1B / 255.0f, 0x29 / 255.0f, 0.92f);
ImVec4 Border        = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
ImVec4 Accent        = ImVec4(0x7C / 255.0f, 0x4D / 255.0f, 1.0f, 1.0f);
ImVec4 AccentHover   = ImVec4(0x90 / 255.0f, 0x68 / 255.0f, 1.0f, 1.0f);
ImVec4 AccentActive  = ImVec4(0x4E / 255.0f, 0x2D / 255.0f, 0xC9 / 255.0f, 1.0f);
ImVec4 AccentGlow    = ImVec4(0x7C / 255.0f, 0x4D / 255.0f, 1.0f, 0.35f);
ImVec4 TextPrimary   = ImVec4(0xF5 / 255.0f, 0xF7 / 255.0f, 0xFA / 255.0f, 1.0f);
ImVec4 TextSecondary = ImVec4(0x9A / 255.0f, 0xA4 / 255.0f, 0xB2 / 255.0f, 1.0f);
ImVec4 Success       = ImVec4(0.0f, 0xE6 / 255.0f, 0x76 / 255.0f, 1.0f);
ImVec4 Warning       = ImVec4(1.0f, 0xB3 / 255.0f, 0x36 / 255.0f, 1.0f);
ImVec4 Offline       = ImVec4(0x7B / 255.0f, 0x81 / 255.0f, 0x90 / 255.0f, 1.0f);
ImVec4 Error         = ImVec4(1.0f, 0.22f, 0.32f, 1.0f);
ImVec4 Disabled      = ImVec4(0.45f, 0.47f, 0.53f, 1.0f);
}

static ThemePreset CurrentPreset = ThemePreset::DefaultPurple;

namespace Sizes {
const float WindowRounding = 18.0f;
const float ChildRounding = 12.0f;
// 手机上 40px 偏小（2510 宽的屏），按下去经常偏；抬到 46/46
const float ButtonHeight = 46.0f;
const float InputHeight = 46.0f;
const float GameInputHeight = 42.0f;
const float RowHeightMin = 66.0f;
const float RowHeightMax = 82.0f;
const float SidebarMinWidth = 186.0f;
const float SidebarMaxWidth = 230.0f;
const float SidebarCompactWidth = 150.0f;
const float MobileBreakpoint = 560.0f;
const float IconSize = 20.0f;
const ImVec2 SwitchSize = ImVec2(58.0f, 34.0f);
const float TitleBarHeight = 72.0f;
}

namespace Spacing {
// 按钮/输入框/滑条的高度全靠这个：9 → 13 把每个控件抬到 ~50px
const ImVec2 FramePadding = ImVec2(14.0f, 13.0f);
const ImVec2 ItemSpacing = ImVec2(12.0f, 14.0f);
const ImVec2 ItemInnerSpacing = ImVec2(10.0f, 7.0f);
const float WindowSafeMargin = 32.0f;
const float ComponentPadding = 14.0f;
const float RowGap = 12.0f;
const float SidebarPadding = 14.0f;
}

ImVec4 WithAlpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

ImVec4 LerpColor(const ImVec4& from, const ImVec4& to, float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    return ImVec4(from.x + (to.x - from.x) * t,
                  from.y + (to.y - from.y) * t,
                  from.z + (to.z - from.z) * t,
                  from.w + (to.w - from.w) * t);
}

const char* PresetName(ThemePreset preset) {
    switch (preset) {
        case ThemePreset::DefaultPurple: return "Default Purple";
        case ThemePreset::Blue: return "Blue";
        case ThemePreset::Red: return "Red";
        case ThemePreset::Green: return "Green";
        case ThemePreset::Monochrome: return "Monochrome";
    }
    return "Default Purple";
}

void SetPreset(ThemePreset preset) {
    CurrentPreset = preset;
    switch (preset) {
        case ThemePreset::Blue:
            Colors::Accent = ImVec4(0.20f, 0.58f, 1.0f, 1.0f);
            Colors::AccentHover = ImVec4(0.35f, 0.68f, 1.0f, 1.0f);
            Colors::AccentActive = ImVec4(0.10f, 0.38f, 0.82f, 1.0f);
            Colors::AccentGlow = ImVec4(0.20f, 0.58f, 1.0f, 0.35f);
            break;
        case ThemePreset::Red:
            Colors::Accent = ImVec4(0.94f, 0.10f, 0.20f, 1.0f);
            Colors::AccentHover = ImVec4(1.0f, 0.24f, 0.34f, 1.0f);
            Colors::AccentActive = ImVec4(0.70f, 0.04f, 0.12f, 1.0f);
            Colors::AccentGlow = ImVec4(0.94f, 0.10f, 0.20f, 0.35f);
            break;
        case ThemePreset::Green:
            Colors::Accent = ImVec4(0.0f, 0.90f, 0.46f, 1.0f);
            Colors::AccentHover = ImVec4(0.18f, 1.0f, 0.58f, 1.0f);
            Colors::AccentActive = ImVec4(0.0f, 0.58f, 0.30f, 1.0f);
            Colors::AccentGlow = ImVec4(0.0f, 0.90f, 0.46f, 0.30f);
            break;
        case ThemePreset::Monochrome:
            Colors::Accent = ImVec4(0.84f, 0.87f, 0.92f, 1.0f);
            Colors::AccentHover = ImVec4(0.95f, 0.96f, 0.98f, 1.0f);
            Colors::AccentActive = ImVec4(0.58f, 0.62f, 0.70f, 1.0f);
            Colors::AccentGlow = ImVec4(0.84f, 0.87f, 0.92f, 0.22f);
            break;
        case ThemePreset::DefaultPurple:
        default:
            Colors::Accent = ImVec4(0x7C / 255.0f, 0x4D / 255.0f, 1.0f, 1.0f);
            Colors::AccentHover = ImVec4(0x90 / 255.0f, 0x68 / 255.0f, 1.0f, 1.0f);
            Colors::AccentActive = ImVec4(0x4E / 255.0f, 0x2D / 255.0f, 0xC9 / 255.0f, 1.0f);
            Colors::AccentGlow = ImVec4(0x7C / 255.0f, 0x4D / 255.0f, 1.0f, 0.35f);
            break;
    }
}

ThemePreset GetPreset() {
    return CurrentPreset;
}

void Apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = Sizes::WindowRounding;
    style.ChildRounding = Sizes::ChildRounding;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 10.0f;
    style.GrabRounding = 8.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.FramePadding = Spacing::FramePadding;
    style.ItemSpacing = Spacing::ItemSpacing;
    style.ItemInnerSpacing = Spacing::ItemInnerSpacing;
    style.ScrollbarSize = 5.0f;

    style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_ChildBg] = Colors::Panel;
    style.Colors[ImGuiCol_PopupBg] = Colors::Panel;
    style.Colors[ImGuiCol_Text] = Colors::TextPrimary;
    style.Colors[ImGuiCol_TextDisabled] = Colors::TextSecondary;
    style.Colors[ImGuiCol_Border] = Colors::Border;
    style.Colors[ImGuiCol_FrameBg] = WithAlpha(Colors::Background, 0.44f);
    style.Colors[ImGuiCol_FrameBgHovered] = WithAlpha(Colors::Accent, 0.18f);
    style.Colors[ImGuiCol_FrameBgActive] = WithAlpha(Colors::Accent, 0.26f);
    style.Colors[ImGuiCol_CheckMark] = Colors::Accent;
    style.Colors[ImGuiCol_Header] = WithAlpha(Colors::Accent, 0.16f);
    style.Colors[ImGuiCol_HeaderHovered] = WithAlpha(Colors::Accent, 0.24f);
    style.Colors[ImGuiCol_HeaderActive] = WithAlpha(Colors::Accent, 0.32f);
    style.Colors[ImGuiCol_SliderGrab] = Colors::Accent;
    style.Colors[ImGuiCol_SliderGrabActive] = Colors::Accent;
}
}
