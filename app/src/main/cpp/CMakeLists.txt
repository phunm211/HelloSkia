cmake_minimum_required(VERSION 3.10)

project(native-lib)

add_library(native-lib SHARED native_renderer.cpp)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)

target_include_directories(native-lib PRIVATE
        ${CMAKE_SOURCE_DIR}/skia/include
        ${CMAKE_SOURCE_DIR}/skia
        ${CMAKE_SOURCE_DIR}/skia/include/core
)

target_link_libraries(native-lib
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${glesv3-lib}
        ${CMAKE_SOURCE_DIR}/skia/out/Debug-android-arm64/libskia.a
)
