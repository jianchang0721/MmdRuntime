cmake_minimum_required(VERSION 3.18)
project(imgui_overlay CXX)

# ---------------------------------------------------------------
# ImGui 悬浮窗 UI（跑在 MmdRuntime 自己的 App 进程里，不进游戏）
# 架构：WindowManager 加一个 GLSurfaceView → ImGui 画在上面
# 游戏逻辑在 libmmdruntime.so（注入进程），两边靠配置文件通信
# ---------------------------------------------------------------

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

add_library(imgui_overlay SHARED
    native_imgui.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/screens
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(imgui_overlay PRIVATE
    IMGUI_IMPL_OPENGL_ES3
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)

target_compile_options(imgui_overlay PRIVATE -O2 -std=c++17 -fexceptions -frtti)

target_link_libraries(imgui_overlay GLESv3 EGL android log)
