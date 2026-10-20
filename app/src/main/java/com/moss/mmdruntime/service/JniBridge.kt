package com.moss.mmdruntime.service

/**
 * UI so（libimgui_overlay.so）的 JNI 入口。
 *
 * 这个 so 不靠 System.loadLibrary 加载 —— 它跟游戏没有关系，
 * 游戏进程的 nativeLibraryDir 里也没有它。
 * HookEntry 会把两个 so 都复制到游戏私有目录后用 System.load(绝对路径) 加载，
 * 加载之后这里的 native 方法才能解析。
 */
object JniBridge {

    external fun setFontPaths(latinPath: String, cjkPath: String)
    external fun setConfigDirectory(path: String)

    /** 返回 true = 这个触摸归菜单，游戏别收到 */
    external fun onTouchEvent(action: Int, x: Float, y: Float, viewW: Float, viewH: Float): Boolean
    external fun onKeyEvent(unicodeChar: Int, keyCode: Int, isDown: Boolean)
    external fun wantTextInput(): Boolean

    /** ImGui 上下文和 GL 后端都就绪了 */
    external fun isReady(): Boolean
}
