#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// =====================================================================
// UI(App 进程) <-> Game(游戏进程) 的中转协议
//
// 选目录：两边 uid 不同，需要一个 App 能 fopen 写、游戏也能 fopen 读、
// 且不用每次 su 的地方。/data/local/tmp 默认 0777，App 无需任何权限即可读写。
//
//   ui.conf       UI  -> Game   (开关/滑条/命令)
//   status.conf   Game -> UI    (状态回写)
//
// 协议是 key=value 纯文本逐行解析。脏检查用 version 字段：
// UI 每次写都 version+1，Game 记住上次读到的 version，变了才重新解析。
// =====================================================================

namespace BridgeConfig {

static const char* const BRIDGE_DIRECTORY = "/data/local/tmp/mmdrt";
static const char* const UI_FILE_NAME = "ui.conf";
static const char* const STATUS_FILE_NAME = "status.conf";
static const char* const BRIDGE_VERSION = "2";

inline std::string UiFilePath() { return std::string(BRIDGE_DIRECTORY) + "/" + UI_FILE_NAME; }
inline std::string StatusFilePath() { return std::string(BRIDGE_DIRECTORY) + "/" + STATUS_FILE_NAME; }

inline std::string TrimCopy(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

inline void WriteReplacing(std::string& subject, const std::string& search,
                           const std::string& replacement) {
    size_t position = 0;
    while ((position = subject.find(search, position)) != std::string::npos) {
        subject.replace(position, search.length(), replacement);
        position += replacement.length();
    }
}

inline std::string EscapeKeyValue(const std::string& value) {
    std::string out = value;
    WriteReplacing(out, "\\", "\\\\");
    WriteReplacing(out, "\n", "\\n");
    WriteReplacing(out, "\r", "\\r");
    WriteReplacing(out, "=", "\\=");
    return out;
}

inline std::string UnescapeKeyValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char next = value[++i];
            if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else if (next == '=') out += '=';
            else if (next == '\\') out += '\\';
            else out += next;
        } else {
            out += value[i];
        }
    }
    return out;
}

// =====================================================================
// 内存 KV 容器，带 mtime 脏检查
// =====================================================================
class KeyValueTable {
public:
    void Set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> guard(lock_);
        table_[key] = value;
    }
    void SetInt(const std::string& key, int value) { Set(key, std::to_string(value)); }
    void SetBool(const std::string& key, bool value) { Set(key, value ? "1" : "0"); }
    void SetFloat(const std::string& key, float value) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
        Set(key, std::string(buffer));
    }

    std::string GetString(const std::string& key, const std::string& fallback = "") const {
        std::lock_guard<std::mutex> guard(lock_);
        auto found = table_.find(key);
        return found == table_.end() ? fallback : found->second;
    }
    int GetInt(const std::string& key, int fallback = 0) const {
        std::string raw = GetString(key, "");
        return raw.empty() ? fallback : atoi(raw.c_str());
    }
    bool GetBool(const std::string& key, bool fallback = false) const {
        std::string raw = GetString(key, "");
        if (raw.empty()) return fallback;
        return raw == "1" || raw == "true" || raw == "yes";
    }
    float GetFloat(const std::string& key, float fallback = 0.0f) const {
        std::string raw = GetString(key, "");
        return raw.empty() ? fallback : static_cast<float>(atof(raw.c_str()));
    }

    bool LoadFromFile(const std::string& path) {
        FILE* handle = fopen(path.c_str(), "r");
        if (!handle) return false;
        std::lock_guard<std::mutex> guard(lock_);
        char lineBuffer[2048];
        while (fgets(lineBuffer, sizeof(lineBuffer), handle)) {
            std::string line(lineBuffer);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t separator = line.find('=');
            if (separator == std::string::npos) continue;
            table_[TrimCopy(line.substr(0, separator))] =
                UnescapeKeyValue(TrimCopy(line.substr(separator + 1)));
        }
        fclose(handle);
        return true;
    }

    // 原子写：临时文件 + rename，读侧永远不会看到半个文件
    bool SaveToFile(const std::string& path) const {
        std::lock_guard<std::mutex> guard(lock_);
        std::string temporary = path + ".tmp";
        FILE* handle = fopen(temporary.c_str(), "w");
        if (!handle) return false;
        fprintf(handle, "# version=%s\n", BRIDGE_VERSION);
        for (const auto& entry : table_) {
            fprintf(handle, "%s=%s\n", EscapeKeyValue(entry.first).c_str(),
                    EscapeKeyValue(entry.second).c_str());
        }
        fclose(handle);
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            ::remove(temporary.c_str());
            return false;
        }
        return true;
    }

    int GetBumpVersion() {
        std::lock_guard<std::mutex> guard(lock_);
        int nextVersion = atoi(table_["version"].c_str()) + 1;
        table_["version"] = std::to_string(nextVersion);
        return nextVersion;
    }

private:
    mutable std::mutex lock_;
    std::unordered_map<std::string, std::string> table_;
};

}  // namespace BridgeConfig
