// =====================================================================
// bridge.cpp
// =====================================================================
#include "bridge.h"

#include <android/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#define BTAG "MmdBridge"
#define BLOG(...) __android_log_print(ANDROID_LOG_INFO, BTAG, __VA_ARGS__)

namespace bridge {

namespace {

const char* kDir        = "/data/local/tmp/mmdrt";
const char* kUiConf     = "/data/local/tmp/mmdrt/ui.conf";
const char* kStatusConf = "/data/local/tmp/mmdrt/status.conf";
const char* kStatusTmp  = "/data/local/tmp/mmdrt/status.conf.tmp";

const int64_t kPollIntervalMs  = 200;
const int64_t kWriteIntervalMs = 200;

int     g_lastVersion = -1;
int64_t g_lastPollMs  = 0;
int64_t g_lastWriteMs = 0;
int     g_failCount   = 0;

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 反转义：\\ → \，\n → 换行，\r → 回车，\= → =
std::string unescape(const char* s) {
    std::string out;
    out.reserve(strlen(s));
    for (const char* p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '=': out.push_back('='); break;
                default:  out.push_back(*p); break;
            }
        } else {
            out.push_back(*p);
        }
    }
    return out;
}

// 转义（写 status 用；名字里可能有 = 或者反斜杠）
std::string escape(const char* s) {
    std::string out;
    if (!s) return out;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case '=':  out += "\\="; break;
            default:   out.push_back(*p); break;
        }
    }
    return out;
}

void copy_to(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    size_t n = src.size();
    if (n >= cap) n = cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = 0;
}

bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

bool dirReady() {
    struct stat st;
    if (stat(kDir, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    return access(kDir, W_OK) == 0;
}

bool poll(UiConfig& out) {
    int64_t t = now_ms();
    if (t - g_lastPollMs < kPollIntervalMs) return false;
    g_lastPollMs = t;

    FILE* f = fopen(kUiConf, "r");
    if (!f) {
        if (g_failCount < 3) {
            g_failCount++;
            BLOG("[BRIDGE] cannot open %s (UI 侧还没建好目录?)", kUiConf);
        }
        return false;
    }
    g_failCount = 0;

    UiConfig cfg;
    int parsed = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        // 去掉行尾
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len == 0 || line[0] == '#') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* rawValue = eq + 1;
        std::string value = unescape(rawValue);
        parsed++;

        if (!strcmp(key, "version"))                 cfg.version = atoi(value.c_str());
        else if (!strcmp(key, "enable_replace"))     cfg.enableReplace = atoi(value.c_str()) != 0;
        else if (!strcmp(key, "model_scale"))        cfg.modelScale = (float)atof(value.c_str());
        else if (!strcmp(key, "model_path"))         copy_to(cfg.modelPath, sizeof(cfg.modelPath), value);
        else if (!strcmp(key, "active_model"))       copy_to(cfg.activeModel, sizeof(cfg.activeModel), value);
        else if (!strcmp(key, "show_bounds"))        cfg.showBounds = atoi(value.c_str()) != 0;
        else if (!strcmp(key, "offset_x"))           cfg.offset[0] = (float)atof(value.c_str());
        else if (!strcmp(key, "offset_y"))           cfg.offset[1] = (float)atof(value.c_str());
        else if (!strcmp(key, "offset_z"))           cfg.offset[2] = (float)atof(value.c_str());
        else if (!strcmp(key, "cmd.rescan"))         cfg.cmdRescan = atoi(value.c_str()) != 0;
        else if (!strcmp(key, "cmd.reapply"))        cfg.cmdReapply = atoi(value.c_str()) != 0;
    }
    fclose(f);

    cfg.valid = (parsed > 0);
    if (!cfg.valid) return false;
    if (cfg.version == g_lastVersion) return false;   // 没变

    g_lastVersion = cfg.version;
    out = cfg;

    BLOG("[BRIDGE] ui.conf version=%d enable=%d scale=%.2f model='%s' path='%s' rescan=%d reapply=%d",
         cfg.version, (int)cfg.enableReplace, cfg.modelScale,
         cfg.activeModel, cfg.modelPath, (int)cfg.cmdRescan, (int)cfg.cmdReapply);
    return true;
}

void writeStatus(const GameStatus& st, bool force) {
    int64_t t = now_ms();
    if (!force && t - g_lastWriteMs < kWriteIntervalMs) return;
    g_lastWriteMs = t;

    FILE* f = fopen(kStatusTmp, "w");
    if (!f) {
        if (g_failCount < 3) {
            g_failCount++;
            BLOG("[BRIDGE] cannot write %s (目录权限?)", kStatusTmp);
        }
        return;
    }

    fprintf(f, "# version=2\n");
    fprintf(f, "game_state=%s\n", st.gameState ? st.gameState : "unknown");
    fprintf(f, "status_message=%s\n", escape(st.statusMessage).c_str());
    fprintf(f, "active_model=%s\n", escape(st.activeModel).c_str());
    fprintf(f, "smr_total=%d\n", st.smrTotal);
    fprintf(f, "smr_replaced=%d\n", st.smrReplaced);
    fprintf(f, "bone_count=%d\n", st.boneCount);
    fprintf(f, "vertex_count=%d\n", st.vertexCount);
    fprintf(f, "replace_progress=%.4f\n", st.replaceProgress);
    fprintf(f, "last_result=%d\n", st.lastResult);
    fprintf(f, "last_error_code=%d\n", st.lastErrorCode);
    fprintf(f, "last_error=%s\n", escape(st.lastError).c_str());
    fprintf(f, "write_time_ms=%lld\n", (long long)(t));
    fclose(f);

    if (rename(kStatusTmp, kStatusConf) != 0) {
        BLOG("[BRIDGE] rename to status.conf failed");
        unlink(kStatusTmp);
    }
}

void invalidate() { g_lastVersion = -1; }

}  // namespace bridge
