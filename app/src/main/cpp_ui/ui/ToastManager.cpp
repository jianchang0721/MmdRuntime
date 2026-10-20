#include "ToastManager.h"

#include <algorithm>
#include <vector>

#include "TemplateModels.h"
#include "Theme.h"
#include "UIComponents.h"
#include "imgui.h"

namespace Toast {
struct Entry {
    ToastType type;
    std::string message;
    float age = 0.0f;
    float duration = 3.0f;
};

static std::vector<Entry> g_Toasts;

static IconId IconFor(ToastType type) {
    switch (type) {
        case ToastType::Success: return IconId::Success;
        case ToastType::Warning: return IconId::Warning;
        case ToastType::Error: return IconId::Error;
        case ToastType::Info:
        default: return IconId::Info;
    }
}

static ImVec4 ColorFor(ToastType type) {
    switch (type) {
        case ToastType::Success: return Theme::Colors::Success;
        case ToastType::Warning: return Theme::Colors::Warning;
        case ToastType::Error: return Theme::Colors::Error;
        case ToastType::Info:
        default: return Theme::Colors::Accent;
    }
}

static void Push(ToastType type, const std::string& message) {
    if (message.empty()) return;
    for (const auto& toast : g_Toasts) {
        if (toast.type == type && toast.message == message && toast.age < 0.35f) return;
    }
    if (g_Toasts.size() >= 4) g_Toasts.erase(g_Toasts.begin());
    g_Toasts.push_back({type, message, 0.0f, 3.0f});
}

void Info(const std::string& message) { Push(ToastType::Info, message); }
void Success(const std::string& message) { Push(ToastType::Success, message); }
void Warning(const std::string& message) { Push(ToastType::Warning, message); }
void Error(const std::string& message) { Push(ToastType::Error, message); }
int Count() { return (int)g_Toasts.size(); }

void Render() {
    if (g_Toasts.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    float dt = std::max(0.016f, io.DeltaTime);
    for (auto& toast : g_Toasts) toast.age += dt;
    g_Toasts.erase(std::remove_if(g_Toasts.begin(), g_Toasts.end(), [](const Entry& toast) {
        return toast.age > toast.duration + 0.45f;
    }), g_Toasts.end());

    ImVec2 viewport = ImGui::GetIO().DisplaySize;
    float toastW = std::min(360.0f, viewport.x - 32.0f);
    float toastH = 52.0f;
    float startY = viewport.y - 18.0f - toastH;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = (int)g_Toasts.size() - 1; i >= 0; --i) {
        const Entry& toast = g_Toasts[i];
        float fadeIn = ClampFloat(toast.age / 0.18f, 0.0f, 1.0f);
        float fadeOut = ClampFloat((toast.duration + 0.45f - toast.age) / 0.45f, 0.0f, 1.0f);
        float alpha = std::min(fadeIn, fadeOut);
        float x = viewport.x - toastW - 18.0f + (1.0f - alpha) * 18.0f;
        float y = startY - (float)((int)g_Toasts.size() - 1 - i) * (toastH + 10.0f);
        ImVec4 color = ColorFor(toast.type);
        ImVec2 min(x, y);
        ImVec2 max(x + toastW, y + toastH);
        dl->AddRectFilled(min, max, ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::Panel, 0.92f * alpha)), 12.0f);
        dl->AddRect(min, max, ImGui::GetColorU32(Theme::WithAlpha(color, 0.28f * alpha)), 12.0f, 0, 1.0f);
        UI::DrawIconAt(dl, IconFor(toast.type), ImVec2(min.x + 12.0f, min.y + 12.0f), 28.0f, Theme::WithAlpha(color, alpha));
        dl->AddText(ImVec2(min.x + 52.0f, min.y + 17.0f), ImGui::GetColorU32(Theme::WithAlpha(Theme::Colors::TextPrimary, alpha)), toast.message.c_str());
        float progress = ClampFloat(1.0f - toast.age / toast.duration, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(min.x + 12.0f, max.y - 4.0f),
                          ImVec2(min.x + 12.0f + (toastW - 24.0f) * progress, max.y - 2.0f),
                          ImGui::GetColorU32(Theme::WithAlpha(color, 0.70f * alpha)), 2.0f);
    }
}
}
