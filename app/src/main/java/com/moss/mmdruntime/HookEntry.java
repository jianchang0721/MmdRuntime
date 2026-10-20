package com.moss.mmdruntime;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.View;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.WeakHashMap;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * MmdRuntime —— 独立 MMD 运行时的壳。
 *
 * 壳只干两件事：把 libmmdruntime.so 加载进游戏进程、提供一个主线程 tick。
 * 真正的逻辑（PMX 解析 / 骨架 / 蒙皮 / 渲染）全在 so 里，
 * 改完只需要替换 so，不用重装这个 APK。
 *
 * 复制成 libmmdruntime_<n>.so 再加载：dlopen 对同一路径返回缓存句柄，
 * 不改名的话换了文件也不生效。
 */
public class HookEntry implements IXposedHookLoadPackage {

    static final String TAG = "MmdRuntime";
    static final String PKG = "com.hermes.h1game";
    static final String SO_BASE = "libmmdruntime";
    static final String SO_NAME = SO_BASE + ".so";
    static final String[] DIRS = {
            "/data/data/com.hermes.h1game/files/",
            "/data/user/0/com.hermes.h1game/files/",
    };

    private static native void nativeTick();
    private static native String nativeStatus();

    static int loadSeq = 0;
    static String lastStatus = "尚未加载";
    static final WeakHashMap<Activity, FloatPanel> panels = new WeakHashMap<>();
    static Handler tickHandler = null;
    static int tickCount = 0;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!PKG.equals(lp.packageName)) return;
        XposedBridge.log(TAG + ": hooked " + lp.packageName + " pid=" + Process.myPid());
        try {
            Class<?> actCls = XposedHelpers.findClass("android.app.Activity", lp.classLoader);
            XposedBridge.hookAllMethods(actCls, "onCreate", new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    final Activity a = (Activity) param.thisObject;
                    // 基类 onCreate 钩子上 thisObject 可能是 null，或者窗口还没 attach。
                    // 这种情况直接跳过，等下一个 Activity。
                    if (a == null) return;
                    try { if (a.getWindow() == null) return; } catch (Throwable ignored) { return; }

                    // so 加载不依赖 UI，放最前面。UI 崩了功能照常。
                    if (loadSeq == 0 && isMainProcess()) {
                        lastStatus = doLoad();
                    }

                    // 悬浮窗尽力而为：推迟到 onCreate 完全返回之后，
                    // 有些游戏 Activity 在 super.onCreate() 阶段 getResources() 还没初始化好
                    final Activity af = a;
                    new Handler(Looper.getMainLooper()).post(new Runnable() {
                        @Override public void run() { installPanel(af); }
                    });
                }

                void installPanel(final Activity a) {
                    try {
                        FloatPanel.Theme th = new FloatPanel.Theme();
                        th.title = "MmdRuntime";
                        th.accent = 0xFF7C4DFF;
                        th.accentDim = 0x667C4DFF;
                        th.textMain = 0xFFD8CCFF;
                        th.textSub = 0xFF7FD4FF;
                        th.btnText = 0xFFE0B9FF;
                        final FloatPanel panel = FloatPanel.get(a, th);
                        panels.put(a, panel);
                        panel.setLog(lastStatus);

                        if (panels.size() <= 1) {   // 只在第一个 Activity 上装按钮
                            panel.clearButtons();
                            TextView btn = panel.addButton("重载 so");
                            btn.setOnClickListener(new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    lastStatus = doLoad();
                                    panel.setLog(lastStatus);
                                }
                            });
                            TextView st = panel.addButton("状态");
                            st.setOnClickListener(new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    try { lastStatus = nativeStatus(); } catch (Throwable ignored) { }
                                    panel.setLog(lastStatus);
                                }
                            });
                        }
                    } catch (Throwable t) {
                        XposedBridge.log(TAG + ": overlay fail " + android.util.Log.getStackTraceString(t));
                    }
                }
            });
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook fail " + t);
        }
    }

    // ---------------- so 加载 ----------------

    /** 子进程（:push / :lebian.base 等）不加载 so，避免多份心跳线程。 */
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

    static synchronized String doLoad() {
        File src = null;
        String dir = null;
        for (String d : DIRS) {
            File f = new File(d, SO_NAME);
            if (f.exists() && f.canRead()) { src = f; dir = d; break; }
        }
        if (src == null) return "找不到 " + SO_NAME + "（应放在 " + DIRS[0] + "）";

        String dstName = SO_BASE + "_" + (++loadSeq) + ".so";
        File dst = new File(dir, dstName);
        try {
            copy(src, dst);
        } catch (Throwable t) {
            return "复制失败: " + t;
        }
        try {
            System.load(dst.getAbsolutePath());
            startTick();
            return "已加载 " + dstName + "  " + dst.length() + " 字节";
        } catch (Throwable t) {
            return "dlopen 失败: " + t;
        }
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

    // ---------------- 主线程 tick ----------------

    static void startTick() {
        if (tickHandler != null) return;
        tickHandler = new Handler(Looper.getMainLooper());
        tickHandler.post(new Runnable() {
            @Override public void run() {
                tickCount++;
                try { nativeTick(); } catch (Throwable ignored) { }
                if (tickCount % 120 == 0) {
                    try { lastStatus = nativeStatus(); refreshStatus(); } catch (Throwable ignored) { }
                }
                tickHandler.postDelayed(this, 16);
            }
        });
    }

    // ---------------- 悬浮窗（FloatPanel 接管 UI） ----------------

    static void refreshStatus() {
        for (FloatPanel p : panels.values()) p.setLog(lastStatus);
    }
}
