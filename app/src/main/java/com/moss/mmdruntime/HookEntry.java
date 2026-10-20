package com.moss.mmdruntime;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import com.moss.mmdruntime.service.JniBridge;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * MmdRuntime 注入壳（v1.2）
 *
 * 干四件事：
 *   1. 把 libmmdruntime.so 和 libimgui_overlay.so 复制改名后 System.load
 *      （先游戏逻辑，后 UI —— UI 初始化时要找 mmd_get_api）
 *   2. 从模块自己的 APK 里解出字体到游戏私有目录，喂给 UI so
 *   3. 每 16ms 调一次 nativeTick（游戏主线程）驱动 MMD 状态机
 *   4. hook Activity.dispatchTouchEvent 把触摸喂给 ImGui，菜单吃掉的不给游戏
 *
 * 已经没有的东西（v1.0~v1.1 那一套）：
 *   · 往 DecorView 里塞 View（游戏 Activity 的 getResources() 未就绪时必崩）
 *   · 独立 App 进程 + WindowManager 悬浮窗 + 前台服务
 *   · 广播拉起（MIUI 会拦 stopped state 的进程）
 *   · ui.conf / status.conf 文件通道
 */
public class HookEntry implements IXposedHookLoadPackage {

    static final String TAG = "MmdRuntime";
    static final String PKG = "com.hermes.h1game";

    static final String GAME_SO = "libmmdruntime.so";
    static final String UI_SO   = "libimgui_overlay.so";

    static final String GAME_FILES = "/data/data/com.hermes.h1game/files";
    static final String[] DIRS = {
            "/data/data/com.hermes.h1game/files/",
            "/data/user/0/com.hermes.h1game/files/",
    };

    // ---- 游戏侧 so 的 JNI ----
    private static native void nativeTick();
    private static native String nativeStatus();
    private static native void nativeBind(Class<?> cls);
    private static native void nativeReset();

    static int  loadSeq      = 0;
    static int  uiLoadSeq    = 0;
    static String lastStatus = "尚未加载";
    static Handler tickHandler = null;
    static int tickCount     = 0;

    static volatile boolean gameSoReady = false;
    static volatile boolean uiSoReady   = false;
    static volatile boolean fontsReady  = false;

    static String latinFontPath = "";
    static String cjkFontPath   = "";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!PKG.equals(lp.packageName)) return;
        XposedBridge.log(TAG + ": hooked " + lp.packageName + " pid=" + Process.myPid());
        if (!isMainProcess()) {
            XposedBridge.log(TAG + ": 子进程，跳过");
            return;
        }

        try {
            Class<?> actCls = XposedHelpers.findClass("android.app.Activity", lp.classLoader);

            // 第一个 Activity 出现后再加载 so：这时进程稳定，也没有早期 classloader 问题
            XposedBridge.hookAllMethods(actCls, "onCreate", new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    Activity a = (Activity) param.thisObject;
                    if (a == null) return;
                    if (loadSeq == 0) {
                        lastStatus = doLoad(a);
                        XposedBridge.log(TAG + ": " + lastStatus);
                    }
                }
            });

            // 触摸转发：菜单吃掉的事件不让游戏收到
            XposedBridge.hookAllMethods(actCls, "dispatchTouchEvent", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (!uiSoReady || param.args == null || param.args.length < 1) return;
                    try {
                        MotionEvent ev = (MotionEvent) param.args[0];
                        if (ev == null) return;
                        View decor = ((Activity) param.thisObject).getWindow().getDecorView();
                        int w = decor.getWidth(), h = decor.getHeight();
                        if (w <= 0 || h <= 0) return;
                        boolean eaten = JniBridge.INSTANCE.onTouchEvent(
                                ev.getActionMasked(), ev.getX(), ev.getY(), (float) w, (float) h);
                        if (eaten) param.setResult(true);
                    } catch (Throwable ignored) {
                    }
                }
            });
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook fail " + t);
        }
    }

    // ================= 加载 =================

    static boolean isMainProcess() {
        try {
            java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.FileReader("/proc/self/cmdline"));
            String s = r.readLine();
            r.close();
            return s != null && !s.contains(":");
        } catch (Throwable t) {
            return true;
        }
    }

    /** static 且签名固定：游戏侧 so 通过 methodID 反射调用 */
    public static synchronized String doLoad(Activity act) {
        StringBuilder sb = new StringBuilder();

        // ① 游戏逻辑 so（必须第一个）
        String r1 = loadSo(GAME_SO, true);
        sb.append("game=").append(r1);

        // ② 字体（UI so 依赖）
        String r2 = prepareFonts(act);
        sb.append(" | fonts=").append(r2);

        // ③ UI so
        String r3 = loadSo(UI_SO, false);
        sb.append(" | ui=").append(r3);

        // ④ 字体路径喂给 UI so
        if (uiSoReady) {
            try {
                JniBridge.INSTANCE.setFontPaths(latinFontPath, cjkFontPath);
                JniBridge.INSTANCE.setConfigDirectory(GAME_FILES + "/mmdrt");
                sb.append(" | fontPath=ok");
            } catch (Throwable t) {
                sb.append(" | fontPath=").append(t);
            }
        }
        return sb.toString();
    }

    static String loadSo(String soName, boolean isGameSide) {
        File src = null;
        String dir = null;
        for (String d : DIRS) {
            File f = new File(d, soName);
            if (f.exists() && f.canRead()) {
                src = f;
                dir = d;
                break;
            }
        }
        if (src == null) return "找不到 " + soName + "（应放在 " + DIRS[0] + "）";

        String base = soName.endsWith(".so") ? soName.substring(0, soName.length() - 3) : soName;
        int seq = isGameSide ? (++loadSeq) : (++uiLoadSeq);
        String dstName = base + "_" + seq + ".so";
        File dst = new File(dir, dstName);
        try {
            copy(src, dst);
        } catch (Throwable t) {
            return "复制失败: " + t;
        }
        try {
            System.load(dst.getAbsolutePath());
            if (isGameSide) {
                gameSoReady = true;
                try { nativeBind(HookEntry.class); } catch (Throwable ignored) { }
                startTick();
            } else {
                uiSoReady = true;
            }
            return "ok(" + dstName + " " + dst.length() + "B)";
        } catch (Throwable t) {
            return "dlopen 失败: " + t;
        }
    }

    // ================= 字体 =================

    /**
     * 模块 APK 里的 assets/fonts 解到游戏私有目录。
     *
     * 为什么不在 App 进程干：现在 UI 跑在游戏进程里，游戏进程读不到 App 的 assets，
     * 只能自己去翻 APK（base.apk 是 0644，任何 uid 都能读）。
     */
    static String prepareFonts(Context ctx) {
        File outDir = new File(GAME_FILES, "fonts");
        if (!outDir.exists()) outDir.mkdirs();
        File latin = new File(outDir, "latin.ttf");
        File cjk = new File(outDir, "cjk.ttf");

        try {
            if (!latin.exists() || latin.length() < 1000) extractAsset(ctx, "fonts/Nunito-Regular.ttf", latin);
            if (!cjk.exists() || cjk.length() < 10000)  extractAsset(ctx, "fonts/cjk.ttf", cjk);
            latinFontPath = latin.getAbsolutePath();
            cjkFontPath = cjk.getAbsolutePath();
            fontsReady = true;
            return latin.length() + "/" + cjk.length();
        } catch (Throwable t) {
            fontsReady = false;
            return "失败: " + t;
        }
    }

    static void extractAsset(Context ctx, String assetName, File dst) throws Exception {
        ApplicationInfo ai = ctx.getPackageManager()
                .getApplicationInfo("com.moss.mmdruntime", 0);
        ZipFile zip = new ZipFile(ai.sourceDir);
        ZipEntry entry = zip.getEntry("assets/" + assetName);
        if (entry == null) throw new RuntimeException("assets 里没有 " + assetName);
        InputStream in = zip.getInputStream(entry);
        OutputStream out = new FileOutputStream(dst);
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            try { in.close(); } catch (Throwable ignored) { }
            try { out.close(); } catch (Throwable ignored) { }
            try { zip.close(); } catch (Throwable ignored) { }
        }
        dst.setReadable(true, false);
    }

    static void copy(File src, File dst) throws Exception {
        InputStream in = new FileInputStream(src);
        OutputStream out = new FileOutputStream(dst);
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } finally {
            try { in.close(); } catch (Throwable ignored) { }
            try { out.close(); } catch (Throwable ignored) { }
        }
        try { dst.setReadable(true, false); } catch (Throwable ignored) { }
    }

    /** ImGui 面板状态按钮用 */
    public static String getStatus() {
        try { return nativeStatus(); } catch (Throwable t) { return lastStatus; }
    }

    // ================= 主线程 tick =================

    static void startTick() {
        if (tickHandler != null) return;
        tickHandler = new Handler(Looper.getMainLooper());
        tickHandler.post(new Runnable() {
            @Override public void run() {
                tickCount++;
                try { nativeTick(); } catch (Throwable ignored) { }
                if (tickCount % 120 == 0) {
                    try { lastStatus = nativeStatus(); } catch (Throwable ignored) { }
                }
                tickHandler.postDelayed(this, 16);
            }
        });
    }
}
