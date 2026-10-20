# MmdRuntime UI ⇄ 游戏侧 bridge 协议

## 目录

```
/data/local/tmp/mmdrt/    ← 必须 chmod 777 + chcon u:object_r:app_data_file:s0
```

- UI 侧（libimgui_overlay.so，App 进程）负责**创建目录**（走 `su`，见
  `FloatingWindowService.ensureBridgeDirectory()`，已含 chcon）
- 游戏侧（libmmdruntime.so，游戏进程）**不要 mkdir**，只读写文件
- 两边都是原子写：先写 `.tmp` 再 `rename()`

## 文件

| 文件 | 方向 | 内容 |
|---|---|---|
| `ui.conf` | UI → 游戏 | UI 的开关/滑条/命令 |
| `status.conf` | 游戏 → UI | 游戏状态回写 |

## ui.conf（UI → 游戏，键=值文本）

```
# version=2
enable_replace=1        # 模型替换总开关 (0/1)
model_scale=1.0000      # 缩放
model_path=/sdcard/MT2/ModelMod/ngyu/【心动应援中】南宫羽   # 反斜杠/换行/= 已转义
show_bounds=0           # 骨骼辅助线
offset_x=0.0000         # 手动偏移（米）
offset_y=0.0000
offset_z=0.0000
active_model=<名>        # 当前选中模型名
cmd.rescan=1            # 一次性命令：重扫 SMR。游戏执行后 UI 侧会自动清掉
cmd.reapply=1           # 一次性命令：重新套用
version=12              # 每次 UI 写 +1，游戏侧记住上次看到的 version，变了才处理
write_time_ms=1491468734
```

转义规则：`\` → `\\`，`\n` → `\n`(转义)，`\r` → `\r`(转义)，`=` → `\=`。
游戏侧解析时先还原再比对。

## status.conf（游戏 → UI）

```
# version=2
game_state=attached     # unknown/detached/attached/loading/active/error
status_message=已绑定 il2cpp
game_version=2018.4.13f1
active_model=【心动应援中】南宫羽
last_result=1           # 0/1
last_error_code=0
last_error_message=
smr_total=53
smr_replaced=0
bone_count=224
vertex_count=21804
replace_progress=0.0000 # 0..1
```

UI 侧每帧 `stat()` 检查 mtime，变了才重读——游戏侧想推送就改写文件即可。
状态变化别写太快（UI 每帧只轮一次），建议游戏侧节流到 >=200ms。

## 游戏侧示例（C++，直接可用）

```cpp
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

static int g_lastUiVersion = -1;
static int64_t g_lastPollMs = 0;

// 每帧（或每 200ms）调用
void BridgePollUi() {
    int64_t now = clock_ms();
    if (now - g_lastPollMs < 200) return;
    g_lastPollMs = now;
    FILE* f = fopen("/data/local/tmp/mmdrt/ui.conf", "r");
    if (!f) return;
    int version = -1;
    bool enableReplace = false;
    float modelScale = 1.0f;
    char modelPath[512] = {0};
    bool rescan = false, reapply = false;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[1900];
        if (sscanf(line, "%63[^=]=%1899[^\r\n]", key, value) != 2) continue;
        if (!strcmp(key, "version")) version = atoi(value);
        else if (!strcmp(key, "enable_replace")) enableReplace = atoi(value);
        else if (!strcmp(key, "model_scale")) modelScale = (float)atof(value);
        else if (!strcmp(key, "model_path")) snprintf(modelPath, sizeof(modelPath), "%s", value);
        else if (!strcmp(key, "cmd.rescan")) rescan = atoi(value);
        else if (!strcmp(key, "cmd.reapply")) reapply = atoi(value);
    }
    fclose(f);
    if (version == g_lastUiVersion) return;  // 没变，跳过
    g_lastUiVersion = version;
    // TODO: 把 enableReplace/modelScale/modelPath 应用到游戏
    // TODO: rescan → 重扫 SMR；reapply → 重新套用
}

// 状态回写（原子写）
void BridgeWriteStatus(const char* gameState, const char* message,
                       const char* modelName, int smrTotal, int smrReplaced,
                       int boneCount, int vertexCount, float progress) {
    static char tmp[256], final_[256];
    snprintf(tmp, sizeof(tmp), "/data/local/tmp/mmdrt/status.conf.tmp");
    snprintf(final_, sizeof(final_), "/data/local/tmp/mmdrt/status.conf");
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# version=2\n");
    fprintf(f, "game_state=%s\n", gameState);
    fprintf(f, "status_message=%s\n", message);
    fprintf(f, "active_model=%s\n", modelName);
    fprintf(f, "smr_total=%d\n", smrTotal);
    fprintf(f, "smr_replaced=%d\n", smrReplaced);
    fprintf(f, "bone_count=%d\n", boneCount);
    fprintf(f, "vertex_count=%d\n", vertexCount);
    fprintf(f, "replace_progress=%.4f\n", progress);
    fprintf(f, "last_result=1\n");
    fclose(f);
    rename(tmp, final_);
}
```

注意：游戏进程 uid 没有 `/data/local/tmp` 的父目录写权限也没关系——目录由
UI 侧建好（root），游戏只写文件本身（777 目录 + app_data_file 标签已验证可写）。
