package com.moss.mmdruntime.service

import android.view.WindowManager
import kotlin.math.roundToInt

/**
 * 悬浮窗边界管理。
 *
 * 关键改动：不再把「屏幕尺寸读不到」（0）悄悄修成 320/420 —— 那正是
 * build-19 里窗口塌到 320x420 还查不出原因的元凶。现在 0 就是非法，
 * 调用方必须先保证 OverlayMetrics 给了有效值。
 */
class OverlayBoundsManager(
    private var screenWidth: Int,
    private var screenHeight: Int
) {

    /** 尺寸变化小于这个像素数就不提交给 WindowManager，避免逐帧抖动 */
    private val resizeThresholdPx = 4

    fun updateScreen(width: Int, height: Int) {
        require(width > 0 && height > 0) { "invalid screen ${width}x${height}" }
        screenWidth = width
        screenHeight = height
    }

    fun screenWidth(): Int = screenWidth
    fun screenHeight(): Int = screenHeight

    fun clampX(x: Int, width: Int): Int = x.coerceIn(0, (screenWidth - width).coerceAtLeast(0))
    fun clampY(y: Int, height: Int): Int = y.coerceIn(0, (screenHeight - height).coerceAtLeast(0))
    fun clampWidth(width: Int): Int = width.coerceIn(1, screenWidth)
    fun clampHeight(height: Int): Int = height.coerceIn(1, screenHeight)

    fun centerX(width: Int): Int = ((screenWidth - width) * 0.5f).roundToInt().coerceAtLeast(0)
    fun centerY(height: Int): Int = ((screenHeight - height) * 0.5f).roundToInt().coerceAtLeast(0)

    /**
     * 把 native 上报的目标尺寸套用到 LayoutParams。
     * 返回 true 表示确实变了、需要 updateViewLayout。
     */
    fun applyDesiredBounds(params: WindowManager.LayoutParams, desired: FloatArray): Boolean {
        if (desired.size < 2) return false
        val rawW = desired[0]
        val rawH = desired[1]
        if (!rawW.isFinite() || !rawH.isFinite()) return false

        val nextW = clampWidth(rawW.roundToInt())
        val nextH = clampHeight(rawH.roundToInt())

        val sizeChanged = kotlin.math.abs(params.width - nextW) >= resizeThresholdPx ||
            kotlin.math.abs(params.height - nextH) >= resizeThresholdPx

        // 尺寸变了就保持中心不动，不然每次重算窗口都会往左上角飘
        val nextX = if (sizeChanged) recenterX(params, nextW) else params.x
        val nextY = if (sizeChanged) recenterY(params, nextH) else params.y
        val clampedX = clampX(nextX, nextW)
        val clampedY = clampY(nextY, nextH)

        if (!sizeChanged && params.x == clampedX && params.y == clampedY) return false

        params.width = nextW
        params.height = nextH
        params.x = clampedX
        params.y = clampedY
        return true
    }

    private fun recenterX(params: WindowManager.LayoutParams, nextW: Int): Int {
        if (params.width == 0) return centerX(nextW)
        val cx = params.x + params.width / 2.0f
        return (cx - nextW / 2.0f).roundToInt()
    }

    private fun recenterY(params: WindowManager.LayoutParams, nextH: Int): Int {
        if (params.height == 0) return centerY(nextH)
        val cy = params.y + params.height / 2.0f
        return (cy - nextH / 2.0f).roundToInt()
    }
}
