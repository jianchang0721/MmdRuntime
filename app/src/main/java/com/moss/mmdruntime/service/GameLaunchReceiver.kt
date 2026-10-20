package com.moss.mmdruntime.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * 游戏启动联动（不依赖 root）：
 *
 * 游戏侧的 HookEntry（LSPosed 模块）在游戏进程一启动就发一条显式广播
 * com.moss.mmdruntime.action.GAME_LAUNCHED 过来。这里收到后：
 *   · 服务没跑 → startForegroundService 拉起悬浮窗
 *   · 服务在跑（折叠成横栏）→ 让 native 展开成完整面板
 *
 * 广播是显式的（setPackage 指定目标），不受 Android 8+ 隐式广播限制；
 * 本应用有 SYSTEM_ALERT_WINDOW 权限，Android 12+ 也豁免后台启动 FGS 限制。
 */
class GameLaunchReceiver : BroadcastReceiver() {

    companion object {
        const val ACTION = "com.moss.mmdruntime.action.GAME_LAUNCHED"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return
        android.util.Log.i("GameLaunchReceiver", "game launched, serviceRunning=${FloatingWindowService.isRunning}")
        try {
            if (FloatingWindowService.isRunning) {
                FloatingWindowService.expandFromGame()
            } else {
                val i = Intent(context, FloatingWindowService::class.java)
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                    context.startForegroundService(i)
                } else {
                    context.startService(i)
                }
            }
        } catch (t: Throwable) {
            android.util.Log.e("GameLaunchReceiver", "start failed", t)
        }
    }
}
