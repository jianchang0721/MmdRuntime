cmake_minimum_required(VERSION 3.18)
project(imgui_overlay CXX)

# ---------------------------------------------------------------
# ImGui UI（libimgui_overlay.so）—— 注入游戏进程，hook eglSwapBuffers
# 把菜单画在游戏自己的 GL 帧上；状态/命令直连 libmmdruntime.so 的 mmd_get_api()
#
# 不再有：独立 App 进程、WindowManager 悬浮窗、GLSurfaceView、
#         ui.conf / status.conf 文件通道、广播拉起
# ---------------------------------------------------------------

set(IMGUI_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
set(NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native)

add_library(imgui_overlay SHARED
    host/EglHost.cpp
    bridge/Bridge.cpp
    core/AppState.cpp
    config/ConfigStorage.cpp
    modules/FeatureFactory.cpp
    modules/Modules.cpp
    runtime/ImGuiRuntime.cpp
    runtime/OverlayWindowManager.cpp
    runtime/RecoveryManager.cpp
    runtime/RuntimeState.cpp
    screens/ScreenManager.cpp
    screens/Screens.cpp
    ui/Animation.cpp
    ui/DebugPanel.cpp
    ui/DynamicMenuRenderer.cpp
    ui/Theme.cpp
    ui/ToastManager.cpp
    ui/UIComponents.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_overlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/screens
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${NATIVE_DIR}/public        # mmd_api.h
    ${NATIVE_DIR}/thirdparty    # dobby/dobby.h
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(imgui_overlay PRIVATE
    IMGUI_IMPL_OPENGL_ES3
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)

target_compile_options(imgui_overlay PRIVATE -O2 -std=c++17 -fexceptions -frtti)

target_link_libraries(imgui_overlay
    GLESv3
    EGL
    android
    log
    dl
    ${NATIVE_DIR}/thirdparty/dobby/arm64-v8a/libdobby.a
)
