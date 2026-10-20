cmake_minimum_required(VERSION 3.18)
project(mmdruntime CXX)

add_library(mmdruntime SHARED
    module.cpp
    mmd/pmx.cpp
)

target_include_directories(mmdruntime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(mmdruntime PRIVATE -O2 -std=c++17)

find_library(log-lib log)
target_link_libraries(mmdruntime ${log-lib})

# 不要 -fvisibility=hidden：JNI 符号要导出
