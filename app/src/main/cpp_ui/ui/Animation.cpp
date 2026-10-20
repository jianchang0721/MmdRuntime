#include "Animation.h"

#include <algorithm>
#include <cmath>

namespace Animation {
static std::unordered_map<std::string, State> g_States;

State& Get(const std::string& id) {
    return g_States[id];
}

float Approach(float value, float target, float speed) {
    float dt = std::max(0.001f, ImGui::GetIO().DeltaTime);
    float step = speed * dt;
    if (value < target) return std::min(target, value + step);
    return std::max(target, value - step);
}

float EaseOutCubic(float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

void UpdateBool(float& value, bool enabled, float speed) {
    value = Approach(value, enabled ? 1.0f : 0.0f, speed);
}

void TriggerPulse(const std::string& id) {
    g_States[id].pulse = 1.0f;
}

}
