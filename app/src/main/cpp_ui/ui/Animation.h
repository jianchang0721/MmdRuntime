#pragma once

#include <string>
#include <unordered_map>

#include "TemplateModels.h"
#include "imgui.h"

namespace Animation {

struct State {
    float hover = 0.0f;
    float active = 0.0f;
    float open = 1.0f;
    float pulse = 0.0f;
};

State& Get(const std::string& id);
float Approach(float value, float target, float speed);
float EaseOutCubic(float t);
void UpdateBool(float& value, bool enabled, float speed = 8.0f);
void TriggerPulse(const std::string& id);

}
