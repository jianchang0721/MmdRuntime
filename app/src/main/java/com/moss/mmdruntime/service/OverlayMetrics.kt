package com.moss.mmdruntime.service

import android.content.Context
import android.os.Build
import android.util.DisplayMetrics
import android.view.WindowManager
import kotlin.math.min

/**
 * 真实屏幕尺寸解析。
 *
 * 起因：Service 没有关联窗口，resources.displayMetrics / Resources.getSystem()
 * 都可能返回 0，导致悬浮窗被 clamp 到下限还查不出原因。
 * 这里按可靠性从高到低兜底，并把命中的那一级名子返回去，方便 logcat 里
 * 一眼看出走了哪条路。
 */
data class RealDisplay(
    val width: Int,
    val height: Int,
    val density: Float,
    val source: String
) {
    fun isValid(): Boolean = width > 0 && height > 0
    override fun toString(): String = "${width}x${height} density=$density via $source"
}

object OverlayMetrics {

    private const val FALLBACK_W = 1080
    private const val FALLBACK_H = 2400
    private const val FALLBACK_DENSITY = 2.625f

    fun resolve(context: Context): RealDisplay {
        for ((source, pair) in collectSizes(context)) {
            if (pair.first > 0 && pair.second > 0) {
                return RealDisplay(pair.first, pair.second, densityOf(context), source)
            }
        }
        return RealDisplay(FALLBACK_W, FALLBACK_H, FALLBACK_DENSITY, "hardcoded-fallback")
    }

    private fun collectSizes(context: Context): List<Pair<String, Pair<Int, Int>>> {
        val out = ArrayList<Pair<String, Pair<Int, Int>>>(6)

        // 1) API 30+：context 关联的 display，Service 里也能拿到真实分辨率
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching {
                context.display?.let { display ->
                    val dm = DisplayMetrics()
                    display.getRealMetrics(dm)
                    out.add("context.display" to Pair(dm.widthPixels, dm.heightPixels))
                }
            }
        }

        // 2) WindowManager 的 defaultDisplay（含系统装饰区）
        runCatching {
            val wm = context.getSystemService(Context.WINDOW_SERVICE) as? WindowManager
            val dm = DisplayMetrics()
            wm?.defaultDisplay?.getRealMetrics(dm)
            out.add("wm.defaultDisplay" to Pair(dm.widthPixels, dm.heightPixels))
        }

        // 3) 系统 Resources（本 app context 拿不到时的软兜底）
        runCatching {
            val dm = android.content.res.Resources.getSystem().displayMetrics
            out.add("Resources.getSystem" to Pair(dm.widthPixels, dm.heightPixels))
        }
        runCatching {
            val dm = context.resources.displayMetrics
            out.add("context.resources" to Pair(dm.widthPixels, dm.heightPixels))
        }

        return out
    }

    private fun densityOf(context: Context): Float {
        runCatching {
            val d = android.content.res.Resources.getSystem().displayMetrics.density
            if (d > 0.0f) return d
        }
        runCatching {
            val d = context.resources.displayMetrics.density
            if (d > 0.0f) return d
        }
        return FALLBACK_DENSITY
    }
}
