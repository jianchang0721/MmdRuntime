cmake_minimum_required(VERSION 3.18)
project(mmdruntime CXX)

# ---------------------------------------------------------------------
# 游戏侧 so（注入 com.hermes.h1game 进程）
#
# 分层：
#   mmd/    PMX 解析、骨骼树、Mesh 构建        —— 不依赖游戏
#   unity/  GameObject/Transform/Mesh 封装     —— 只依赖 il2cpp + Unity API
#   game/   SMR 写入、总装流程                 —— 依赖具体游戏
#   bone_hook.cpp  m_Bones 容器析构拦截        —— 扩容的析构保护
#
# 不含任何 UI —— UI 在 App 自己的悬浮窗里（libimgui_overlay.so）。
# ---------------------------------------------------------------------

add_library(mmdruntime SHARED
    module.cpp
    bone_hook.cpp
    mmd/pmx.cpp
    mmd/skeleton.cpp
    mmd/mesh_builder.cpp
    mmd/texture.cpp
    mmd/material.cpp
    logsink.cpp
    stealth.cpp
    unity/unity_objects.cpp
    game/smr_bind.cpp
    game/loader.cpp
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
