// =====================================================================
// crash_report.h —— 自己的 SIGSEGV 现场记录
//
// 为什么需要：游戏自带字节跳动的 libnpth_wrapper 会 hook 信号链
// （dmesg 里刷 process_monitor-sig_record: signal_record_hook_handle），
// crash_dump64 接不到，/data/tombstones 一片空白，exit-info 只有
// reason=2 status=11。想知道崩在哪，只能自己接信号。
//
// 只做三件事：记 PC / LR / SP、把地址翻译成「哪个 so + 偏移」、FP 链走几步。
// 打完恢复默认处理再把自己打死，不掩盖真实崩溃。
// =====================================================================
#pragma once

namespace crash_report {

// 必须在 JNI_OnLoad 里尽早调。logPath 传游戏私有目录里的日志文件。
void install(const char* logPath);

// 给「当前线程」装信号备用栈（sigaltstack 是线程级的，不装的话栈坏了就进不了 handler）。
// tick 第一次跑的时候在 UnityMain 上调一次。
void prepare_thread();

// 定期检查处理器还在不在，被顶掉就重装并记日志。
// build-125 实测：手动给进程发 SIGSEGV，进程既不死也不留任何记录——
// 说明装的处理器已经被别人（libnpth_wrapper）接管了。
void check();

}  // namespace crash_report
