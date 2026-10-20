# STEALTH —— 反作弊 / 反调试面

这个文件记录 `native/stealth.cpp` 干了什么、为什么这么干、以及**怎么关掉**。
调别的问题（模型不上屏、UI 不出来）时，第一件事就是把这一层摘掉再试。

---

## 威胁模型

游戏侧的反作弊扫这几样东西，按被发现的严重程度排：

| # | 检测点 | 我们能提供什么 |
|---|---|---|
| 1 | `/proc/self/maps` 里的 so 路径 | 游戏自己的 so 全在 `/data/app/.../lib/arm64/`，我们那条 `/data/data/com.hermes.h1game/files/xxx.so` 等于举牌 |
| 2 | `/proc/self/task/<tid>/maps` | 同一份数据的另一个入口 |
| 3 | `/proc/self/status` 的 `TracerPid` | 挂调试器时非 0 |
| 4 | `dl_iterate_phdr` 枚举 | 拿到 `dlpi_name` 和基址，比读 maps 更直接 |
| 5 | `__system_property_get("ro.debuggable")` | 判定「这机器能调」 |
| 6 | 文件系统扫私有目录 | 多出来的 `libxxx.so` |
| 7 | `ptrace` 附加 | 调试器/机器人 |

## 三层

### 第一层：过滤 `/proc`（stealth.cpp）

hook `open` / `openat` / `open64` / `fopen` / `fopen64`，命中
`/proc/self/{maps,smaps,map_files,task/*/maps,status}` 的**只读打开**时：

```
用原始 openat 打开真文件
  → 全读进内存
  → 删掉命中 mark 的行（maps）/ 把 TracerPid 改成 0（status）
  → 写进 memfd（匿名内存文件）
  → 返回 memfd 的 fd
```

调用方拿到的是**真 fd**，`read`/`fstat`/`lseek`/`fgets` 全部照常工作。

隐藏规则两条：

1. 含 mark：`mmdruntime`、`imgui_overlay`、`MmdRuntime`、`frida`、`gum-js-loop`、`gadget`
2. **兜底**：从 `/data/data/com.hermes.h1game/files/` mmap 的 `.so`，一律隐藏 —— 不管文件叫什么名字。游戏自己永远不会从私有目录加载 so，所以这条规则不会误伤。

### 第二层：`dl_iterate_phdr` + 系统属性

```
dl_iterate_phdr(cb, data)
   ↓ 被 shim 接住
遍历每个 so 的 dlpi_name，命中就 return 0（继续下一个）
   ↓
调用方的 cb 只会看到游戏自己的模块
```

`return 0` 是「跳过这一个、继续遍历」，不是终止。写错成 1 会让枚举提前结束，
反作弊一眼就能看出不对。

系统属性伪装表（`__system_property_get`）：

| 属性 | 返回 |
|---|---|
| `ro.debuggable` | `0` |
| `ro.secure` | `1` |
| `ro.build.tags` | `release-keys` |
| `ro.build.type` | `user` |
| `ro.build.selinux` | `1` |
| `service.adb.root` | `0` |
| `ro.kernel.qemu` | `0` |
| `init.svc.adbd` | `stopped` |

### 第三层：加载方式

```
HookEntry（Java）
  ├─ 解包 libmmdruntime.so → libil2cpp_ext_1.so → System.load
  │     （名字伪装成看着像游戏自己的扩展库）
  └─ 解包 libimgui_overlay.so → 交 nativeLoadUi()
        ├─ 读进内存
        ├─ memfd_create + 写入
        ├─ android_dlopen_ext(假名, RTLD_GLOBAL, {USE_LIBRARY_FD, fd})
        └─ unlink 落盘文件   ← 加载完盘上就没了
```

UI so 在 maps 里显示成：

```
/data/app/com.hermes.h1game/lib/arm64/libunity_gl.so
```

游戏里根本不存在的路径，但看着像游戏自己的库。

**副作用**：`android_dlopen_ext` 加载的 so，ART 不当它是模块，
`Java_com_xxx` 形式的 JNI 符号没人解析。所以 UI 的 JNI 入口全部收回到
`libmmdruntime.so`（`nativeLoadUi` / `nativeUiFonts` / `nativeUiTouch` / `nativeUiReady`），
so 之间用普通 C 符号（`ui_*`）dlsym 转发。

---

## 主动防护

tick 每 2 秒查一次 `/proc/self/status` 的 `TracerPid`。非 0 说明有东西在 ptrace 我们，
此时**冻结 `ST_LOAD`** —— 那一刻任何"正好发生"的行为都会变成关联证据。

```cpp
case ST_LOAD:
    if (g_traced) { "..."; g_stage = ST_IDLE; break; }
```

---

## 日志

启动时应该看到：

```
MmdStealth: install: self='/data/data/.../libil2cpp_ext_1.so' marks=8 openat=1 open=1 fopen=1 dliter=1 prop=1 rc=0 ok=1
[STEALTH] installed=1 self='...'
[STEALTH] real maps hits=0 OK(hidden)
[UI] loaded from memfd as '/data/app/com.hermes.h1game/lib/arm64/libunity_gl.so' handle=0x...
```

`hits` 是**走原始 fd** 数出来的真实 maps 命中行数，用来打自己脸。装了 hook 还非 0
说明过滤没生效（比如地址算错、路径不匹配）。

---

## 怎么关掉（排查必看）

按从便宜到贵的顺序砍：

1. **只关掉 memfd 加载**（UI 退回 System.load）
   `HookEntry.loadUiSo()` 里把 `nativeLoadUi(...)` 那段直接 `err = "skipped"`，
   就会走 `System.load` 兜底分支。

2. **只关掉 /proc 过滤**
   `module.cpp` 的 `JNI_OnLoad` 里注释掉 `stealth::Install();`。

3. **全关**
   上面两条一起，再把 `native/CMakeLists.txt` 里 `stealth.cpp` 去掉。

UI 那个 so 如果 memfd 这条路过不去，会自动退回 `System.load`，游戏还是能跑，
只是隐身效果打折。

---

## 安全性设计

- **hook 失败自动回滚**：装完立刻读一次 `/proc/self/cmdline` 自检，
  失败就 `DobbyDestroy` 全部卸掉。hook 链一坏，全进程文件 I/O 全废，
  那比被检测到严重得多。
- **只拦 `O_RDONLY`**：写操作全部放行。
- **hook 内不调项目日志**：它自己会 `fopen`，直接递归。用重入计数兜底。

---

## 已知缺口（没做的）

| 缺口 | 说明 |
|---|---|
| mmdruntime 本体还是落盘 | 改名了（`libil2cpp_ext_1.so`），但文件在。要彻底干净得再套一层 bootstrap |
| inline hook 可被校验 | `open` / `fopen` / `dl_iterate_phdr` 函数开头现在是 Dobby 的跳转指令。反作弊如果比对代码段能看出来 —— 所有 inline hook 方案的通病 |
| 直接 syscall 读 maps | 不走 libc 就拦不住 |
| `dl_iterate_phdr` 之外的枚举方式 | 比如直接遍历 linker 的 `soinfo` 链（需要找 `__dl__ZL10g_soinfo_list` 之类的符号） |

---

## 一个必须搞清楚的区别

我们拦的是**进程内部**的读取。

从 Termux root 里 `cat /proc/<pid>/maps` 照样能看到 `libil2cpp_ext_1.so` ——
那是从外部读，不经过进程里的 hook。这不代表隐身失效，反作弊是跑在游戏进程里的。
