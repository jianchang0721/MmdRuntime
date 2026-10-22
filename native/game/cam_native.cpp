// =====================================================================
// cam_native.cpp —— 直接读写 Transform native 内存
// =====================================================================
#include "cam_native.h"

#include <cmath>
#include <cstring>
#include <cstdint>

#include "logsink.h"
#include "unity/il2cpp_api.h"

#define NTAG "MmdCamNat"
#define NLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {
namespace cam_native {

namespace {

void*  g_camTrManaged = nullptr;
void*  g_camTrNative  = nullptr;

// 偏移：运行时探测。默认 -1 = 未探测。
int    g_offPos = -1;
int    g_offRot = -1;
int    g_offWorldPos = -1;
int    g_offWorldRot = -1;

// 候选偏移表（Unity Transform 的常见布局）
const int kPosCand[] = { 0x10, 0x14, 0x18, 0x90, 0xa0, 0xb0 };
const int kRotCand[] = { 0x30, 0x34, 0x38, 0x3c, 0xa0, 0xc0 };

bool plausible_pos(const float* p) {
    // 世界坐标：非零、有限、量级合理（游戏世界几百米内）
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    const float m = fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]);
    return m > 0.5f && m < 100000.0f;
}

bool plausible_rot(const float* q) {
    if (!std::isfinite(q[0]) || !std::isfinite(q[1]) ||
        !std::isfinite(q[2]) || !std::isfinite(q[3])) return false;
    const float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    return n > 0.5f && n < 2.0f;   // 单位四元数附近
}

}  // namespace

int  off_pos() { return g_offPos; }
int  off_rot() { return g_offRot; }

void* attach(void* camTransform) {
    if (!camTransform) return nullptr;
    g_camTrManaged = camTransform;
    // managed Object: klass(8) monitor(8) m_CachedPtr(8) ...
    g_camTrNative = *(void**)((char*)camTransform + 16);
    NLOG("[CAMNAT] managed=%p native=%p", camTransform, g_camTrNative);
    return g_camTrNative;
}

bool get_world_pos(void* tr, float out[3]) {
    void* nat = tr ? *(void**)((char*)tr + 16) : g_camTrNative;
    if (!nat) return false;
    // 先找「哪个偏移存的是合理位置」
    if (g_offWorldPos < 0) {
        for (int off : kPosCand) {
            float* p = (float*)((char*)nat + off);
            if (plausible_pos(p)) { g_offWorldPos = off; break; }
        }
    }
    if (g_offWorldPos >= 0) {
        float* p = (float*)((char*)nat + g_offWorldPos);
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
        return true;
    }
    return false;
}

bool set_world_pos(void* tr, const float p[3]) {
    void* nat = tr ? *(void**)((char*)tr + 16) : g_camTrNative;
    if (!nat) return false;
    if (g_offWorldPos < 0) {
        float tmp[3];
        if (!get_world_pos(tr, tmp)) return false;
    }
    if (g_offWorldPos >= 0) {
        float* d = (float*)((char*)nat + g_offWorldPos);
        d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
        return true;
    }
    return false;
}

bool get_world_rot(void* tr, float out[4]) {
    void* nat = tr ? *(void**)((char*)tr + 16) : g_camTrNative;
    if (!nat) return false;
    if (g_offWorldRot < 0) {
        for (int off : kRotCand) {
            float* q = (float*)((char*)nat + off);
            if (plausible_rot(q)) { g_offWorldRot = off; break; }
        }
    }
    if (g_offWorldRot >= 0) {
        float* q = (float*)((char*)nat + g_offWorldRot);
        out[0] = q[0]; out[1] = q[1]; out[2] = q[2]; out[3] = q[3];
        return true;
    }
    return false;
}

bool set_world_rot(void* tr, const float q[4]) {
    void* nat = tr ? *(void**)((char*)tr + 16) : g_camTrNative;
    if (!nat) return false;
    if (g_offWorldRot < 0) {
        float tmp[4];
        if (!get_world_rot(tr, tmp)) return false;
    }
    if (g_offWorldRot >= 0) {
        float* d = (float*)((char*)nat + g_offWorldRot);
        d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
        return true;
    }
    return false;
}

}  // namespace cam_native
}  // namespace game
