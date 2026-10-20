// =====================================================================
// logsink.h —— 统一日志汇
//
// 为什么要有这个：模块里原来各自 __android_log_print，而 logcat 在
// 游戏启动瞬间会被 logd 限流吃掉（一次 PMX dump 几百行就触发）。
// 结果就是最关键的 [SKEL] / [LOADER] 报错一行都看不到，只能靠猜。
//
// 现在所有模块统一走 logsink::Write，由 module.cpp 注入一个写文件的实现。
// =====================================================================
#pragma once

namespace logsink {

using Fn = void (*)(const char* fmt, ...);

// 由 module.cpp 在 JNI_OnLoad 里注入（指向那个带时间戳写文件的 L()）
void Set(Fn fn);

// 没注入之前静默丢弃，绝不崩
void Write(const char* fmt, ...);

bool ready();

}  // namespace logsink
