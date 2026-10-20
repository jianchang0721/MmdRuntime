cmake_minimum_required(VERSION 3.18)
project(mmdruntime CXX)

add_library(mmdruntime SHARED
    module.cpp
    overlay.cpp
    mmd/pmx.cpp
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/backends/imgui_impl_opengl3.cpp
)

target_include_directories(mmdruntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui/backends
)

target_compile_options(mmdruntime PRIVATE
    -O2 -std=c++17
    -DIMGUI_IMPL_OPENGL_ES3
    -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)

target_link_libraries(mmdruntime
    ${log-lib} ${android-lib} ${egl-lib} ${gles-lib}
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dobby/arm64-v8a/libdobby.a
    dl
)

# 不要 -fvisibility=hidden：JNI 符号要导出
