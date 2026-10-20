# MmdRuntime 两个 so 的联动协议（mmd_get_api）

架构：**两个 so 注入同一个游戏进程，互相直调**。

```
com.hermes.h1game 进程
├── libmmdruntime.so      MMD 逻辑（PMX / 骨骼 / Mesh / SMR / 扩容 hook）
│     └─ 导出 const MmdApi* mmd_get_api(void)
└── libimgui_overlay.so   ImGui UI，hook eglSwapBuffers 画在游戏 GL 帧上
      └─ dlopen(RTLD_NOLOAD) + dlsym("mmd_get_api") → 直接调
```

文件通道（`/data/local/tmp/mmdrt/ui.conf`、`status.conf`）**已废弃删除**。

---

## 1. 头文件

`native/public/mmd_api.h` —— 纯 C，两边共用，不依赖 STL，不依赖 il2cpp。

```c
#define MMD_API_VERSION 1
#define MMD_BUILD_TAG   "v1.2-api"
```

`MmdStatus` / `MmdApi` 都只允许**在末尾追加字段**；`apiVersion` 变了就是 ABI 变了。

## 2. UI 侧怎么拿到 API

```cpp
#include "public/mmd_api.h"
#include <dlfcn.h>

typedef const MmdApi* (*GetApiFn)(void);

static const MmdApi* g_api = nullptr;

static bool bind_mmd_api() {
    if (g_api) return true;

    // 关键：RTLD_NOLOAD。libmmdruntime.so 已经被 HookEntry System.load 过了，
    // 这里只要句柄，不要重复初始化。加载顺序必须是「先 mmdruntime 后 imgui_overlay」。
    void* h = dlopen("libmmdruntime.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    auto fn = (GetApiFn)dlsym(h, "mmd_get_api");
    if (!fn) return false;

    const MmdApi* api = fn();
    if (!api) return false;
    if (api->apiVersion != MMD_API_VERSION || api->structSize != (int)sizeof(MmdApi)) {
        LOGE("API mismatch: so=%d hdr=%d size=%d/%zu",
             api->apiVersion, MMD_API_VERSION, api->structSize, sizeof(MmdApi));
        return false;
    }
    g_api = api;
    return true;
}
```

拿不到就是加载顺序错了，或者 mmdruntime 自己没起来（看 `[API] mmd_get_api()` 有没有打）。同进程里不要 `dlopen` 不带 `RTLD_NOLOAD` 的路径写法，会拿到另一个句柄。

## 3. 状态

```c
MmdStatus st;
g_api->get_status(&st);
```

| 字段 | 含义 |
|---|---|
| `stage` | tick 状态机：0 WAIT / 1 BIND / 2 CLASSES / 3 SCAN / 4 IDLE / 5 LOAD / 6 ACTIVE |
| `gameState` | 给 UI 显示的粗状态：0 UNKNOWN / 1 DETACHED（il2cpp 没绑上）/ 2 ATTACHED / 3 LOADING / 4 ACTIVE / 5 ERROR |
| `smrTotal` | 扫到多少个 SkinnedMeshRenderer |
| `smrReplaced` | 0/1，模型有没有真的套上去 |
| `boneCount` / `vertexCount` / `triCount` | 建出来的骨骼数 / 顶点 / 三角面 |
| `progress` | 0..100 |
| `lastResult` | 1 成功 / 0 失败 / -1 还没跑过 |
| `enableReplace` | UI 设的总开关回读 |
| `modelScale` | 预留，缩放还没接进加载流程 |
| `statusVersion` | **内容变了才 +1**，UI 拿它判断要不要重画（别每帧比字符串） |
| `uptimeMs` | so 加载到现在的毫秒数（不参与 statusVersion 比较） |
| `statusMessage` | 人读的一行状态，直接显示 |
| `activeModel` | UI 选的模型目录名 |
| `lastError` | 上一次失败原因，`lastResult==0` 时显示 |
| `pmxPath` | 上一次实际加载的 pmx 绝对路径 |
| `logPath` | 日志文件绝对路径（游戏私有目录，UI 有 root 才能读） |

状态是**每帧末尾刷新的快照**，`get_status` 随时可调、不阻塞主线程。

## 4. 命令

| 调用 | 语义 |
|---|---|
| `set_enable(1)` | 打开总开关 → tick 在 IDLE 看到 `enable && !load.ok` 就自动去加载 |
| `set_enable(0)` | 关掉 → ACTIVE 状态回到 IDLE（**模型还原还没接，屏幕上仍是替换后的样子**） |
| `set_model("南宫羽")` | 选模型：游戏私有目录 `models/<名字>/` 下递归找第一个 `.pmx`；传 `""` 表示用 `models/` 下任意一个 |
| `set_scale(f)` | 记下来，暂未生效 |
| `cmd_reapply()` | 强制重新套用一次（重新走 8 步加载） |
| `cmd_rescan()` | 清空 SMR 列表重新扫（换场景/换角色后用） |
| `cmd_unload()` | 请求还原（当前是 stub，只把状态打回 IDLE） |
| `log("...")` | UI 想留痕的写进游戏侧同一份日志，前缀 `[UI]` |

命令是**置位式**：setter 只写标志，真正动作在下一次 `native_tick`（游戏主线程）里做，做完清标志。所以 UI 侧连点两次不会跑两遍。

## 5. 线程

- `native_tick` 跑在游戏 **Java 主线程**（HookEntry 的 16ms Handler）
- `eglSwapBuffers` hook 也在**渲染线程**（Unity 上就是主线程）
- 所有 setter/cmd 内部有 `pthread_mutex`，从任意线程调都安全
- `get_status` 拿的是快照，不和 tick 抢锁

## 6. 日志

```c
g_api->log("menu opened");
const char* path = g_api->get_log_path();   // /data/data/com.hermes.h1game/files/mmdruntime.log
const char* err  = g_api->get_last_error();
```

游戏进程写不进 `/sdcard`（targetSdk 30 分区存储），日志落在游戏私有目录，靠 Termux 侧 `~/mmdbg-sync.sh` 那套脚本拷出来。

## 7. 空实现（API 存在但还没接）

- `modelScale` 只存不用
- `cmd_unload` / `set_enable(0)` 不还原模型
- 贴图、材质还没进加载流程（`game/loader.cpp` 只做了骨骼 + mesh）

## 8. 验证方法

游戏启动后，UI so 加载时调一次 `mmd_get_api()`，日志里会出现：

```
[API] mmd_get_api() v1 size=... tag=v1.2-api
```

UI 侧每秒打一行：

```
[UI] stage=4 gameState=2 smr=68 msg='ready (68 SMR)'
```

两边都出现就说明直连通了。
