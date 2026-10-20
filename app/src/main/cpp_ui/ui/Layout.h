#pragma once

#include <algorithm>

#include "Theme.h"
#include "imgui.h"

namespace Layout {

inline float AvailableWidth() {
    return ImGui::GetContentRegionAvail().x;
}

inline bool Compact(float width) {
    return width < Theme::Sizes::MobileBreakpoint;
}

inline float SidebarWidth(float windowWidth) {
    if (windowWidth < 560.0f) return Theme::Sizes::SidebarCompactWidth;
    return ClampFloat(windowWidth * 0.22f, Theme::Sizes::SidebarMinWidth, Theme::Sizes::SidebarMaxWidth);
}

inline float ControlWidth(float rowWidth, bool stacked) {
    if (stacked) return std::max(1.0f, rowWidth - 36.0f);
    return ClampFloat(rowWidth * 0.38f, 132.0f, std::min(260.0f, rowWidth - 56.0f));
}

inline float LabelWidth(float rowWidth, float controlWidth, bool stacked) {
    return stacked ? std::max(1.0f, rowWidth - 36.0f) : std::max(1.0f, rowWidth - controlWidth - 54.0f);
}

inline void ApplyTouchScroll() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) return;
    if (ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDelta.y == 0.0f) return;
    float nextY = ImGui::GetScrollY() - io.MouseDelta.y;
    ImGui::SetScrollY(ClampFloat(nextY, 0.0f, ImGui::GetScrollMaxY()));
}

}
