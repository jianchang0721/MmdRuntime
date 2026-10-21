// =====================================================================
// bone_names.cpp
//
// 两件事：
//   1. 遍历 SMR.bones[] 读名字 —— 走 managed get_bones + Object.get_name，
//      不碰任何 native 偏移，换游戏版本也照跑。
//   2. hook Object.get_name，记录「谁在读名字」——用来反查游戏内部的
//      骨骼查找函数。拿到调用点相对模块的偏移后，就能固化成基址定位。
//
// 安全红线：
//   · hook 里绝对不能再调 object_name（会重入自己 → 死循环），只解析返回值
//   · 装之前先确认 get_name 的 methodPointer 不是所有 icall 共用的 thunk
//   · 记录配额用完直接透传，不给 UI 线程添负担
// =====================================================================
#include "bone_names.h"

#include <cstdio>
#include <cstring>

#include "logsink.h"
#include "unity/il2cpp_api.h"
#include "unity/unity_objects.h"
#include "dobby/dobby.h"

#define BTAG "MmdBoneName"
#define BLOG(...) logsink::Write(__VA_ARGS__)

namespace game {

namespace {

const MethodInfo* g_mGetBones = nullptr;
void* g_nameTarget = nullptr;
bool  g_installed  = false;
int   g_maxRecords = 0;
int   g_records    = 0;

typedef void* (*GetNameFn)(void* self);
GetNameFn g_origGetName = nullptr;

// 把地址翻译成「模块名+偏移」。只在记录配额内调用，不进热路径。
void describe_addr(void* addr, char* out, size_t n) {
    out[0] = 0;
    if (!addr) return;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    uintptr_t a = (uintptr_t)addr;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (a < lo || a >= hi) continue;
        // 路径在最后一列
        char* p = strchr(line, '/');
        const char* mod = p ? p : "?";
        const char* base = strrchr(mod, '/');
        base = base ? base + 1 : mod;
        snprintf(out, n, "%s+0x%lx", base, (unsigned long)(a - lo));
        break;
    }
    fclose(f);
}

// 只解析返回的 Il2CppString，不调用任何 Unity API（防重入）
void copy_name(void* str, char* out, int cap) {
    out[0] = 0;
    if (!str || cap <= 1) return;
    char* p = (char*)str;
    int32_t len = *(int32_t*)(p + 16);
    if (len < 0 || len > 4096) return;
    const uint16_t* ch = (const uint16_t*)(p + 20);
    int k = 0;
    for (int32_t i = 0; i < len && k < cap - 1; i++) {
        uint16_t c = ch[i];
        out[k++] = (c < 0x80) ? (char)c : '?';
    }
    out[k] = 0;
}

void* hook_get_name(void* self) __attribute__((noinline));
void* hook_get_name(void* self) {
    void* r = g_origGetName ? g_origGetName(self) : nullptr;
    if (g_records < g_maxRecords && r) {
        g_records++;
        char name[128];
        copy_name(r, name, sizeof(name));
        void* lr = __builtin_extract_return_addr(__builtin_return_address(0));
        char where[192];
        describe_addr(lr, where, sizeof(where));
        BLOG("[NAMEHOOK] #%d name='%s' lr=%p %s", g_records, name, lr, where);
    }
    return r;
}

}  // namespace

bool collect_bone_names(void* smrManaged, std::vector<BoneNameEntry>& out) {
    out.clear();
    if (!smrManaged) return false;

    if (!g_mGetBones) {
        void* k = uo::u_class("UnityEngine", "SkinnedMeshRenderer");
        g_mGetBones = (const MethodInfo*)uo::u_method(k, "get_bones", 0);
        if (!g_mGetBones) {
            BLOG("[BONENAME] get_bones 方法没找到");
            return false;
        }
    }

    void* arr = uo::u_call(smrManaged, (void*)g_mGetBones, nullptr);
    if (!arr) {
        BLOG("[BONENAME] get_bones() 返回空");
        return false;
    }
    size_t n = *(size_t*)((char*)arr + 24);   // Il2CppArray.max_length
    if (n == 0 || n > 4096) {
        BLOG("[BONENAME] 骨骼数异常 %zu", n);
        return false;
    }

    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        void* tr = *(void**)uo::u_array_data_from(arr, i);
        BoneNameEntry e;
        if (tr) {
            e.instanceId = uo::instance_id(tr);
            e.name = uo::object_name(tr);
        }
        out.push_back(std::move(e));
    }
    return true;
}

void* name_hook_target() {
    if (g_nameTarget) return g_nameTarget;
    void* kObj = uo::u_class("UnityEngine", "Object");
    if (!kObj) return nullptr;
    const MethodInfo* m = (const MethodInfo*)uo::u_method(kObj, "get_name", 0);
    if (!m) {
        BLOG("[NAMEHOOK] Object.get_name 没找到");
        return nullptr;
    }
    // MethodInfo 第一个字段就是 methodPointer
    g_nameTarget = *(void**)m;
    BLOG("[NAMEHOOK] Object.get_name target=%p", g_nameTarget);
    return g_nameTarget;
}

bool install_name_hook(int maxRecords) {
    if (g_installed) return true;
    void* target = name_hook_target();
    if (!target) return false;

    // 独立性校验：如果 get_name 和 GetInstanceID 的 methodPointer 相同，
    // 说明是所有 icall 共用的 thunk，hook 它会把所有 icall 都拦下来且
    // 参数语义对不上，宁可不装。
    void* kObj = uo::u_class("UnityEngine", "Object");
    const MethodInfo* mId = (const MethodInfo*)uo::u_method(kObj, "GetInstanceID", 0);
    void* pId = mId ? *(void**)mId : nullptr;
    if (pId && pId == target) {
        BLOG("[NAMEHOOK] 共用 thunk（get_name == GetInstanceID == %p），不装 hook", target);
        return false;
    }

    g_maxRecords = maxRecords > 0 ? maxRecords : 0;
    g_records = 0;
    int rc = DobbyHook(target, (void*)hook_get_name, (void**)&g_origGetName);
    if (rc != 0 || !g_origGetName) {
        BLOG("[NAMEHOOK] DobbyHook 失败 rc=%d orig=%p", rc, (void*)g_origGetName);
        return false;
    }
    g_installed = true;
    BLOG("[NAMEHOOK] hooked @%p orig=%p quota=%d", target, (void*)g_origGetName, g_maxRecords);
    return true;
}

bool name_hook_installed() { return g_installed; }

void dump_bone_names(const std::vector<BoneNameEntry>& v, int maxPrint) {
    int n = (int)v.size();
    if (maxPrint > 0 && n > maxPrint) n = maxPrint;
    BLOG("[BONENAME] total=%zu（只打前 %d 条）", v.size(), n);
    for (int i = 0; i < n; i++) {
        BLOG("[BONENAME]   #%d id=%d name='%s'", i, v[i].instanceId, v[i].name.c_str());
    }
}

}  // namespace game
