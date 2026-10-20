// =====================================================================
// bone_hook.cpp —— 用 Dobby 挂 m_Bones 容器析构
//
// 目标函数（libunity.so 0x293134，Unity 2018.4.13f1）：
//
//   stp  x19, x30, [sp, #-0x10]!
//   mov  x19, x0                 ; x19 = &m_Bones
//   ldr  x0, [x19]               ; data
//   cbz  x0, ret
//   ldrb w8, [x19, #0x18]        ; flag（bit0 = 内存归不归我管）
//   tbnz w8, #0x0, ret           ; bit0==1 直接跳过释放
//   ldr  w1, [x19, #0x8]
//   bl   Deallocate
//   str  xzr, [x19]
//   ret
//
// 我们在它执行前，如果发现 data 是我们的指针，就把 data/size/capacity
// 换回 Unity 原来的值，让 Unity 正常释放它自己的内存。
// =====================================================================
#include "logsink.h"
#include "bone_hook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "dobby/dobby.h"

#define HTAG "MmdBoneHook"
#define HLOG(...) logsink::Write(__VA_ARGS__)

namespace bonehook {

namespace {

// ---- 特征码：容器析构函数的前 7 条指令 ----
static const uint32_t kSig[] = {
    0xA9BF7BF3,  // stp  x19, x30, [sp, #-0x10]!
    0xAA0003F3,  // mov  x19, x0
    0xF9400260,  // ldr  x0, [x19]
    0xB40000C0,  // cbz  x0, +0x18
    0x39406268,  // ldrb w8, [x19, #0x18]
    0x37000088,  // tbnz w8, #0x0, +0x14
    0xB9400A61,  // ldr  w1, [x19, #0x8]
};
static const int kSigLen = (int)(sizeof(kSig) / sizeof(kSig[0]));

using DtorFn = void (*)(void*);
static DtorFn g_orig = nullptr;
static void*  g_hookAddr = nullptr;
static bool   g_installed = false;

// 我们登记的数据
static void*   g_ourData  = nullptr;
static void*   g_origData = nullptr;
static int32_t g_origSize = 0;
static int64_t g_origCap  = 0;
static int     g_hitCount = 0;

// m_Bones 结构内偏移（实测）
static const int OFF_DATA = 0x00;
static const int OFF_SIZE = 0x10;
static const int OFF_CAP  = 0x18;

void hook_body(void* self) {
    if (self) {
        void* cur = *(void**)((char*)self + OFF_DATA);
        if (g_ourData && cur == g_ourData) {
            // 把 Unity 原来的三字段还回去，让它按原样释放自己的那块内存
            *(void**)((char*)self + OFF_DATA) = g_origData;
            *(int32_t*)((char*)self + OFF_SIZE) = g_origSize;
            *(int64_t*)((char*)self + OFF_CAP)  = g_origCap;
            g_hitCount++;
            HLOG("[BONEHOOK] dtor intercepted #%d, restored origData=%p size=%d cap=%lld",
                 g_hitCount, g_origData, g_origSize, (long long)g_origCap);
        }
    }
    g_orig(self);
}

uintptr_t maps_base(const char* soname) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    static char line[2048];
    uintptr_t lowest = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, soname)) continue;
        uintptr_t a = strtoull(line, nullptr, 16);
        uint8_t* p = (uint8_t*)a;
        if (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') { fclose(f); return a; }
        if (!lowest || a < lowest) lowest = a;
    }
    fclose(f);
    return lowest;
}

// 在 libunity.so 的可执行段里按指令特征扫
void* scan_signature(uintptr_t base) {
    if (!base) return nullptr;
    ElfW(Ehdr)* eh = (ElfW(Ehdr)*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return nullptr;

    ElfW(Phdr)* ph = (ElfW(Phdr)*)(base + eh->e_phoff);
    int hits = 0;
    void* found = nullptr;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (!(ph[i].p_flags & PF_X)) continue;      // 只要可执行段
        uintptr_t start = base + ph[i].p_vaddr;
        size_t size = ph[i].p_filesz;
        if (size < sizeof(kSig)) continue;

        const uint32_t* p = (const uint32_t*)start;
        size_t n = size / 4;
        for (size_t k = 0; k + kSigLen <= n; k++) {
            bool ok = true;
            for (int j = 0; j < kSigLen; j++) {
                if (p[k + j] != kSig[j]) { ok = false; break; }
            }
            if (ok) {
                hits++;
                if (!found) found = (void*)(start + k * 4);
            }
        }
    }

    HLOG("[BONEHOOK] scan done: hits=%d found=%p", hits, found);
    return found;
}

}  // namespace

bool install() {
    if (g_installed) return true;

    uintptr_t base = maps_base("libunity.so");
    if (!base) { HLOG("[BONEHOOK] libunity.so not in maps"); return false; }
    HLOG("[BONEHOOK] libunity base=%p", (void*)base);

    void* addr = scan_signature(base);
    if (!addr) {
        HLOG("[BONEHOOK] signature not found, hook skipped");
        return false;
    }
    g_hookAddr = addr;

    int rc = DobbyHook(addr, (void*)hook_body, (void**)&g_orig);
    if (rc != 0 || !g_orig) {
        HLOG("[BONEHOOK] DobbyHook failed rc=%d orig=%p", rc, (void*)g_orig);
        return false;
    }

    g_installed = true;
    HLOG("[BONEHOOK] hooked bones dtor @%p orig=%p", addr, (void*)g_orig);
    return true;
}

void set_ours(void* ourData, void* origData, int32_t origSize, int64_t origCap) {
    g_ourData  = ourData;
    g_origData = origData;
    g_origSize = origSize;
    g_origCap  = origCap;
    HLOG("[BONEHOOK] set_ours our=%p orig=%p size=%d cap=%lld",
         ourData, origData, origSize, (long long)origCap);
}

bool installed() { return g_installed; }
void* found_address() { return g_hookAddr; }

}  // namespace bonehook
