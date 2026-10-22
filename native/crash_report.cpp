// =====================================================================
// crash_report.cpp
//
// 注意：这里所有文件操作都走 syscall(__NR_*) 直连，不走 libc 包装。
// 原因有两个：
//   1. stealth 自己 hook 了 open/openat/fopen，信号处理链里再进那套逻辑就是自杀
//   2. 崩溃瞬间 stdio 的状态是未定义的，syscall 最稳
// =====================================================================
#include "crash_report.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

namespace crash_report {

namespace {

struct MapEnt {
    uintptr_t start;
    uintptr_t end;
    uintptr_t off;
    char      path[112];
};

// 这台机子 /proc/self/maps 有 6000+ 行，2048 条会截断——截断在外面
// 就等于 pc 查不到模块。表开够 8192 条，读缓冲 2MB。
MapEnt  g_maps[8192];
int     g_nmaps = 0;
int     g_fd    = -1;

static char s_mapsBuf[2 * 1024 * 1024];

char    g_altstack[64 * 1024] __attribute__((aligned(16)));

void raw_write(const char* s, int n) {
    if (g_fd >= 0 && n > 0) (void)syscall(__NR_write, g_fd, s, (size_t)n);
}

const char* base_name(const char* p) {
    const char* b = p;
    for (const char* q = p; *q; ++q) if (*q == '/') b = q + 1;
    return b;
}

void load_maps() {
    int fd = (int)syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) return;

    char* buf = s_mapsBuf;
    const size_t bufSize = sizeof(s_mapsBuf);
    g_nmaps = 0;
    ssize_t total = 0;
    for (;;) {
        if (total >= (ssize_t)bufSize - 1) break;
        ssize_t n = syscall(__NR_read, fd, buf + total, bufSize - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
    }
    syscall(__NR_close, fd);
    buf[total] = 0;

    char* p = buf;
    while (*p && g_nmaps < (int)(sizeof(g_maps) / sizeof(g_maps[0]))) {
        char* e = strchr(p, '\n');
        if (!e) break;
        *e = 0;

        unsigned long start = 0, end = 0, off = 0;
        if (sscanf(p, "%lx-%lx %*s %lx", &start, &end, &off) == 3) {
            MapEnt& m = g_maps[g_nmaps];
            m.start = start;
            m.end   = end;
            m.off   = off;
            char* slash = strchr(p, '/');
            if (slash) {
                strncpy(m.path, slash, sizeof(m.path) - 1);
                m.path[sizeof(m.path) - 1] = 0;
            } else {
                // 匿名映射用权限+偏移当标识
                m.path[0] = 0;
            }
            g_nmaps++;
        }
        p = e + 1;
    }
}

const MapEnt* find_map(uintptr_t a) {
    for (int i = 0; i < g_nmaps; i++)
        if (a >= g_maps[i].start && a < g_maps[i].end) return &g_maps[i];
    return nullptr;
}

void fmt_addr(char* out, size_t n, uintptr_t a) {
    const MapEnt* m = find_map(a);
    if (m && m->path[0]) {
        snprintf(out, n, "%s+0x%lx", base_name(m->path),
                 (unsigned long)(a - m->start + m->off));
    } else if (m) {
        snprintf(out, n, "anon+0x%lx", (unsigned long)(a - m->start));
    } else {
        snprintf(out, n, "?");
    }
}

void handler(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc = (ucontext_t*)uc_;

    // 现场重读一遍 maps：装上之后新 mmap 的库也能查到
    load_maps();

    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    uintptr_t far = (uintptr_t)si->si_addr;

    char a1[160], a2[160], a3[160];
    fmt_addr(a1, sizeof a1, pc);
    fmt_addr(a2, sizeof a2, lr);
    fmt_addr(a3, sizeof a3, far);

    char line[768];
    int len = snprintf(line, sizeof line,
        "[CRASH] sig=%d code=%d tid=%d fault=%p(%s) pc=%p(%s) lr=%p(%s) sp=%p fp=%p\n",
        sig, si->si_code, (int)syscall(__NR_gettid),
        (void*)far, a3, (void*)pc, a1, (void*)lr, a2, (void*)sp, (void*)fp);
    raw_write(line, len);

    // FP 链：Unity 基本带帧指针，能走几步算几步
    for (int i = 0; i < 12 && fp > 0x1000 && (fp & 7) == 0; i++) {
        uintptr_t* f = (uintptr_t*)fp;
        if (!f) break;
        uintptr_t ret = f[1];
        uintptr_t nfp = f[0];
        char af[160];
        fmt_addr(af, sizeof af, ret);
        len = snprintf(line, sizeof line, "[CRASH]   fp#%d ret=%p(%s)\n", i, (void*)ret, af);
        raw_write(line, len);
        if (nfp <= fp) break;
        fp = nfp;
    }

    // 恢复默认处理再把自己打死，不掩盖真实崩溃
    signal(sig, SIG_DFL);
    (void)syscall(__NR_tgkill, (int)syscall(__NR_getpid), (int)syscall(__NR_gettid), sig);
    // aarch64 没有 pause，用 getpid 空转兜底（tgkill 正常时根本走不到这）
    for (;;) (void)syscall(__NR_getpid);
}

}  // namespace

void install(const char* logPath) {
    if (g_fd >= 0) return;

    g_fd = (int)syscall(__NR_openat, AT_FDCWD, logPath,
                        O_WRONLY | O_APPEND | O_CREAT, 0666);
    load_maps();

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp    = g_altstack;
    ss.ss_size  = sizeof(g_altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);

    char line[256];
    int len = snprintf(line, sizeof line, "[CRASH] handler installed fd=%d maps=%d\n",
                       g_fd, g_nmaps);
    raw_write(line, len);
}

}  // namespace crash_report
