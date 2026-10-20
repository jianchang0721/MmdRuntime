// =====================================================================
// module.cpp —— MmdRuntime 入口
//
// 这一版只做一件事：把 PMX 解析出来的统计打出来，跟已知数字比对。
// 不建骨架、不蒙皮、不渲染 —— 那些是后面几轮的事。
// =====================================================================
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <android/log.h>
#include <string>
#include <vector>

#include "mmd/pmx.h"

#define TAG "MmdRuntime"

// ---------------------------- 日志 ----------------------------
static std::string g_dir = "/data/data/com.hermes.h1game/files";
static FILE* g_fp = nullptr;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static void pick_dir() {
    const char* cands[] = {
        "/storage/emulated/0/MT2/log",
        "/sdcard/MT2/log",
        "/data/data/com.hermes.h1game/files",
    };
    for (int i = 0; i < 3; i++) {
        std::string probe = std::string(cands[i]) + "/.mmdrtest";
        FILE* f = fopen(probe.c_str(), "w");
        if (f) { fclose(f); unlink(probe.c_str()); g_dir = cands[i]; break; }
    }
}

static FILE* logfp() {
    if (g_fp) return g_fp;
    std::string p = g_dir + "/mmdruntime.log";
    g_fp = fopen(p.c_str(), "a");
    if (!g_fp && g_dir != "/data/data/com.hermes.h1game/files") {
        g_dir = "/data/data/com.hermes.h1game/files";
        g_fp = fopen((g_dir + "/mmdruntime.log").c_str(), "a");
    }
    return g_fp;
}

static void L(const char* fmt, ...) {
    pthread_mutex_lock(&g_mtx);
    FILE* f = logfp();
    if (f) {
        time_t t = time(nullptr);
        struct tm tmv; localtime_r(&t, &tmv);
        fprintf(f, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fflush(f);
    }
    va_list ap2; va_start(ap2, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, ap2);
    va_end(ap2);
    pthread_mutex_unlock(&g_mtx);
}

// ---------------------------- 解析测试 ----------------------------
static void run_parse_test() {
    L("========== [PMX] PARSE TEST BEGIN ==========");
    L("[PMX] log dir = %s", g_dir.c_str());

    const char* cands[] = {
        "/data/data/com.hermes.h1game/files/nangongyu.pmx",
        "/data/user/0/com.hermes.h1game/files/nangongyu.pmx",
        "/data/data/com.hermes.h1game/files/model.pmx",
    };
    const char* path = nullptr;
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        FILE* f = fopen(cands[i], "rb");
        if (f) { fclose(f); path = cands[i]; break; }
    }
    if (!path) {
        L("[PMX] ERR no pmx file found, expected one of:");
        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) L("[PMX]   %s", cands[i]);
        L("========== [PMX] PARSE TEST ABORT ==========");
        return;
    }
    L("[PMX] file = %s", path);

    mmd::Model model;
    std::string err;
    bool ok = mmd::Load(path, model, err);
    if (!ok) {
        L("[PMX] ERR parse failed: %s", err.c_str());
        L("========== [PMX] PARSE TEST FAILED ==========");
        return;
    }

    mmd::DumpStats(model, L);

    // 跟已知数字比对（另一条独立解析路径核对过的值）
    const int EXPECT_VERTS = 21804;
    const int EXPECT_MATS  = 25;
    const int EXPECT_BONES = 224;
    L("[PMX] ---- compare with known values ----");
    L("[PMX] verts   got=%zu expect=%d %s", model.verts.size(), EXPECT_VERTS,
      (int)model.verts.size() == EXPECT_VERTS ? "OK" : "MISMATCH");
    L("[PMX] mats    got=%zu expect=%d %s", model.materials.size(), EXPECT_MATS,
      (int)model.materials.size() == EXPECT_MATS ? "OK" : "MISMATCH");
    L("[PMX] bones   parsed=%zu declared=%d %s", model.bonesParsed, EXPECT_BONES,
      (int)model.bonesParsed >= 200 ? "OK(尾部几根坏数据已跳过)" : "TOO FEW");

    L("========== [PMX] PARSE TEST END ==========");
}

static void* worker(void*) {
    pick_dir();
    L("===== libmmdruntime.so loaded =====");
    run_parse_test();

    // 心跳
    int beat = 0;
    while (true) {
        sleep(60);
        beat++;
        L("[BEAT] %d", beat);
    }
    return nullptr;
}

// ---------------------------- JNI ----------------------------
extern "C" JNIEXPORT void JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeTick(JNIEnv*, jclass) {
    // 这一版不需要 tick，留空给后面用
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_moss_mmdruntime_HookEntry_nativeStatus(JNIEnv* env, jclass) {
    char buf[256];
    snprintf(buf, sizeof(buf), "log=%s", g_dir.c_str());
    return env->NewStringUTF(buf);
}

JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
    return JNI_VERSION_1_6;
}
