// =====================================================================
// elf_util.h —— 从进程内存里找 so 基址 + 手工解析 .dynsym 找符号
//
// 为什么不直接用 dlsym：
//   · libunity.so 被 strip 到只剩几个导出，dlopen 在游戏的 classloader
//     namespace 里还会解析不到私有依赖（实测报 libssgamesdkcronet.so not found）
//   · 但符号表本身还在内存里，翻 map + .dynsym 反而拿得到
//
// 这套代码跟游戏侧 module.cpp 里的 maps_base / elf_sym 是同一份，故意重复：
// 两个 so 完全解耦，谁都不依赖谁（只依赖对方的导出 API）。
// =====================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace elfu {

// 直接系统调用读文件，绕开进程内所有 open/fopen hook。
//
// 为什么必须这样：stealth 会把 /proc/self/maps 里我们自己的 so 过滤掉，
// 用 fopen 读只会拿到过滤后的副本 —— 过滤生效了，但也把“找自己”
// 这条路给堵死了（UI 就是因为这个一直「等待连接」）。
inline int RawOpen(const char* path) {
    return (int)syscall(__NR_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
}

// 把整个文件读进 buf，返回字节数，失败 -1
inline long RawReadAll(const char* path, char* buf, long cap) {
    int fd = RawOpen(path);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = 0;
    return total;
}

// 按「同时包含两个子串」找 so 基址。
//
// 用 soname 找有个致命问题：HookEntry 为了让同一个 so 能重复加载，
// 会把它复制改名（libmmdruntime.so → libil2cpp_ext_1.so），
// 按原名去 maps 里搜直接落空。所以改成按目录 + 扩展名找：
// 游戏自己绝对不会从私有目录 mmap 一个 .so，这条规则不会误伤。
inline uintptr_t MapsBaseContains(const char* a, const char* b) {
    static char buf[512 * 1024];
    if (RawReadAll("/proc/self/maps", buf, (long)sizeof(buf)) <= 0) return 0;

    uintptr_t addrs[64];
    int n = 0;
    char* p = buf;
    while (*p) {
        char* e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        if (strstr(p, a) && (!b || strstr(p, b))) {
            uintptr_t start = strtoull(p, nullptr, 16);
            if (n < 64) addrs[n++] = start;
        }
        p = e + 1;
    }

    uintptr_t lowest = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)addrs[i];
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return addrs[i];
        if (!lowest || addrs[i] < lowest) lowest = addrs[i];
    }
    return lowest;
}

// 找 so 的加载基址：扫 /proc/self/maps，返回带 ELF 魔数的那个映射起点
inline uintptr_t MapsBase(const char* soname) {
    static char buf[512 * 1024];
    if (RawReadAll("/proc/self/maps", buf, (long)sizeof(buf)) <= 0) return 0;

    uintptr_t addrs[128];
    int n = 0;
    char* p = buf;
    while (*p) {
        char* e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        if (strstr(p, soname)) {
            uintptr_t start = strtoull(p, nullptr, 16);
            if (n < 128) addrs[n++] = start;
        }
        p = e + 1;
    }

    uintptr_t lowest = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* p = (uint8_t*)addrs[i];
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') return addrs[i];
        if (!lowest || addrs[i] < lowest) lowest = addrs[i];
    }
    return lowest;
}

// 在 so 的 .dynsym 里按名字找符号，返回运行时绝对地址
inline uintptr_t ElfSym(uintptr_t base, const char* name) {
    if (!base) return 0;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;

    uintptr_t dyn = 0;
    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) { dyn = base + ph[i].p_vaddr; break; }
    }
    if (!dyn) return 0;

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    for (ElfW(Dyn)* d = (ElfW(Dyn)*)dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab = d->d_un.d_ptr; break;
            case DT_HASH:     hash   = d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnu    = d->d_un.d_ptr; break;
            default: break;
        }
    }
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;
    if (hash && hash < base) hash += base;
    if (gnu && gnu < base) gnu += base;
    if (!symtab || !strtab) return 0;

    uint32_t nsym = 0;
    if (hash) {
        nsym = ((uint32_t*)hash)[1];
    } else if (gnu) {
        uint32_t* g = (uint32_t*)gnu;
        uint32_t nbuckets = g[0], symoffset = g[1], bloom = g[2];
        uint32_t* buckets = &g[4 + bloom * 2];
        uint32_t* chain = &buckets[nbuckets];
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= symoffset) {
            while (!(chain[last - symoffset] & 1)) last++;
            nsym = last + 1;
        }
    }
    if (!nsym) return 0;
    if (nsym > 200000) nsym = 200000;

    ElfW(Sym)* syms = (ElfW(Sym)*)symtab;
    const char* strs = (const char*)strtab;
    size_t nameLen = strlen(name);
    for (uint32_t i = 0; i < nsym; i++) {
        uint32_t off = syms[i].st_name;
        if (off == 0 || off > 0xFFFFFF) continue;
        const char* s = strs + off;
        size_t k = 0;
        while (k <= nameLen && s[k] == name[k]) k++;
        if (k == nameLen + 1) return base + syms[i].st_value;
    }
    return 0;
}

}  // namespace elfu
