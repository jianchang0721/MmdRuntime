// =====================================================================
// render_probe.cpp —— 渲染诊断 + hook 点探测
//
// 用途：
//   1. hook libunity.so 渲染函数，确认是不是 SMR 渲染路径
//   2. 检查自建 SMR 的渲染状态（isVisible、bounds、materials）
// =====================================================================
#include "render_probe.h"

#include <cstring>
#include <cstdlib>

#include "../logsink.h"
#include "../unity/il2cpp_api.h"
#include "../unity/unity_objects.h"

#define PTAG "MmdProbe"
#define PLOG(...) logsink::Write(__VA_ARGS__)

namespace game {

namespace {

// libunity.so 基址（运行时获取）
void* g_unityBase = nullptr;

// hook 状态
bool g_hooked = false;
int g_renderCount = 0;

// 找 libunity.so 基址
void* find_unity_base() {
    if (g_unityBase) return g_unityBase;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libunity.so") && strstr(line, "r-xp")) {
            char* end = strchr(line, '-');
            if (end) {
                *end = 0;
                g_unityBase = (void*)strtoull(line, nullptr, 16);
                break;
            }
        }
    }
    fclose(f);
    return g_unityBase;
}

// hook 回调：渲染函数被调用
// 原函数签名可能是 void Renderer::Render(int) 或类似
// 我们只打印参数，不改行为
void (*g_orig_render)(void*) = nullptr;

void my_render_hook(void* arg0) {
    g_renderCount++;
    if (g_renderCount <= 5) {
        PLOG("[PROBE] render call #%d, arg0=%p", g_renderCount, arg0);
        if (arg0) {
            // 读对象前几个字段
            void** fields = (void**)arg0;
            PLOG("[PROBE]   +0: %p, +8: %p, +10: %p", fields[0], fields[1], fields[2]);
        }
    }
    if (g_orig_render) {
        g_orig_render(arg0);
    }
}

// 用 Dobby hook
bool hook_render_func() {
    if (g_hooked) return true;
    void* base = find_unity_base();
    if (!base) {
        PLOG("[PROBE] libunity.so not found in maps");
        return false;
    }
    
    // 0x835ce8 是 glDrawElements 调用者（从 frida 调用栈拿到）
    void* target = (char*)base + 0x835ce8;
    PLOG("[PROBE] target=%p (base=%p + 0x835ce8)", target, base);
    
    // 用 Dobby hook
    // DobbyHook(target, (void*)my_render_hook, (void**)&g_orig_render);
    // 但 Dobby 之前崩过，先试简单方式：只打印不 hook
    PLOG("[PROBE] skipping actual hook (Dobby risky), just logging");
    g_hooked = true;
    return true;
}

// 检查自建 SMR 状态
void probe_self_smr(SelfSmr& smr) {
    if (!smr.smr) {
        PLOG("[PROBE] self smr is null");
        return;
    }
    
    PLOG("[PROBE] === Self SMR State ===");
    PLOG("[PROBE] managed=%p native=%p", smr.smr, smr.native);
    
    // 读 native 字段
    if (smr.native) {
        uint8_t* base = (uint8_t*)smr.native;
        int32_t meshId = *(int32_t*)(base + 0x1d4);
        int32_t boneSize = *(int32_t*)(base + 0x1f0);
        void* boneData = *(void**)(base + 0x1e0);
        int64_t boneCap = *(int64_t*)(base + 0x1f8);
        
        PLOG("[PROBE] meshId=%d boneSize=%d boneData=%p boneCap=%lld",
             meshId, boneSize, boneData, (long long)boneCap);
    }
    
    // 调 get_bones 验证
    // 需要 MethodInfo，暂时跳过
}

// 检查 Mesh 数据
void probe_mesh(void* meshObj) {
    if (!meshObj) {
        PLOG("[PROBE] mesh is null");
        return;
    }
    
    PLOG("[PROBE] === Mesh State ===");
    PLOG("[PROBE] mesh=%p", meshObj);
    
    // 用 il2cpp API 读 vertexCount
    // 需要 mesh 的 klass 和方法
    // 暂时用硬编码偏移（有风险，但诊断用）
    uint8_t* base = (uint8_t*)meshObj;
    int32_t klass = *(int32_t*)base;
    PLOG("[PROBE] klass=%d", klass);
}

}  // namespace

bool probe_init() {
    PLOG("[PROBE] init");
    return hook_render_func();
}

void probe_self_smr_state(SelfSmr& smr, void* meshObj) {
    probe_self_smr(smr);
    probe_mesh(meshObj);
}

void probe_frame() {
    // 每帧调用，打印渲染统计
    static int frame = 0;
    frame++;
    if (frame % 60 == 0) {
        PLOG("[PROBE] frame %d, render calls %d", frame, g_renderCount);
    }
}

}  // namespace game
