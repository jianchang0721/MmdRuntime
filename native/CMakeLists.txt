cmake_minimum_required(VERSION 3.18)
project(mmdruntime CXX)

# ---------------------------------------------------------------------
# 游戏侧 so（注入 com.hermes.h1game 进程）
#
# 只管三件事：PMX 解析、il2cpp 绑定、SMR 内存操作。
# 不包含任何 UI、不 hook 任何 GL 函数 —— UI 在 App 自己的悬浮窗里
#（libimgui_overlay.so，由 app/src/main/cpp_ui 构建）。
# ---------------------------------------------------------------------

add_library(mmdruntime SHARED
    module.cpp
    mmd/pmx.cpp
)

target_include_directories(mmdruntime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mmdruntime PRIVATE -O2 -std=c++17)

find_library(log-lib log)
target_link_libraries(mmdruntime ${log-lib})

# 不要 -fvisibility=hidden：JNI 符号要导出
