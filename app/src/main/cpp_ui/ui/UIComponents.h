#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "TemplateModels.h"
#include "imgui.h"

namespace UI {
void Icon(IconId icon, float size, ImVec4 color);
void IconText(IconId icon, const char* text, ImVec4 color);
void DrawIconAt(ImDrawList* dl, IconId icon, ImVec2 min, float size, ImVec4 color);
void IconPlaceholder(ImDrawList* dl, ImVec2 min, ImVec2 size, bool gamepad);
void StatusDot(ImDrawList* dl, ImVec2 center, bool online);

bool Button(const char* label, ImVec2 size);
bool IconButton(const char* label, ImVec2 size);
bool IconButton(IconId icon, const char* id);
bool InputText(const char* id, char* buffer, size_t bufferSize, const char* hint, ImVec2 size);
bool SearchInput(char* buffer, size_t bufferSize, float width);
bool Toggle(const char* id, bool* value);
bool Slider(const char* id, float* value, float width);
bool Dropdown(const char* id, int* index, float width);
bool Dropdown(const char* id, int* index, const std::vector<std::string>& options, float width);
bool Keybind(const char* label, ImVec2 size);
bool Badge(const char* label, ImVec2 size = ImVec2(84.0f, 32.0f));

void Section(const char* title, const char* subtitle = nullptr);
void LabelBlock(const char* label, const char* desc = nullptr);
ImVec2 Card(const char* label, const char* desc, float height);
ImVec2 CardSized(const char* label, const char* desc, float width, float height);
bool SidebarTab(const FeatureTab& tab, int index, int* activeTab, ImVec2 min, ImVec2 max);
void FooterStatus(const char* left, const char* right, float y, float width, bool online);
bool ErrorPanel(const char* title, const char* message, const char* optionalButtonText = nullptr);
bool GameRow(const GameProfile& game, float y, float width);
}
