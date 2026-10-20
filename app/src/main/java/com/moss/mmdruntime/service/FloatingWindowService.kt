package com.moss.mmdruntime.service

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.content.res.Configuration
import android.graphics.PixelFormat
import android.opengl.GLSurfaceView
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import androidx.core.app.NotificationCompat
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.roundToInt

class FloatingWindowService : Service() {
    private lateinit var windowManager: WindowManager
    private lateinit var glSurfaceView: TemplateGLSurfaceView
    private lateinit var params: WindowManager.LayoutParams
    private val mainHandler = Handler(Looper.getMainLooper())
    private var lastWantTextInput = false
    private var touchMode = TOUCH_NONE
    private var dragStartRawX = 0f
    private var dragStartRawY = 0f
    private var dragStartX = 0
    private var dragStartY = 0
    private var screenWidth = 1080
    private var screenHeight = 2400
    private lateinit var boundsManager: OverlayBoundsManager

    private companion object {
        const val TAG = "FloatingWindowService"
        const val CHANNEL_ID = "floating_window_channel"
        const val TOUCH_NONE = 0
        const val TOUCH_DRAG = 1
        const val DRAG_HANDLE_HEIGHT = 56f
        const val CLOSE_ZONE_WIDTH = 120f
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /** 桥目录在 /data/local/tmp 下，App uid 没有写权限建不了目录；借 root 建一次。 */
    private fun ensureBridgeDirectory() {
        Thread {
            try {
                Runtime.getRuntime()
                    .exec(arrayOf(
                        "su", "-c",
                        "mkdir -p /data/local/tmp/mmdrt && " +
                        "chmod 777 /data/local/tmp/mmdrt && " +
                        "chcon u:object_r:app_data_file:s0 /data/local/tmp/mmdrt 2>/dev/null; " +
                        "chown root:root /data/local/tmp/mmdrt"
                    ))
                    .waitFor()
            } catch (_: Throwable) {
                // KernelSU 没授权就建不了，桥不可用但 UI 照常跑
            }
        }.start()
    }

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(WINDOW_SERVICE) as WindowManager
        ensureBridgeDirectory()

        // 屏幕尺寸必须在 native 读取 shape 之前定下来。
        // Service 没有关联窗口，单看 resources.displayMetrics 经常拿到 0。
        val display = OverlayMetrics.resolve(this)
        android.util.Log.i(TAG, "screen = $display")
        screenWidth = display.width
        screenHeight = display.height
        boundsManager = OverlayBoundsManager(screenWidth, screenHeight)
        JniBridge.setScreenSize(screenWidth, screenHeight)
        JniBridge.setConfigDirectory(File(filesDir, "configs").absolutePath)
        updateOverlayRuntimeState(surfaceActive = false)
        setupForegroundNotification()
        prepareImGuiFonts()
        setupOverlay()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val display = OverlayMetrics.resolve(this)
        screenWidth = display.width
        screenHeight = display.height
        boundsManager.updateScreen(screenWidth, screenHeight)
        JniBridge.setScreenSize(screenWidth, screenHeight)
        updateOverlayRuntimeState(surfaceActive = ::glSurfaceView.isInitialized && glSurfaceView.isAttachedToWindow)
        if (::params.isInitialized && ::glSurfaceView.isInitialized && glSurfaceView.isAttachedToWindow) {
            val desired = try { JniBridge.getDesiredWindowSize() } catch (_: Exception) { floatArrayOf(params.width.toFloat(), params.height.toFloat()) }
            if (boundsManager.applyDesiredBounds(params, desired)) {
                windowManager.updateViewLayout(glSurfaceView, params)
            }
        }
    }

    private fun setupForegroundNotification() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Floating Window Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply { description = "Running Dear ImGui overlay template" }
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(channel)
        }

        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Floating Overlay Template")
            .setContentText("Dear ImGui template is running.")
            .setSmallIcon(android.R.drawable.ic_menu_info_details)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                startForeground(1001, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            } else {
                startForeground(1001, notification)
            }
        } else {
            startForeground(1001, notification)
        }
    }

    private fun prepareImGuiFonts() {
        // stb_truetype 处理不了系统 TTC 集合包（NotoSansCJK-Regular.ttc 是 5 套字体合一），
        // ImGui::NewFrame 建图集时会 null deref 崩。用户给了单体 TTF gkai00mp-2.ttf
        // （AR PL KaitiM GB，4.6MB），这里拷出来当 CJK 字形源。
        //
        // 注意 native 侧是「拉丁打底 + CJK 合并」：CJK 字体里的拉丁字形偏窄且缺
        // 常规符号，必须让 Nunito 当基字体，否则数字和 % 会变成方块。
        val cjk = copyAssetFontToFiles("fonts/cjk.ttf")
        val latin = try {
            copyAssetFontToFiles("fonts/Nunito-Regular.ttf")
        } catch (_: Throwable) { File("") }

        val usableLatin = if (latin.exists() && latin.length() > 10_000) latin.absolutePath else ""
        val usableCjk = if (cjk.exists() && cjk.length() > 100_000) cjk.absolutePath else ""
        android.util.Log.i(TAG, "fonts latin=${usableLatin.ifEmpty { "(builtin)" }} cjk=${usableCjk.ifEmpty { "(none)" }}")
        JniBridge.setFontPaths(usableLatin, usableCjk)
    }

    private fun copyAssetFontToFiles(assetPath: String): File {
        val output = File(filesDir, assetPath.substringAfterLast('/'))
        assets.open(assetPath).use { input ->
            output.outputStream().use { output -> input.copyTo(output) }
        }
        return output
    }

    private fun setupOverlay() {
        val layoutFlag = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
        } else {
            @Suppress("DEPRECATION")
            WindowManager.LayoutParams.TYPE_PHONE
        }

        // 先按屏幕算一个合理初值；真正的尺寸由 native 的 SizeFor 决定，
        // setupOverlay 之后第一帧 syncActiveWindowBounds 会纠正过来。
        val initialWidth = (screenWidth * 0.86f).roundToInt().coerceIn(320, screenWidth)
        val initialHeight = (screenHeight * 0.44f).roundToInt().coerceIn(360, screenHeight)

        // The overlay view is intentionally sized to the active ImGui window, not fullscreen.
        // Android naturally passes touches outside this rectangle through to the app/game below.
        params = WindowManager.LayoutParams(
            initialWidth,
            initialHeight,
            layoutFlag,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = boundsManager.centerX(initialWidth)
            y = boundsManager.centerY(initialHeight)
        }

        glSurfaceView = TemplateGLSurfaceView(this).apply {
            setEGLConfigChooser(8, 8, 8, 8, 16, 0)
            setEGLContextClientVersion(3)
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                    updateOverlayRuntimeState(surfaceActive = true)
                    JniBridge.initImGui()
                }

                override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
                    updateOverlayRuntimeState(surfaceActive = true)
                    JniBridge.resizeImGui(width, height)
                }

                override fun onDrawFrame(gl: GL10?) {
                    JniBridge.stepAndRenderImGui()
                    syncActiveWindowBounds()
                    checkKeyboardFocus()
                    if (JniBridge.checkCloseRequested()) {
                        mainHandler.post { stopSelf() }
                    }
                }
            })
            holder.setFormat(PixelFormat.TRANSLUCENT)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        dragStartRawX = event.rawX
                        dragStartRawY = event.rawY
                        dragStartX = params.x
                        dragStartY = params.y
                        touchMode = if (
                            event.y <= DRAG_HANDLE_HEIGHT &&
                            event.x <= glSurfaceView.width - CLOSE_ZONE_WIDTH
                        ) TOUCH_DRAG else TOUCH_NONE
                        if (touchMode == TOUCH_DRAG) return@setOnTouchListener true
                    }

                    MotionEvent.ACTION_MOVE -> {
                        if (touchMode == TOUCH_DRAG) {
                            val dx = event.rawX - dragStartRawX
                            val dy = event.rawY - dragStartRawY
                            params.x = boundsManager.clampX(dragStartX + dx.roundToInt(), params.width)
                            params.y = boundsManager.clampY(dragStartY + dy.roundToInt(), params.height)
                            if (glSurfaceView.isAttachedToWindow) {
                                windowManager.updateViewLayout(glSurfaceView, params)
                            }
                            return@setOnTouchListener true
                        }
                    }

                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        if (touchMode == TOUCH_DRAG) {
                            touchMode = TOUCH_NONE
                            return@setOnTouchListener true
                        }
                        touchMode = TOUCH_NONE
                    }
                }

                val action = when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> 0
                    MotionEvent.ACTION_UP -> 1
                    MotionEvent.ACTION_MOVE -> 2
                    else -> -1
                }
                if (action != -1) JniBridge.onTouchEvent(action, event.x, event.y)
                true
            }
        }

        windowManager.addView(glSurfaceView, params)
        updateOverlayRuntimeState(surfaceActive = true)
    }

    private fun updateOverlayRuntimeState(surfaceActive: Boolean) {
        val canDraw = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Settings.canDrawOverlays(this)
        } else {
            true
        }
        JniBridge.updateOverlayRuntimeState(
            overlayPermission = canDraw,
            drawOverApps = canDraw,
            surfaceActive = surfaceActive,
            appForeground = true,
            width = screenWidth,
            height = screenHeight
        )
    }

    private fun syncActiveWindowBounds() {
        if (touchMode == TOUCH_DRAG || !::glSurfaceView.isInitialized || !glSurfaceView.isAttachedToWindow) return
        val desired = try { JniBridge.getDesiredWindowSize() } catch (_: Exception) { return }

        mainHandler.post {
            if (!glSurfaceView.isAttachedToWindow) return@post
            if (boundsManager.applyDesiredBounds(params, desired)) {
                windowManager.updateViewLayout(glSurfaceView, params)
            }
        }
    }

    private fun setWindowFocusable(focusable: Boolean) {
        val isFocusable = (params.flags and WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0
        if (focusable == isFocusable) return
        params.flags = if (focusable) {
            params.flags and WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE.inv()
        } else {
            params.flags or WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
        }
        if (glSurfaceView.isAttachedToWindow) {
            windowManager.updateViewLayout(glSurfaceView, params)
        }
    }

    private fun checkKeyboardFocus() {
        val wantTextInput = try { JniBridge.wantTextInput() } catch (_: Exception) { false }
        if (wantTextInput == lastWantTextInput) return

        lastWantTextInput = wantTextInput
        mainHandler.post {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            if (wantTextInput) {
                setWindowFocusable(true)
                glSurfaceView.postDelayed({
                    glSurfaceView.requestFocus()
                    imm.showSoftInput(glSurfaceView, InputMethodManager.SHOW_IMPLICIT)
                }, 50)
            } else {
                imm.hideSoftInputFromWindow(glSurfaceView.windowToken, 0)
                setWindowFocusable(false)
            }
        }
    }

    override fun onDestroy() {
        updateOverlayRuntimeState(surfaceActive = false)
        if (::glSurfaceView.isInitialized && glSurfaceView.isAttachedToWindow) {
            windowManager.removeView(glSurfaceView)
        }
        super.onDestroy()
    }
}

class TemplateGLSurfaceView(context: Context) : GLSurfaceView(context) {
    init {
        isFocusableInTouchMode = true
        isFocusable = true
    }

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE
        return object : BaseInputConnection(this, true) {
            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                text?.forEach { char ->
                    JniBridge.onKeyEvent(char.code, 0, true)
                    JniBridge.onKeyEvent(char.code, 0, false)
                }
                return super.commitText(text, newCursorPosition)
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                repeat(beforeLength) {
                    JniBridge.onKeyEvent(0, KeyEvent.KEYCODE_DEL, true)
                    JniBridge.onKeyEvent(0, KeyEvent.KEYCODE_DEL, false)
                }
                return super.deleteSurroundingText(beforeLength, afterLength)
            }
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val isDown = event.action == KeyEvent.ACTION_DOWN
        JniBridge.onKeyEvent(event.getUnicodeChar(), event.keyCode, isDown)
        return if (JniBridge.wantTextInput()) true else super.dispatchKeyEvent(event)
    }
}
