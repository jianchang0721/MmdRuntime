package com.moss.mmdruntime;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

/**
 * MmdRuntime —— 独立 MMD 运行时的壳（v0.9）
 *
 * 这一版把 View 悬浮窗整个删了：往游戏 DecorView 里加 View 在部分 Activity 上
 * 会碰上 getResources() 未初始化的坑，直接把游戏带闪退（实测）。
 *
 * 现在 UI 全在 so 里：ImGui 挂 eglSwapBuffers，画在游戏自己的 GL 帧上。
 * 壳只干四件事：
 *   1. 把 libmmdruntime.so 复制改名后 System.load
 *   2. 每 16ms 调一次 nativeTick（主线程）
 *   3. 把触摸事件转发给 native（菜单吃掉事件时返回 true）
 *   4. 把类自身交给 native，供 ImGui 按钮回调（重载 so / 状态）
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
    private static native void nativeBind(Class<?> cls);
    private static native void nativeReset();
    private static native boolean nativeTouch(int action, float x, float y, float vw, float vh);

    static int loadSeq = 0;
    static String lastStatus = "尚未加载";
    static Handler tickHandler = null;
    static int tickCount = 0;
    static volatile boolean soReady = false;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (!PKG.equals(lp.packageName)) return;
        XposedBridge.log(TAG + ": hooked " + lp.packageName + " pid=" + Process.myPid());
        // 联动悬浮窗：游戏一启动就通知 MmdRuntime App 弹窗。
        // 显式广播不需要任何权限、不依赖 root；Application 未就绪时 receiver
        // 端收不到也由下面的 Activity onCreate 重发一次兜底。
        notifyGameLaunched();
        try {
            Class<?> actCls = XposedHelpers.findClass("android.app.Activity", lp.classLoader);

            // 第一个 Activity 出现时加载 so（不建任何 View）
            XposedBridge.hookAllMethods(actCls, "onCreate", new XC_MethodHook() {
                @Override
                protected void afterHookedMethod(MethodHookParam param) {
                    Activity a = (Activity) param.thisObject;
                    if (a == null) return;
                    if (loadSeq == 0 && isMainProcess()) {
                        lastStatus = doLoad();
                        XposedBridge.log(TAG + ": " + lastStatus);
                    }
                    // Application 此时必然就绪，重发一次联动广播兜底
                    notifyGameLaunched();
                }
            });

            // 触摸转发：菜单吃掉的事件不让游戏收到
            XposedBridge.hookAllMethods(actCls, "dispatchTouchEvent", new XC_MethodHook() {
                @Override
                protected void beforeHookedMethod(MethodHookParam param) {
                    if (!soReady || param.args == null || param.args.length < 1) return;
                    try {
                        MotionEvent ev = (MotionEvent) param.args[0];
                        if (ev == null) return;
                        View decor = ((Activity) param.thisObject).getWindow().getDecorView();
                        int w = decor.getWidth(), h = decor.getHeight();
                        if (w <= 0 || h <= 0) return;
                        boolean eaten = nativeTouch(ev.getActionMasked(), ev.getX(), ev.getY(),
                                (float) w, (float) h);
                        if (eaten) param.setResult(true);
                    } catch (Throwable ignored) { }
                }
            });
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": hook fail " + t);
        }
    }

    // ---------------- 联动悬浮窗 ----------------

    /** 向 MmdRuntime App 发「游戏已启动」显式广播。无权限要求、无 root 依赖。 */
    static void notifyGameLaunched() {
        try {
            Class<?> atCls = XposedHelpers.findClass("android.app.ActivityThread", null);
            Object app = XposedHelpers.callStaticMethod(atCls, "currentApplication");
            if (app instanceof android.content.Context) {
                android.content.Intent i =
                        new android.content.Intent("com.moss.mmdruntime.action.GAME_LAUNCHED");
                i.setPackage("com.moss.mmdruntime");
                ((android.content.Context) app).sendBroadcast(i);
            }
        } catch (Throwable t) {
            XposedBridge.log(TAG + ": notify fail " + t);
        }
    }

    // ---------------- so 加载 ----------------

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

    /** static 且签名固定：native 侧通过 methodID 反射调用。 */
    public static synchronized String doLoad() {
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
            soReady = true;
            try { nativeBind(HookEntry.class); } catch (Throwable ignored) { }
            startTick();
            return "已加载 " + dstName + "  " + dst.length() + " 字节";
        } catch (Throwable t) {
            return "dlopen 失败: " + t;
        }
    }

    /** ImGui 面板状态按钮用；和 nativeStatus 同源。 */
    public static String getStatus() {
        try { return nativeStatus(); } catch (Throwable t) { return lastStatus; }
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
                    try { lastStatus = nativeStatus(); } catch (Throwable ignored) { }
                }
                tickHandler.postDelayed(this, 16);
            }
        });
    }
}
