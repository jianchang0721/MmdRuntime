// =====================================================================
// stealth.h —— 让反作弊看不见我们
//
// 威胁模型（这套东西是怎么暴露的）：
//   1. /proc/self/maps 里出现 /data/data/com.hermes.h1game/files/libmmdruntime_1.so
//      —— 最常见、最致命。反作弊扫一眼就完了：游戏自己的 so 全在
//         /data/app/.../lib/arm64/ 下，我们这条路径一看就是外挂
//   2. /proc/self/maps 里出现 libimgui_overlay_1.so 同理
//   3. /proc/self/task/<tid>/maps 是同一份数据的另一个入口，也得过
//   4. /proc/self/status 的 TracerPid —— 挂调试器时非 0
//   5. dl_iterate_phdr 枚举出来的模块列表 —— 这个要 hook linker 内部，
//      代价大，暂不做（真要做得改 rtld 的 soinfo 链，风险比收益高）
//
// 做法：hook open / openat / fopen / fopen64，把要读 /proc 的请求
// 换成一个 memfd —— 内容是原文件删掉我们那几行之后的版本。
// 拿到的是真文件描述符，read/fstat/lseek/fgets 全都正常工作，
// 反作弊那边看不出区别（除了行数少了）。
//
// 纪律：
//   · hook 内部绝不调用本项目的日志函数（它自己 fopen，会递归）
//   · 只拦 O_RDONLY，写操作一律放行
//   · 任何一步失败就退回原始函数，宁可暴露也不能把游戏搞崩
// =====================================================================
#pragma once

namespace stealth {

// 装 hook。幂等。要在 so 加载后尽早调（最好在 JNI_OnLoad 里第一件事）。
void Install();

// 追加一个要隐藏的子串（比如别的模块的 so 名）。Install 之前调也行。
void AddHiddenMark(const char* mark);

bool installed();

// 自己这个 so 的加载路径（诊断用）
const char* self_path();

// 各个 hook 装没装上的摘要（一行）。
// 存在的意义：stealth 自己的 __android_log_print 在某些时机会被 logd 吃掉，
// 交给游戏侧的 L() 写文件才看得到。
const char* hook_summary();

// 真实 maps 里我们自己的映射段数（走原始 fd，绕过自己的 hook）。
// 正常是 2~4 段（代码 / 只读 / 数据），**非 0 才是对的**。
int real_mappings();

// 通过普通 fopen 读 maps 时还能看到我们几次。
// 这个才是隐身是否生效的证据：装好 hook 后应该返回 0。
int filtered_hits();

// /proc/self/status 的 TracerPid 非 0 = 有人在 ptrace 我们
bool is_traced();

}  // namespace stealth
