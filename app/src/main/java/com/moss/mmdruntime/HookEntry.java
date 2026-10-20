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
    static final String CUSTOM_SO_DIR = GAME_FILES + "/so";
    static final String[] DIRS = {
            "/data/data/com.hermes.h1game/files/",
            "/data/user/0/com.hermes.h1game/files/",
    };

    // ---- 游戏侧 so 的 JNI ----
    private static native void nativeTick();
    private static native String nativeStatus();
    private static native void nativeBind(Class<?> cls);
    private static native void nativeReset();

    // UI so 的 JNI 入口全部收回到游戏侧 so（因为 UI so 是从 memfd 加载的，
    // ART 不认识它，Java_com_xxx 符号没人解析）
    private static native String  nativeLoadUi(String soPath, String fakeName);
    private static native void    nativeUiFonts(String latin, String cjk);
    private static native boolean nativeUiTouch(int action, float x, float y, float vw, float vh);
    private static native boolean nativeUiReady();
    private static native boolean nativeCameraTouch(int action, int pointerId,
                                                    float x, float y, float vw, float vh);

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

            // 触摸分发，优先级从高到低：
            //   1. UI 面板（ImGui，含标题栏拖拽）
            //   2. 自由视角摄像机（左右半屏，开着才接管）
            //   3. 游戏
            // 多指逐根问，一根被吃就整体吃掉 —— 否则游戏会收到
            // 「一半的拖动」，人物跟着乱走。
            XposedBridge.hookAllMethods(actCls, "dispatchTouchEvent", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (param.args == null || param.args.length < 1) return;
                    try {
                        MotionEvent ev = (MotionEvent) param.args[0];
                        if (ev == null) return;
                        View decor = ((Activity) param.thisObject).getWindow().getDecorView();
                        int w = decor.getWidth(), h = decor.getHeight();
                        if (w <= 0 || h <= 0) return;

                        boolean eaten = false;
                        final int cnt = ev.getPointerCount();
                        for (int i = 0; i < cnt && !eaten; i++) {
                            final int action = (cnt > 1)
                                    ? (ev.getActionMasked() | (i << 8))
                                    : ev.getActionMasked();
                            final float x = ev.getX(i);
                            final float y = ev.getY(i);
                            final int pid = ev.getPointerId(i);

                            if (uiSoReady && nativeUiTouch(action, x, y, (float) w, (float) h)) {
                                eaten = true;
                                break;
                            }
                            if (nativeCameraTouch(action, pid, x, y, (float) w, (float) h)) {
                                eaten = true;
                                break;
                            }
                        }
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
        String r1 = loadSo(GAME_SO, true, act);
        sb.append("game=").append(r1);

        // ② 字体（UI so 依赖）
        String r2 = prepareFonts(act);
        sb.append(" | fonts=").append(r2);

        // ③ UI so（主路径：mmdruntime 用 memfd 加载，落盘文件加载后被删）
        String r3 = loadUiSo(act);
        sb.append(" | ui=").append(r3);

        // ④ 字体路径喂给 UI
        if (uiSoReady) {
            try {
                nativeUiFonts(latinFontPath, cjkFontPath);
                sb.append(" | fontPath=ok");
            } catch (Throwable t) {
                sb.append(" | fontPath=").append(t);
            }
        }

        // ⑤ 玩家自己拖进来的 so（App 的「加载自定义 so」放这里）
        String r5 = loadCustomSos();
        sb.append(" | customSo=").append(r5);
        return sb.toString();
    }

    /**
     * 加载 files/so/ 下玩家自己放的 .so。
     *
     * 用 System.load 而不是 android_dlopen_ext：这些是第三方 native 库，
     * 大概率带 JNI_OnLoad，走 ART 那条路才能被正确注册。
     * 每个单独 try，一个挂了不影响后面的。
     */
    static String loadCustomSos() {
        File dir = new File(CUSTOM_SO_DIR);
        File[] files = dir.listFiles();
        if (files == null || files.length == 0) return "none";

        int ok = 0, fail = 0;
        for (File f : files) {
            if (!f.isFile()) continue;
            String n = f.getName();
            if (!n.endsWith(".so")) continue;
            try {
                System.load(f.getAbsolutePath());
                ok++;
                XposedBridge.log(TAG + ": custom so loaded " + n + " (" + f.length() + "B)");
            } catch (Throwable t) {
                fail++;
                XposedBridge.log(TAG + ": custom so FAILED " + n + " -> " + t);
            }
        }
        return ok + " ok / " + fail + " fail";
    }

    /**
     * UI so 加载。
     *
     * 主路径：解到私有目录 → 交给 mmdruntime 用 android_dlopen_ext + memfd 加载。
     *         maps 里显示的是我们编的假路径，落盘文件加载完立刻被 native 侧 unlink。
     * 兜底：memfd 那条路挂了就退回 System.load（能跑起来比隐身他妈的重要）。
     */
    static String loadUiSo(Context ctx) {
        File src = null;
        String dir = DIRS[0];
        for (String d : DIRS) {
            File f = new File(d, UI_SO);
            if (f.exists() && f.canRead()) { src = f; dir = d; break; }
        }

        int seq = ++uiLoadSeq;
        File dst = new File(dir, "libunity_gl_" + seq + ".so");

        try {
            if (src != null) copy(src, dst);
            else extractSoFromApk(ctx, UI_SO, dst);
        } catch (Throwable t) {
            return "解包到 " + dst.getName() + " 失败: " + t;
        }

        // maps 里显示的名字 —— 看着像游戏自己的库，实际是不存在的路径
        String fakeName = "/data/app/com.hermes.h1game/lib/arm64/libunity_gl.so";

        String err = null;
        try {
            err = nativeLoadUi(dst.getAbsolutePath(), fakeName);
        } catch (Throwable t) {
            err = "nativeLoadUi 异常: " + t;
        }
        if (err == null || err.isEmpty()) {
            uiSoReady = true;
            return "memfd ok";
        }

        try {
            System.load(dst.getAbsolutePath());
            uiSoReady = true;
            return "fallback System.load (" + err + ")";
        } catch (Throwable t) {
            return "全部失败: " + err + " / " + t;
        }
    }

    static String loadSo(String soName, boolean isGameSide, Context ctx) {
        File src = null;
        String dir = DIRS[0];
        for (String d : DIRS) {
            File f = new File(d, soName);
            if (f.exists() && f.canRead()) {
                src = f;
                dir = d;
                break;
            }
        }

        // 落盘名伪装。私有目录里躺一个 libmmdruntime_1.so 太扎眼，
        // 改成看着像游戏自身扩展库的名字。
        // （stealth 那边还有一条规则：私有目录里 mmap 的 .so 一律隐藏，不管叫什么）
        int seq = isGameSide ? (++loadSeq) : (++uiLoadSeq);
        String camo = isGameSide ? "libil2cpp_ext" : "libunity_gl";
        String dstName = camo + "_" + seq + ".so";
        File dst = new File(dir, dstName);

        boolean alreadyAtDst = false;
        if (src == null) {
            // 游戏侧 so 只能靠人推到游戏目录；UI so 在模块 APK 里，自己解。
            // （extractNativeLibs=false 时 APK 的 lib/ 不会落地到 nativeLibraryDir，
            //   所以直接翻 zip 最稳）
            if (isGameSide) return "找不到 " + soName + "（应放在 " + DIRS[0] + "）";
            try {
                extractSoFromApk(ctx, soName, dst);
            } catch (Throwable t) {
                return "从 APK 解 " + soName + " 失败: " + t;
            }
            alreadyAtDst = true;   // 已经落在目标路径了，别再自己拷自己（会把文件截成 0）
        }

        if (!alreadyAtDst) {
            try {
                copy(src, dst);
            } catch (Throwable t) {
                return "复制失败: " + t;
            }
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

    /**
     * 拿模块自己 APK 的路径。
     *
     * 游戏进程里查别的包可能被 Android 11+ 的包可见性挡住（抛 NameNotFound），
     * 所以先试 PackageManager，再试隐藏 API IPackageManager（不经过可见性过滤），
     * 最后试常见路径。
     */
    static String moduleApkPath(Context ctx) {
        try {
            ApplicationInfo ai = ctx.getPackageManager()
                    .getApplicationInfo("com.moss.mmdruntime", 0);
            if (ai != null && ai.sourceDir != null) return ai.sourceDir;
        } catch (Throwable ignored) { }

        try {
            Class<?> at = XposedHelpers.findClass("android.app.ActivityThread", null);
            Object pm = XposedHelpers.callStaticMethod(at, "getPackageManager");
            Object ai = XposedHelpers.callMethod(pm, "getApplicationInfo",
                    "com.moss.mmdruntime", 0L, 0);
            if (ai != null) {
                Object dir = XposedHelpers.getObjectField(ai, "sourceDir");
                if (dir instanceof String) return (String) dir;
            }
        } catch (Throwable ignored) { }

        return null;
    }

    /** 从模块 APK 的 lib/arm64-v8a/ 里解一个 so 落地 */
    static void extractSoFromApk(Context ctx, String soName, File dst) throws Exception {
        String apk = moduleApkPath(ctx);
        if (apk == null) throw new RuntimeException("拿不到模块 APK 路径");
        ZipFile zip = new ZipFile(apk);
        ZipEntry entry = zip.getEntry("lib/arm64-v8a/" + soName);
        if (entry == null) {
            zip.close();
            throw new RuntimeException("APK 里没有 lib/arm64-v8a/" + soName);
        }
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
        dst.setExecutable(true, false);
    }

    static void extractAsset(Context ctx, String assetName, File dst) throws Exception {
        String apk = moduleApkPath(ctx);
        if (apk == null) throw new RuntimeException("拿不到模块 APK 路径");
        ZipFile zip = new ZipFile(apk);
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
