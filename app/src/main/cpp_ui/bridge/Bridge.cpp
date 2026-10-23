#include "Bridge.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "AppState.h"
#include "elf_util.h"   // host/elf_util.h
#include "mmd_api.h"    // native/public/mmd_api.h

namespace Bridge {

static BridgeModel g_Model;
static const MmdApi* g_Api = nullptr;
static int64_t g_LastBindTry = 0;

BridgeModel& Shared() { return g_Model; }

static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ------------------------------------------------------------------
// 绑定 mmd_get_api
//
// 不能用 dlopen("libmmdruntime.so", RTLD_NOLOAD)：HookEntry 为了让同一个
// so 能重复加载，会把它复制成 libmmdruntime_1.so / _2.so…… 然后 System.load
// 绝对路径。dlopen 按 soname 找会直接错过。
//
// 所以走跟游戏侧同一套：/proc/self/maps 找基址 + 手工解析 .dynsym。
// ------------------------------------------------------------------
bool Bind() {
    if (g_Api) return true;

    const int64_t now = NowMs();
    if (now - g_LastBindTry < 1000) return false;
    g_LastBindTry = now;

    // 找游戏侧 so。不能按原名找 —— HookEntry 会把它改名成
    // libil2cpp_ext_1.so。按「私有目录 + .so」找，游戏自己不会从那儿加载。
    uintptr_t base = elfu::MapsBaseContains("/data/data/com.hermes.h1game/files/", ".so");
    if (!base) base = elfu::MapsBaseContains("/data/user/0/com.hermes.h1game/files/", ".so");
    if (!base) base = elfu::MapsBase("libil2cpp_ext");
    if (!base) base = elfu::MapsBase("libmmdruntime");
    if (!base) {
        g_LastConfigStatus = "游戏侧 so 还没加载";
        return false;
    }

    // 私有目录里可能不止一个 so（比如之前版本残留），逐个试到能拿到符号为止
    uintptr_t sym = elfu::ElfSym(base, "mmd_get_api");
    if (!sym) {
        // 换个思路：直接遍历 maps 里所有私有目录 so 的 base
        uintptr_t alt = elfu::MapsBase("libil2cpp_ext");
        if (alt && alt != base) sym = elfu::ElfSym(alt, "mmd_get_api");
    }
    if (!sym) {
        g_LastConfigStatus = "找不到 mmd_get_api";
        return false;
    }
    auto fn = (const MmdApi* (*)(void))sym;
    const MmdApi* api = fn();
    if (!api || api->apiVersion != MMD_API_VERSION || api->structSize != (int)sizeof(MmdApi)) {
        g_LastConfigStatus = "API 版本不匹配";
        return false;
    }

    g_Api = api;
    g_LastConfigStatus = "已直连游戏侧";
    if (api->log) api->log("UI bound via mmd_get_api");
    return true;
}

bool Connected() { return g_Api != nullptr; }

// ------------------------------------------------------------------
// 拉状态
// ------------------------------------------------------------------
void PollStatus() {
    if (!Bind()) {
        BridgeModel& m = Shared();
        m.state = GameState::Unknown;
        m.statusMessage = g_LastConfigStatus;
        return;
    }

    MmdStatus st;
    g_Api->get_status(&st);

    BridgeModel& m = Shared();
    switch (st.gameState) {
        case MMD_GS_DETACHED: m.state = GameState::Detached;  break;
        case MMD_GS_ATTACHED: m.state = GameState::Attached;  break;
        case MMD_GS_LOADING:  m.state = GameState::Loading;   break;
        case MMD_GS_ACTIVE:   m.state = GameState::Active;    break;
        case MMD_GS_ERROR:    m.state = GameState::ErrorState;break;
        default:              m.state = GameState::Unknown;   break;
    }
    m.statusMessage      = st.statusMessage[0] ? st.statusMessage : "等待游戏侧 so…";
    m.activeModel        = st.activeModel[0] ? st.activeModel : "无";
    m.lastResult         = st.lastResult;
    m.smrTotal           = st.smrTotal;
    m.smrReplaced        = st.smrReplaced;
    m.boneCount          = st.boneCount;
    m.vertexCount        = st.vertexCount;
    m.replaceProgress    = (float)st.progress / 100.0f;
    m.lastErrorCode      = st.lastResult == 0 ? 1 : 0;
    m.lastErrorMessage   = st.lastError;
    m.lastReadMs         = NowMs();

    // 回读开关，UI 上滑条/开关跟游戏侧保持一致
    m.enableReplace      = st.enableReplace != 0;
    if (st.modelScale > 0.0f) m.modelScale = st.modelScale;
    (void)st.statusVersion;
    (void)st.uptimeMs;
    (void)st.stage;
    (void)st.triCount;
    (void)st.pmxPath;
    (void)st.logPath;
}

// ------------------------------------------------------------------
// 模型列表：目录条目 + 单个 .pmx 文件都收
// ------------------------------------------------------------------
void RefreshModelList() {
    BridgeModel& model = Shared();
    model.models.clear();

    // 先问游戏侧：模型都在它自己的私有目录里，分区存储下我们读不到
    if (Bind() && g_Api->list_models) {
        static char buf[8192];
        buf[0] = 0;
        const int n = g_Api->list_models(buf, (int)sizeof(buf));
        if (n > 0) {
            const char* p = buf;
            while (*p) {
                const char* e = p;
                while (*e && *e != '\n') e++;
                if (e > p) model.models.emplace_back(p, (size_t)(e - p));
                p = (*e == '\n') ? e + 1 : e;
            }
            std::sort(model.models.begin(), model.models.end());
            model.storageDir = "/data/data/com.hermes.h1game/files/models";
            if (model.selectedModel >= (int)model.models.size()) model.selectedModel = 0;
            if (!model.models.empty() && model.modelPath.empty()) {
                model.modelPath = model.models[model.selectedModel];
            }
            return;
        }
    }

    // 退回扫 /sdcard（旧逻辑，分区存储下大概率为空）
    if (model.storageDir.empty()) model.storageDir = "/sdcard/MT2/ModelMod/ngyu";

    DIR* handle = opendir(model.storageDir.c_str());
    if (!handle) return;
    struct dirent* entry = nullptr;
    while ((entry = readdir(handle)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (entry->d_type == DT_DIR) {
            model.models.push_back(name);
        } else {
            const size_t dot = name.rfind('.');
            if (dot != std::string::npos && name.substr(dot) == ".pmx") {
                model.models.push_back(name);
            }
        }
    }
    closedir(handle);
    std::sort(model.models.begin(), model.models.end());

    if (model.selectedModel >= (int)model.models.size()) model.selectedModel = 0;
    if (!model.models.empty() && model.modelPath.empty()) {
        model.modelPath = model.storageDir + "/" + model.models[model.selectedModel];
    }
}

// ------------------------------------------------------------------
// setters —— 本地记一份，同时直接喂给游戏侧
// ------------------------------------------------------------------
void SetStorageDir(const std::string& path) {
    Shared().storageDir = path;
    RefreshModelList();
}

void SetEnableReplace(bool value) {
    Shared().enableReplace = value;
    Shared().lastWriteMs = NowMs();
    if (Bind()) g_Api->set_enable(value ? 1 : 0);
}

void SetModelScale(float value) {
    Shared().modelScale = value;
    Shared().lastWriteMs = NowMs();
    if (Bind()) g_Api->set_scale(value);
}

void SetModelPath(const std::string& value) {
    Shared().modelPath = value;
}

void SetShowBounds(bool value) { Shared().showBounds = value; }

void SetManualOffset(float x, float y, float z) {
    BridgeModel& model = Shared();
    model.offsetX = x;
    model.offsetY = y;
    model.offsetZ = z;
}

void SelectModel(const std::string& name) {
    BridgeModel& model = Shared();
    for (size_t i = 0; i < model.models.size(); ++i) {
        if (model.models[i] == name) {
            model.selectedModel = (int)i;
            model.modelPath = model.storageDir + "/" + name;
        }
    }
    model.lastWriteMs = NowMs();
    // 游戏侧按「目录名」在自己私有目录 models/<名字> 里递归找 pmx
    if (Bind()) g_Api->set_model(name.c_str());
}

void RequestInit() {
    Shared().pendingCommand = "init";
    if (Bind() && g_Api->cmd_init) g_Api->cmd_init();
}

void RequestRescan() {
    Shared().pendingCommand = "rescan";
    if (Bind()) g_Api->cmd_rescan();
}

void RequestReapply() {
    Shared().pendingCommand = "reapply";
    if (Bind()) g_Api->cmd_reapply();
}

void ImportModelsFromConfig() {
    RefreshModelList();
    if (Bind()) g_Api->log("model list refreshed");
}

int CameraEnable(int on) {
    if (!Bind() || !g_Api->camera_enable) return 0;
    return g_Api->camera_enable(on ? 1 : 0);
}

int CameraEnabled() {
    if (!Bind() || !g_Api->camera_is_enabled) return 0;
    return g_Api->camera_is_enabled();
}

void CameraReset() {
    if (Bind() && g_Api->camera_reset) g_Api->camera_reset();
}

void CameraZoom(float delta) {
    if (Bind() && g_Api->camera_zoom) g_Api->camera_zoom(delta);
}

void CameraMotion(int ascend, int descend, int boost) {
    if (Bind() && g_Api->camera_motion)
        g_Api->camera_motion(ascend ? 1 : 0, descend ? 1 : 0, boost ? 1 : 0);
}

void CameraConfig(float speed, float sens, float radius) {
    if (Bind() && g_Api->camera_config)
        g_Api->camera_config(speed, sens, radius);
}

void RenderTick() {
    if (Bind() && g_Api->render_tick) g_Api->render_tick();
}

}  // namespace Bridge
