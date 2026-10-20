cmake_minimum_required(VERSION 3.18)
project(mmdruntime CXX)

# ---------------------------------------------------------------------
# 游戏侧 so（注入 com.hermes.h1game 进程）
#
# 职责：PMX 解析、il2cpp 绑定、SMR 劫持（含 m_Bones 扩容 hook）。
# 不含任何 UI —— UI 在 App 自己的悬浮窗里（libimgui_overlay.so）。
# ---------------------------------------------------------------------

add_library(mmdruntime SHARED
    module.cpp
    mmd/pmx.cpp
    bone_hook.cpp
)

target_include_directories(mmdruntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)

target_compile_options(mmdruntime PRIVATE -O2 -std=c++17)

find_library(log-lib log)

target_link_libraries(mmdruntime
    ${log-lib}
    dl
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dobby/arm64-v8a/libdobby.a
)

# 不要 -fvisibility=hidden：JNI 符号要导出
