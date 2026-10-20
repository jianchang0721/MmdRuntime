// =====================================================================
// bridge.h —— UI ⇄ 游戏 的文件通道
//
// 协议见仓库根目录 BRIDGE.md。要点：
//   /data/local/tmp/mmdrt/ui.conf      UI → 游戏（带 version 脏检查）
//   /data/local/tmp/mmdrt/status.conf  游戏 → UI（原子写，节流 ≥200ms）
//   目录由 UI 侧用 root 建好并 chcon，游戏侧只读写文件，不 mkdir
//
// 值里的转义：\\ → \，\n → 换行，\r → 回车，\= → =
// =====================================================================
#pragma once
#include <cstdint>

namespace bridge {

struct UiConfig {
    int   version       = -1;
    bool  enableReplace = false;
    float modelScale    = 1.0f;
    char  modelPath[512] = {0};
    char  activeModel[160] = {0};
    bool  showBounds    = false;
    float offset[3]     = {0.0f, 0.0f, 0.0f};
    bool  cmdRescan     = false;
    bool  cmdReapply    = false;
    bool  valid         = false;   // 文件存在且至少解析出一个键
};

struct GameStatus {
    const char* gameState   = "unknown";  // unknown/detached/attached/loading/active/error
    const char* statusMessage = "";
    const char* activeModel = "";
    int   smrTotal        = 0;
    int   smrReplaced     = 0;
    int   boneCount       = 0;
    int   vertexCount     = 0;
    float replaceProgress = 0.0f;
    int   lastResult      = 0;
    int   lastErrorCode   = 0;
    const char* lastError = "";
};

// 读 ui.conf。返回 true 表示 version 变了（内容有更新），此时 out 已填好。
// 内部节流 200ms，可以每帧调。
bool poll(UiConfig& out);

// 写 status.conf（tmp + rename 原子写）。内部节流到 200ms，
// force=true 时无视节流立即写。
void writeStatus(const GameStatus& st, bool force = false);

// 让下一次 poll 强制重读（比如重扫之后）
void invalidate();

// 诊断：目录是否可写（返回 false 说明 UI 侧没建好目录或权限不对）
bool dirReady();

// 注入日志回调（游戏侧把它接到写文件的日志上，logcat 会被游戏日志冲掉）
void setLogger(void (*fn)(const char* msg));

}  // namespace bridge
