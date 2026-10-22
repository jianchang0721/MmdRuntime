// =====================================================================
// mmd_api.h —— libmmdruntime.so 对外导出（纯 C ABI，供同进程的 UI so 调用）
//
// 谁用：libimgui_overlay.so（同一个游戏进程，hook eglSwapBuffers 画 ImGui）
//
// 怎么拿：
//     uintptr_t base = maps_base("libmmdruntime.so");
//     uintptr_t sym  = elf_sym(base, "mmd_get_api");
//     typedef const MmdApi* (*GetApiFn)(void);
//     const MmdApi* api = ((GetApiFn)sym)();
//     if (!api || api->apiVersion != MMD_API_VERSION) { 放弃 }
//
// 纪律：
//   - 字符串一律固定 char 数组，不跨 so 传 std::string / 不传堆指针
//   - setter / cmd 任意线程可调（内部加锁），真正的动作在游戏主线程的 tick 里做
//   - get_status 任意线程可调，填的是最近一帧的快照（不会阻塞主线程干活）
//   - 两个 so 的版本要对上：apiVersion 不等就别调，先 log 报出来
// =====================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MMD_API_VERSION 1
#define MMD_BUILD_TAG   "v1.2-api"

// tick 状态机
typedef enum {
    MMD_ST_WAIT    = 0,
    MMD_ST_BIND    = 1,
    MMD_ST_CLASSES = 2,
    MMD_ST_SCAN    = 3,
    MMD_ST_IDLE    = 4,
    MMD_ST_LOAD    = 5,
    MMD_ST_ACTIVE  = 6,
    MMD_ST_DORMANT = 7   // 睡死：不管游戏进多少场景都不碰 Unity，等 UI 发 cmd_init
} MmdStage;

// 给 UI 显示用的粗状态
typedef enum {
    MMD_GS_UNKNOWN  = 0,
    MMD_GS_DETACHED = 1,   // il2cpp 还没绑上
    MMD_GS_ATTACHED = 2,   // 绑上了，待命
    MMD_GS_LOADING  = 3,
    MMD_GS_ACTIVE   = 4,
    MMD_GS_ERROR    = 5
} MmdGameState;

// 状态快照。字段只加在末尾，apiVersion 变了才允许改前面。
typedef struct MmdStatus {
    int  apiVersion;
    int  stage;          // MmdStage
    int  gameState;      // MmdGameState
    int  smrTotal;       // 扫到多少个 SkinnedMeshRenderer
    int  smrReplaced;    // 0 / 1
    int  boneCount;      // 建出来多少根骨骼
    int  vertexCount;
    int  triCount;
    int  progress;       // 0..100
    int  lastResult;     // 1 成功 / 0 失败 / -1 还没跑过
    int  enableReplace;  // UI 设置的总开关
    float modelScale;    // 预留，缩放还没接
    int  statusVersion;  // 内容变了才 +1（UI 拿它判断要不要重画/记日志）
    long long uptimeMs;  // so 加载到现在的毫秒数
    char statusMessage[256];
    char activeModel[160];   // UI 选的模型目录名
    char lastError[192];
    char pmxPath[512];       // 上一次实际加载的 pmx 绝对路径
    char logPath[512];       // 日志文件绝对路径（游戏私有目录里那个）
} MmdStatus;

// 函数表。只加在末尾。
typedef struct MmdApi {
    int  structSize;    // sizeof(MmdApi)，防两边头文件不一致
    int  apiVersion;
    const char* buildTag;

    void (*get_status)(MmdStatus* out);
    void (*set_enable)(int on);
    void (*set_model)(const char* modelDirName);   // 游戏私有目录 models/<名字>
    void (*set_scale)(float scale);
    void (*cmd_rescan)(void);
    void (*cmd_reapply)(void);
    void (*cmd_unload)(void);
    void (*log)(const char* msg);                  // UI 侧往游戏日志里写一行
    const char* (*get_log_path)(void);
    const char* (*get_last_error)(void);

    // 列出游戏私有目录 models/ 下的模型（子目录名或 .pmx 文件名），
    // 每行一个写进 out，返回条数。游戏进程才有权限扫自己的私有目录，
    // UI 侧在 targetSdk 30 分区存储下读不到 /sdcard。
    int  (*list_models)(char* out, int outSize);
    // 同签名，但返回的是当前生效的模型名（空串 = 还没加载）
    const char* (*active_model)(void);
    // 把 PMX 重新解析一遍（换模型/改了文件用），下次 load 时生效
    void (*cmd_reparse)(void);

    // 自由视角摄像机
    int  (*camera_enable)(int on);    // 返回 1 = 已开启
    int  (*camera_is_enabled)(void);
    void (*camera_reset)(void);
    // 喂触摸。返回 1 = 这个触摸归摄像机，别给游戏
    int  (*camera_touch)(int action, int pointerId, float x, float y, float vw, float vh);
    // 拉近拉远（滑条用，单位世界单位）
    void (*camera_zoom)(float d);

    // 在 UnityMain 线程上跑一帧。UI so 的 eglSwapBuffers hook 每帧调。
    //
    // 为什么必需：nativeTick 跑在 Java 主线程，而 Unity 的 native
    // 对象创建（Mesh..ctor 等）只能在 UnityMain 线程做。
    // 实测：Java 主线程 tid=724，UnityMain tid=1428，两者不同；
    // GameObject..ctor 能过，Mesh..ctor 直接静默退出。
    void (*render_tick)(void);

    // 初始化：绑 il2cpp + 找 Unity 类 + 扫 SMR。只做准备，不建模型。
    // 为什么要手动：自动加载会在玩家还没进场景时就去摸 Unity 对象，
    // 早版本 build-121 实测会把游戏画面搞卡。现在默认睡死（MMD_ST_DORMANT），
    // 必须玩家自己点「初始化」-> 「加载模型」。
    void (*cmd_init)(void);
} MmdApi;

// 唯一导出符号
const MmdApi* mmd_get_api(void);

#ifdef __cplusplus
}
#endif
