#pragma once

#include "TemplateModels.h"
#include "imgui.h"

namespace Theme {
namespace Colors {
extern ImVec4 Background;
extern ImVec4 Panel;
extern ImVec4 ChildPanel;
extern ImVec4 Border;
extern ImVec4 Accent;
extern ImVec4 AccentHover;
extern ImVec4 AccentActive;
extern ImVec4 AccentGlow;
extern ImVec4 TextPrimary;
extern ImVec4 TextSecondary;
extern ImVec4 Success;
extern ImVec4 Warning;
extern ImVec4 Offline;
extern ImVec4 Error;
extern ImVec4 Disabled;
}

namespace Sizes {
extern const float WindowRounding;
extern const float ChildRounding;
extern const float ButtonHeight;
extern const float InputHeight;
extern const float GameInputHeight;
extern const float RowHeightMin;
extern const float RowHeightMax;
extern const float SidebarMinWidth;
extern const float SidebarMaxWidth;
extern const float SidebarCompactWidth;
extern const float MobileBreakpoint;
extern const float IconSize;
extern const ImVec2 SwitchSize;
extern const float TitleBarHeight;
}

namespace Spacing {
extern const ImVec2 FramePadding;
extern const ImVec2 ItemSpacing;
extern const ImVec2 ItemInnerSpacing;
extern const float WindowSafeMargin;
extern const float ComponentPadding;
extern const float RowGap;
extern const float SidebarPadding;
}

ImVec4 WithAlpha(ImVec4 color, float alpha);
ImVec4 LerpColor(const ImVec4& from, const ImVec4& to, float t);
const char* PresetName(ThemePreset preset);
void SetPreset(ThemePreset preset);
ThemePreset GetPreset();
void Apply();
}
