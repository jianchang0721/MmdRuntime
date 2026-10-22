// =====================================================================
// stealth.cpp —— /proc 过滤（maps / status）
//
// 原理一句话：把「读 /proc/self/maps」变成「读一个删掉了我们那几行的副本」。
//
//   open("/proc/self/maps")  ──▶  被我们的 hook 截住
//                                     │
//                                     ├─ 用原始 openat 打开真文件
//                                     ├─ 全读进内存，剔除命中 mark 的行
//                                     ├─ 写进 memfd（匿名内存文件）
//                                     └─ 返回 memfd 的 fd
//   调用方拿到的是真 fd，read/fstat/lseek/fgets 全部照常工作。
//
// 为什么用 memfd 而不是临时文件：不落盘、不占 inode、进程退出自动释放、
// /proc/self/fd 里看到的是 /memfd:j (deleted)，比 /tmp/xxx 干净得多。
// =====================================================================
#include "logsink.h"
#include "stealth.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <link.h>   // dl_phdr_info / dl_iterate_phdr

#include <string>

#include "dobby/dobby.h"

#define STAG "MmdStealth"
#define SLOG(...) logsink::Write(__VA_ARGS__)

// 调试开关：0 = 完全不装任何 libc hook（对照组实验用）
// 用来判断贴图/文件读取的崩溃是不是 stealth 的 fopen hook 造成的
#ifndef MMD_STEALTH_ENABLE
#define MMD_STEALTH_ENABLE 1
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace stealth {
namespace {

// ---------------------------------------------------------------------
// 要隐藏的子串
// ---------------------------------------------------------------------
const int kMaxMarks = 8;
char g_marks[kMaxMarks][64];
int  g_markCount = 0;

bool         g_installed = false;
bool         g_hookOk    = false;
char         g_selfPath[512] = {0};
char         g_summary[256] = "not installed";

// 原始函数
int   (*g_open)(const char*, int, ...)            = nullptr;
int   (*g_openat)(int, const char*, int, ...)     = nullptr;
int   (*g_open64)(const char*, int, ...)          = nullptr;
FILE* (*g_fopen)(const char*, const char*)        = nullptr;
FILE* (*g_fopen64)(const char*, const char*)      = nullptr;
int   (*g_dl_iter)(int (*)(struct dl_phdr_info*, size_t, void*), void*) = nullptr;
int   (*g_prop_get)(const char* name, char* value) = nullptr;

// 重入保护：hook 内部我们会调原始函数，但保险起见还是拦一层
thread_local int t_depth = 0;

struct Guard {
    Guard()  { ++t_depth; }
    ~Guard() { --t_depth; }
};

// ---------------------------------------------------------------------
// 判断一个 map 行是不是我们
// ---------------------------------------------------------------------
// 游戏私有目录。游戏自己的 so 全在 /data/app/.../lib/arm64/ 下，
// 从 files/ 里 mmap 的 so 只可能是我们——不管它叫什么名字。
const char kPrivateDir[] = "/data/data/com.hermes.h1game/files/";

bool LineHasMark(const char* line, size_t n) {
    for (int i = 0; i < g_markCount; i++) {
        const char* m = g_marks[i];
        size_t ml = strlen(m);
        if (ml == 0 || ml > n) continue;
        // 朴素子串搜索，maps 行一般 100~200 字节，够快
        for (size_t k = 0; k + ml <= n; k++) {
            if (line[k] == m[0] && memcmp(line + k, m, ml) == 0) return true;
        }
    }

    // 兜底：私有目录里的 .so 映射，一律当自己人
    const size_t dl = sizeof(kPrivateDir) - 1;
    if (n > dl + 3) {
        if (memmem(line, n, kPrivateDir, dl) != nullptr &&
            memmem(line, n, ".so", 3) != nullptr) {
            return true;
        }
    }
    return false;
}

bool TextHasMark(const char* text) {
    for (int i = 0; i < g_markCount; i++) {
        if (g_marks[i][0] && strstr(text, g_marks[i])) return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// 路径分类：0 = 不管，1 = maps 系，2 = status 系
// ---------------------------------------------------------------------
int ClassifyPath(const char* path) {
    if (!path || path[0] != '/') return 0;
    if (strncmp(path, "/proc/", 6) != 0) return 0;

    const char* rest = path + 6;
    bool mine = false;

    if (strncmp(rest, "self/", 5) == 0) {
        mine = true;
    } else {
        // /proc/<pid>/...
        char digits[24];
        int n = 0;
        while (rest[n] >= '0' && rest[n] <= '9' && n < 20) {
            digits[n] = rest[n];
            n++;
        }
        if (n > 0 && rest[n] == '/') {
            digits[n] = 0;
            if (atoi(digits) == getpid()) mine = true;
        }
    }
    if (!mine) return 0;

    // maps / smaps / map_files/xxx 都算
    if (strstr(path, "/maps")) return 1;
    if (strstr(path, "/status")) return 2;
    return 0;
}

// ---------------------------------------------------------------------
// 内容过滤
// ---------------------------------------------------------------------
void FilterMaps(const char* in, size_t n, std::string& out) {
    out.clear();
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && in[e] != '\n') e++;
        const size_t lineLen = e - i;
        if (!LineHasMark(in + i, lineLen)) {
            out.append(in + i, lineLen);
            if (e < n) out.push_back('\n');
        }
        i = e + 1;
    }
}

void FilterStatus(const char* in, size_t n, std::string& out) {
    out.assign(in, n);
    // TracerPid:\t1234  → TracerPid:\t0
    size_t p = out.find("TracerPid:");
    if (p == std::string::npos) return;
    size_t q = p + 10;
    while (q < out.size() && (out[q] == ' ' || out[q] == '\t')) q++;
    size_t r = q;
    while (r < out.size() && out[r] >= '0' && out[r] <= '9') r++;
    if (r > q) out.replace(q, r - q, "0");
}

// ---------------------------------------------------------------------
// 造一个"过滤后的 fd"
// ---------------------------------------------------------------------
int MakeMemfd(const std::string& content) {
    int fd = (int)syscall(__NR_memfd_create, "j", (unsigned int)MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (!content.empty()) {
        size_t off = 0;
        while (off < content.size()) {
            ssize_t w = write(fd, content.data() + off, content.size() - off);
            if (w <= 0) { close(fd); return -1; }
            off += (size_t)w;
        }
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

int OpenFiltered(const char* path, int kind) {
    int real = g_openat ? g_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0) : -1;
    if (real < 0) return -1;

    std::string data;
    {
        char buf[16384];
        ssize_t r;
        while ((r = read(real, buf, sizeof(buf))) > 0) data.append(buf, (size_t)r);
    }
    close(real);

    std::string out;
    if (kind == 1) FilterMaps(data.data(), data.size(), out);
    else           FilterStatus(data.data(), data.size(), out);

    return MakeMemfd(out);
}

// ---------------------------------------------------------------------
// hook 本体
// ---------------------------------------------------------------------
int MyOpenat(int dirfd, const char* path, int flags, mode_t mode) {
    if (t_depth == 0 && (flags & O_ACCMODE) == O_RDONLY) {
        int kind = ClassifyPath(path);
        if (kind) {
            Guard guard;
            int fd = OpenFiltered(path, kind);
            if (fd >= 0) return fd;
        }
    }
    return g_openat ? g_openat(dirfd, path, flags, mode) : -1;
}

int MyOpen(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (t_depth == 0 && (flags & O_ACCMODE) == O_RDONLY) {
        int kind = ClassifyPath(path);
        if (kind) {
            Guard guard;
            int fd = OpenFiltered(path, kind);
            if (fd >= 0) return fd;
        }
    }
    if (g_open)   return g_open(path, flags, mode);
    if (g_open64) return g_open64(path, flags, mode);
    return -1;
}

FILE* MyFopen(const char* path, const char* mode) {
    if (t_depth == 0 && mode && mode[0] == 'r') {
        int kind = ClassifyPath(path);
        if (kind) {
            Guard guard;
            int fd = OpenFiltered(path, kind);
            if (fd >= 0) {
                FILE* f = fdopen(fd, mode);
                if (f) return f;
                close(fd);
            }
        }
    }
    return g_fopen ? g_fopen(path, mode) : nullptr;
}

FILE* MyFopen64(const char* path, const char* mode) {
    return MyFopen(path, mode);
}

// ---------------------------------------------------------------------
// 路径 / 模块名是不是我们自己的
// ---------------------------------------------------------------------
bool IsOursName(const char* s) {
    if (!s || !s[0]) return false;
    return LineHasMark(s, strlen(s));
}

// ---------------------------------------------------------------------
// dl_iterate_phdr 过滤
//
// 反作弊枚举模块的那条路在这里。原生 callback 只要看到我们的 soinfo
// 就能拿到基址，进而扫我们代码——比读 maps 更直接。
// 我们用一层 shim 把我们的模块从遍历结果里扣掉。
// ---------------------------------------------------------------------
struct IterCtx {
    int (*cb)(struct dl_phdr_info*, size_t, void*);
    void* ud;
};

int IterShim(struct dl_phdr_info* info, size_t size, void* p) {
    IterCtx* c = (IterCtx*)p;
    if (info && info->dlpi_name && IsOursName(info->dlpi_name)) {
        return 0;   // 返回 0 = 继续下一个，不终止遍历
    }
    return c->cb(info, size, c->ud);
}

int MyDlIterate(int (*cb)(struct dl_phdr_info*, size_t, void*), void* data) {
    if (!g_dl_iter || !cb) return -1;
    IterCtx c{cb, data};
    return g_dl_iter(IterShim, &c);
}

// ---------------------------------------------------------------------
// 系统属性伪装
//
// 反作弊很爱查这几条：ro.debuggable=1 直接判定可调试，
// ro.build.tags=test-keys 判定非官方包。改成官方机的值。
// ---------------------------------------------------------------------
struct PropOverride {
    const char* name;
    const char* value;
};

const PropOverride kPropOverrides[] = {
    {"ro.debuggable",            "0"},
    {"ro.secure",                "1"},
    {"ro.build.tags",            "release-keys"},
    {"ro.build.type",            "user"},
    {"ro.build.selinux",         "1"},
    {"service.adb.root",         "0"},
    {"ro.kernel.qemu",           "0"},
    {"init.svc.adbd",            "stopped"},
};

int MyPropGet(const char* name, char* value) {
    if (name) {
        for (const auto& o : kPropOverrides) {
            if (strcmp(name, o.name) == 0) {
                const size_t len = strlen(o.value);
                if (value) memcpy(value, o.value, len + 1);
                return (int)len;
            }
        }
    }
    return g_prop_get ? g_prop_get(name, value) : -1;
}

// 验证 trampoline 链路正常：读一个不在过滤名单里的 proc 文件
bool SelfTest() {
    FILE* f = nullptr;
    if (g_fopen) f = g_fopen("/proc/self/cmdline", "r");
    if (!f) {
        int fd = g_openat ? g_openat(AT_FDCWD, "/proc/self/cmdline", O_RDONLY | O_CLOEXEC, 0) : -1;
        if (fd < 0) return false;
        f = fdopen(fd, "r");
        if (!f) { close(fd); return false; }
    }
    char buf[64] = {0};
    bool ok = fgets(buf, sizeof(buf), f) != nullptr && buf[0] != 0;
    fclose(f);
    return ok;
}

void* Sym(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    void* libc = dlopen("libc.so", RTLD_NOW);
    return libc ? dlsym(libc, name) : nullptr;
}

}  // namespace

// =====================================================================
// 对外
// =====================================================================
void AddHiddenMark(const char* mark) {
    if (!mark || !mark[0]) return;
    if (g_markCount >= kMaxMarks) return;
    for (int i = 0; i < g_markCount; i++) {
        if (strcmp(g_marks[i], mark) == 0) return;   // 去重
    }
    snprintf(g_marks[g_markCount], sizeof(g_marks[0]), "%s", mark);
    g_markCount++;
}

void Install() {
#if MMD_STEALTH_ENABLE == 0
    SLOG("[STEALTH] disabled (MMD_STEALTH_ENABLE=0) —— 对照组：不装任何 libc hook");
    return;
#endif
    if (g_installed) return;
    g_installed = true;

    // 1) 先认自己
    Dl_info info;
    if (dladdr((void*)&Install, &info) && info.dli_fname) {
        snprintf(g_selfPath, sizeof(g_selfPath), "%s", info.dli_fname);
    }

    // 2) 默认要隐藏的标记
    AddHiddenMark("mmdruntime");
    AddHiddenMark("imgui_overlay");
    AddHiddenMark("MmdRuntime");
    if (g_selfPath[0]) {
        const char* base = strrchr(g_selfPath, '/');
        if (base && base[1]) AddHiddenMark(base + 1);
    }

    // 3) 顺手把 frida 痕迹也盖掉（如果进程里真有）
    //    只留特征明显的，别写 "gmain" 这种会误伤正常线程名的
    AddHiddenMark("frida");
    AddHiddenMark("gum-js-loop");
    AddHiddenMark("gadget");

    // 4) 装 hook
    void* aOpenat  = Sym("openat");
    void* aOpen    = Sym("open");
    void* aOpen64  = Sym("open64");
    void* aFopen   = Sym("fopen");
    void* aFopen64 = Sym("fopen64");
    void* aDlIter  = Sym("dl_iterate_phdr");
    void* aPropGet = Sym("__system_property_get");

    int rc = 0;
    if (aOpenat)  rc |= DobbyHook(aOpenat,  (void*)MyOpenat,  (void**)&g_openat);
    if (aOpen)    rc |= DobbyHook(aOpen,    (void*)MyOpen,    (void**)&g_open);
    if (aOpen64)  rc |= DobbyHook(aOpen64,  (void*)MyOpen,    (void**)&g_open64);
    // 2026-10-22 实测：fopen/fopen64 两条 hook 导致 fdsan abort
    // （fdsan: attempted to close file descriptor ... owned by FILE* ...），
    // 模型加载成功后进程立刻死。open/openat 已经覆盖 maps/status 的读取路径，
    // fopen 系暂时不装，等定位到 fdsan 冲突根因再恢复。
    // if (aFopen)   rc |= DobbyHook(aFopen,   (void*)MyFopen,   (void**)&g_fopen);
    // if (aFopen64) rc |= DobbyHook(aFopen64, (void*)MyFopen64, (void**)&g_fopen64);
    if (aDlIter)  rc |= DobbyHook(aDlIter,  (void*)MyDlIterate, (void**)&g_dl_iter);
    if (aPropGet) rc |= DobbyHook(aPropGet, (void*)MyPropGet,   (void**)&g_prop_get);

    g_hookOk = (g_openat != nullptr) || (g_open != nullptr) || (g_fopen != nullptr);

    // 5) 自检：装完立刻验证普通文件 I/O 没被搞坏。
    //    hook 链一坏，整个进程的文件读取全废，游戏当场就死——
    //    宁可卸载 hook 继续暴露，也不能把游戏搞崩。
    if (g_hookOk && !SelfTest()) {
        SLOG("selftest FAILED, rolling back hooks");
        if (aOpenat)  DobbyDestroy(aOpenat);
        if (aOpen)    DobbyDestroy(aOpen);
        if (aOpen64)  DobbyDestroy(aOpen64);
        if (aFopen)   DobbyDestroy(aFopen);
        if (aFopen64) DobbyDestroy(aFopen64);
        if (aDlIter)  DobbyDestroy(aDlIter);
        if (aPropGet) DobbyDestroy(aPropGet);
        g_open = nullptr; g_openat = nullptr; g_open64 = nullptr;
        g_fopen = nullptr; g_fopen64 = nullptr;
        g_dl_iter = nullptr; g_prop_get = nullptr;
        g_hookOk = false;
    }

    snprintf(g_summary, sizeof(g_summary),
             "openat=%d open=%d fopen=%d dliter=%d prop=%d rc=%d ok=%d marks=%d",
             aOpenat != nullptr, aOpen != nullptr, aFopen != nullptr,
             aDlIter != nullptr, aPropGet != nullptr, rc, (int)g_hookOk, g_markCount);
    SLOG("install: self='%s' %s", g_selfPath, g_summary);
}

bool installed() { return g_hookOk; }

const char* self_path() { return g_selfPath; }

const char* hook_summary() { return g_summary; }

int real_mappings() {
    // 故意走原始 openat：要看的是真实 maps，不是我们伪造的那份
    int fd = g_openat ? g_openat(AT_FDCWD, "/proc/self/maps", O_RDONLY | O_CLOEXEC, 0) : -1;
    if (fd < 0) return -1;

    std::string data;
    char buf[16384];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) data.append(buf, (size_t)r);
    close(fd);

    int hits = 0;
    size_t i = 0;
    while (i < data.size()) {
        size_t e = i;
        while (e < data.size() && data[e] != '\n') e++;
        if (LineHasMark(data.data() + i, e - i)) hits++;
        i = e + 1;
    }
    return hits;
}

int filtered_hits() {
    // 故意走普通 fopen：会命中我们自己的 hook，拿到过滤后的 memfd
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;

    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    int hits = 0;
    size_t i = 0;
    while (i < data.size()) {
        size_t e = i;
        while (e < data.size() && data[e] != '\n') e++;
        if (LineHasMark(data.data() + i, e - i)) hits++;
        i = e + 1;
    }
    return hits;
}

bool is_traced() {
    int fd = g_openat ? g_openat(AT_FDCWD, "/proc/self/status", O_RDONLY | O_CLOEXEC, 0) : -1;
    if (fd < 0) return false;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;

    const char* p = strstr(buf, "TracerPid:");
    if (!p) return false;
    p += 10;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p) != 0;
}

}  // namespace stealth
