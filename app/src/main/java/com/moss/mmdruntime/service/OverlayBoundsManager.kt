package com.moss.mmdruntime.service

import android.view.WindowManager
import kotlin.math.roundToInt

class OverlayBoundsManager(
    private var screenWidth: Int,
    private var screenHeight: Int
) {
    fun updateScreen(width: Int, height: Int) {
        screenWidth = width.coerceAtLeast(1)
        screenHeight = height.coerceAtLeast(1)
    }

    fun responsiveWidth(percent: Float, minValue: Int, maxValue: Int): Int {
        val available = (screenWidth - 32).coerceAtLeast(320)
        val desired = (screenWidth * percent).roundToInt().coerceIn(minValue, maxValue)
        return desired.coerceAtMost(available)
    }

    fun responsiveHeight(percent: Float, minValue: Int, maxValue: Int): Int {
        val available = (screenHeight - 48).coerceAtLeast(420)
        val desired = (screenHeight * percent).roundToInt().coerceIn(minValue, maxValue)
        return desired.coerceAtMost(available)
    }

    fun centerX(width: Int): Int = ((screenWidth - width) * 0.5f).roundToInt().coerceAtLeast(0)

    fun centerY(height: Int): Int = ((screenHeight - height) * 0.5f).roundToInt().coerceAtLeast(0)

    fun clampX(x: Int, width: Int): Int = x.coerceIn(0, (screenWidth - width).coerceAtLeast(0))

    fun clampY(y: Int, height: Int): Int = y.coerceIn(0, (screenHeight - height).coerceAtLeast(0))

    fun clampWidth(width: Int): Int = width.coerceIn(1, screenWidth)

    fun clampHeight(height: Int): Int = height.coerceIn(1, screenHeight)

    fun applyDesiredBounds(params: WindowManager.LayoutParams, desired: FloatArray): Boolean {
        if (desired.size < 2) return false

        val nextW = clampWidth(desired[0].roundToInt())
        val nextH = clampHeight(desired[1].roundToInt())
        val nextX = clampX(params.x, nextW)
        val nextY = clampY(params.y, nextH)
        if (params.width == nextW && params.height == nextH && params.x == nextX && params.y == nextY) {
            return false
        }

        params.width = nextW
        params.height = nextH
        params.x = nextX
        params.y = nextY
        return true
    }
}
