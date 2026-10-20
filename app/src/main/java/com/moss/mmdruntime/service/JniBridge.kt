package com.moss.mmdruntime.service

object JniBridge {
    init {
        System.loadLibrary("imgui_overlay")
    }

    external fun setFontPaths(bodyPath: String, headingPath: String)
    external fun setConfigDirectory(path: String)
    external fun updateOverlayRuntimeState(
        overlayPermission: Boolean,
        drawOverApps: Boolean,
        surfaceActive: Boolean,
        appForeground: Boolean,
        width: Int,
        height: Int
    )
    external fun setScreenSize(width: Int, height: Int)
    external fun initImGui()
    external fun resizeImGui(width: Int, height: Int)
    external fun stepAndRenderImGui()
    external fun onTouchEvent(action: Int, x: Float, y: Float)
    external fun onKeyEvent(unicodeChar: Int, keyCode: Int, isDown: Boolean)
    external fun wantTextInput(): Boolean
    external fun checkCloseRequested(): Boolean
    external fun getDesiredWindowSize(): FloatArray
}
