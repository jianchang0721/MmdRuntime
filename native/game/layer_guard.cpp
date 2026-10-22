// =====================================================================
// layer_guard.cpp —— 模型层 + 相机 cullingMask 守卫（实现）
// =====================================================================
#include "layer_guard.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/syscall.h>

#include "logsink.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"

#define GTAG "MmdLayer"
#define GLOG(...) logsink::Write(__VA_ARGS__)

namespace game {
namespace layer_guard {

namespace {

void* g_go    = nullptr;
int   g_layer = -1;
bool  g_camDumped = false;

long long now_ms() {
    struct timespec ts;
    syscall(__NR_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int read_layer(void* go) {
    if (!go) return -1;
    void* cls = uo::u_class("UnityEngine", "GameObject");
    void* m = cls ? uo::u_method(cls, "get_layer", 0) : nullptr;
    if (!m) return -1;
    void* r = uo::u_call(go, m, nullptr);
    return r ? *(int32_t*)((char*)r + 16) : -1;
}

bool write_layer(void* go, int layer) {
    if (!go) return false;
    void* cls = uo::u_class("UnityEngine", "GameObject");
    void* m = cls ? uo::u_method(cls, "set_layer", 1) : nullptr;
    if (!m) return false;
    void* a[1] = { &layer };
    return uo::u_call_void(go, m, a);
}

// 每台相机 dump 一次：layerCullDistances / layerCullSpherical
void dump_camera_once(void* cam, int idx) {
    void* camCls = uo::u_class("UnityEngine", "Camera");
    if (!camCls) return;

    void* mLCD = uo::u_method(camCls, "get_layerCullDistances", 0);
    if (mLCD) {
        void* arr = uo::u_call(cam, mLCD, nullptr);
        if (arr) {
            int32_t n = *(int32_t*)((char*)arr + 24);
            float* d = (float*)((char*)arr + 32);
            char buf[256];
            int off = 0;
            buf[0] = 0;
            for (int i = 0; i < n && i < 32 && off < (int)sizeof(buf) - 20; i++) {
                if (d[i] > 0.0f)
                    off += snprintf(buf + off, sizeof(buf) - off, " L%d=%.0f", i, d[i]);
            }
            GLOG("[CAM] cam[%d] layerCullDistances n=%d%s", idx, n,
                 off ? buf : " 全0(不限距)");
        } else {
            GLOG("[CAM] cam[%d] layerCullDistances=null", idx);
        }
    } else {
        GLOG("[CAM] cam[%d] 没有 get_layerCullDistances", idx);
    }

    void* mSph = uo::u_method(camCls, "get_layerCullSpherical", 0);
    if (mSph) {
        void* r = uo::u_call(cam, mSph, nullptr);
        GLOG("[CAM] cam[%d] layerCullSpherical=%d", idx,
             r ? (*(uint8_t*)((char*)r + 16) ? 1 : 0) : -1);
    }
}

}  // namespace

// 读一台相机的 cullingMask。优先 Camera.main，拿不到用 allCameras[0]。
static uint32_t main_camera_mask(bool* ok) {
    if (ok) *ok = false;
    void* camCls = uo::u_class("UnityEngine", "Camera");
    if (!camCls) return 0;
    void* mMask = uo::u_method(camCls, "get_cullingMask", 0);
    if (!mMask) return 0;

    void* cam = nullptr;
    void* mMain = uo::u_method(camCls, "get_main", 0);
    if (mMain) cam = uo::u_call(nullptr, mMain, nullptr);
    if (!cam) {
        void* mAll = uo::u_method(camCls, "get_allCameras", 0);
        void* arr = mAll ? uo::u_call(nullptr, mAll, nullptr) : nullptr;
        if (arr && *(int32_t*)((char*)arr + 24) > 0)
            cam = *(void**)((char*)arr + 32);
    }
    if (!cam) return 0;

    void* r = uo::u_call(cam, mMask, nullptr);
    if (!r) return 0;
    if (ok) *ok = true;
    return *(uint32_t*)((char*)r + 16);
}

// 选一个「主相机真看得见」的层。
//
// 为什么不能直接抄角色层：实测这局角色在 layer 23，主相机 mask=0xFB7BFF57
// 里根本没 bit23，而且游戏每 2 秒就把 mask 改回去（ensure_once 补了又被覆盖），
// 结果模型永远 isVisible=0。抢 mask 抢不过，就换个本来就开着的层。
static int pick_visible_layer(int want) {
    bool ok = false;
    uint32_t mask = main_camera_mask(&ok);
    if (!ok) {
        GLOG("[LAYER] 读不到主相机 mask，直接用角色层 %d", want);
        return want;
    }
    if (want >= 0 && want < 32 && (mask & (1u << want))) {
        GLOG("[LAYER] 角色层 %d 已在主相机 mask 0x%08X 里，直接用", want, mask);
        return want;
    }
    // 偏好：10/0/8/9… 这些是常见角色/默认层，光照和遮挡行为最接近角色
    static const int pref[] = { 10, 0, 8, 9, 11, 12, 13, 6, 5, 4, 2, 1 };
    for (int i = 0; i < (int)(sizeof(pref) / sizeof(pref[0])); i++) {
        int L = pref[i];
        if (mask & (1u << L)) {
            GLOG("[LAYER] 角色层 %d 不在主相机 mask 0x%08X，改用 %d", want, mask, L);
            return L;
        }
    }
    for (int L = 0; L < 32; L++) {
        if (mask & (1u << L)) {
            GLOG("[LAYER] 角色层 %d 不可见，兜底用 mask 里第一个置位层 %d", want, L);
            return L;
        }
    }
    return want;
}

int apply(void* go, void* sourceSmr) {
    if (!go) { GLOG("[LAYER] apply: go 为空"); return -1; }

    int want = -1;
    void* sgo = nullptr;
    if (sourceSmr) {
        void* compCls = uo::u_class("UnityEngine", "Component");
        void* mGetGO = compCls ? uo::u_method(compCls, "get_gameObject", 0) : nullptr;
        sgo = mGetGO ? uo::u_call(sourceSmr, mGetGO, nullptr) : nullptr;
        want = read_layer(sgo);
        GLOG("[LAYER] ref GO=%p layer=%d", sgo, want);
    }
    if (want < 0) {
        GLOG("[LAYER] 读 ref layer 失败，不动模型层");
        return -1;
    }

    const int use = pick_visible_layer(want);

    int before = read_layer(go);
    bool called = write_layer(go, use);
    int after = read_layer(go);

    GLOG("[LAYER] model GO=%p layer %d -> %d (setLayer调用=%d 读回=%d)%s",
         go, before, use, (int)called, after,
         (after == use) ? "  OK" : "  <-- 写入没生效！");

    register_model(go, after >= 0 ? after : want);
    return after;
}

void register_model(void* go, int layer) {
    g_go = go;
    g_layer = layer;
    g_camDumped = false;
    GLOG("[LAYER] 模型登记: go=%p layer=%d", go, layer);
}

void unregister_model() {
    g_go = nullptr;
    g_layer = -1;
    g_camDumped = false;
}

void ensure_once() {
    if (!g_go) return;

    // 先看模型 GO 现在真在哪层（不信变量，读回来）
    int real = read_layer(g_go);
    if (real >= 0 && real != g_layer) {
        GLOG("[CAM] model GO layer 变了 %d -> %d，跟随真值", g_layer, real);
        g_layer = real;
    }
    if (g_layer < 0 || g_layer > 31) return;   // layer 合法范围 0..31，越界下面移位就是 UB

    void* camCls = uo::u_class("UnityEngine", "Camera");
    if (!camCls) return;
    void* mMask = uo::u_method(camCls, "get_cullingMask", 0);
    void* mSet  = uo::u_method(camCls, "set_cullingMask", 1);
    if (!mMask || !mSet) {
        GLOG("[CAM] cullingMask getter/setter 缺失");
        return;
    }

    const uint32_t bit = 1u << (uint32_t)g_layer;
    int checked = 0, patched = 0, hit = 0;

    void* mAll = uo::u_method(camCls, "get_allCameras", 0);
    void* arr = mAll ? uo::u_call(nullptr, mAll, nullptr) : nullptr;
    if (arr) {
        int32_t n = *(int32_t*)((char*)arr + 24);
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            void* cam = *(void**)((char*)arr + 32 + (size_t)i * 8);
            if (!cam) continue;
            checked++;
            void* r = uo::u_call(cam, mMask, nullptr);
            if (!r) continue;
            uint32_t cur = *(uint32_t*)((char*)r + 16);
            if (!g_camDumped) dump_camera_once(cam, i);
            if (cur & bit) { hit++; continue; }
            // 纯 UI 相机（只开 layer 5）不碰：它只负责画界面，
            // 给它补世界层会让模型被 UI 相机再画一遍、盖在界面上。
            // mask==0 的相机不碰：那是切场景中途/未初始化的相机，
            // 给它补层反而会多画东西（实测 build-129：cam[0]=0 被补成 0x400）
            if (cur == 0) continue;
            if (cur == (1u << 5)) {
                static bool skipLogged = false;
                if (!skipLogged) {
                    skipLogged = true;
                    GLOG("[CAM] cam[%d] 是纯 UI 相机（mask=0x%08X），跳过（只打一次）", i, cur);
                }
                continue;
            }
            uint32_t want = cur | bit;
            void* a[1] = { &want };
            uo::u_call_void(cam, mSet, a);
            patched++;
            GLOG("[CAM] cam[%d] mask 0x%08X -> 0x%08X (补 layer %d)",
                 i, cur, want, g_layer);
        }
        g_camDumped = true;
    }

    // 兜底：allCameras 拿不到就用 Camera.main
    if (checked == 0) {
        void* mMain = uo::u_method(camCls, "get_main", 0);
        void* cam = mMain ? uo::u_call(nullptr, mMain, nullptr) : nullptr;
        if (cam) {
            void* r = uo::u_call(cam, mMask, nullptr);
            if (r) {
                checked = 1;
                uint32_t cur = *(uint32_t*)((char*)r + 16);
                if (!g_camDumped) { dump_camera_once(cam, 0); g_camDumped = true; }
                if (cur & bit) {
                    hit++;
                } else {
                    uint32_t want = cur | bit;
                    void* a[1] = { &want };
                    uo::u_call_void(cam, mSet, a);
                    patched++;
                    GLOG("[CAM] main mask 0x%08X -> 0x%08X (补 layer %d)",
                         cur, want, g_layer);
                }
            }
        }
    }

    // 只在变化时打汇总，避免每 200ms 刷屏
    static int lastChecked = -1, lastPatched = -1, lastHit = -1;
    if (checked != lastChecked || patched != lastPatched || hit != lastHit) {
        GLOG("[CAM] 相机=%d 含layer%d=%d 本次补丁=%d", checked, g_layer, hit, patched);
        lastChecked = checked;
        lastPatched = patched;
        lastHit = hit;
    }
}

void tick() {
    // 以前 200ms 一次，实测游戏每 2 秒把 mask 改回去、中间那段就是黑的；
    // 现在每帧都补（相机就 2~4 个，开销可以忽略）。
    static long long last = 0;
    long long t = now_ms();
    if (t - last < 16) return;
    last = t;
    ensure_once();
}

}  // namespace layer_guard
}  // namespace game
