cmake_minimum_required(VERSION 3.20)

project(KJ)

add_executable(KJ src/main.cpp)

# === Steinberg VST3 SDK include paths ===
set(VST3_SDK_DIR "${CMAKE_SOURCE_DIR}/external/vst3sdk")
target_include_directories(KJ PRIVATE
    ${VST3_SDK_DIR}
    ${VST3_SDK_DIR}/pluginterfaces
    ${VST3_SDK_DIR}/base
    ${VST3_SDK_DIR}/base/source
    ${VST3_SDK_DIR}/public.sdk
    ${VST3_SDK_DIR}/public.sdk/source/vst
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility
)

include_directories(
    ${VST3_SDK_DIR}
    ${VST3_SDK_DIR}/pluginterfaces
)

# === Steinberg VST3 SDK Include Paths ===

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        STRICT
        UNICODE
        _UNICODE
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/O2 /EHsc /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(MINGW)
        add_compile_options(-O2 -std=c++20 -Wall)
    endif()
endif()

target_include_directories(KJ PRIVATE
    include
    ${VST3_SDK_DIR}
    ${VST3_SDK_DIR}/public.sdk
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting
    ${CMAKE_SOURCE_DIR}/external/vst3sdk/public.sdk/source/vst/hosting
)
target_sources(KJ PRIVATE src/hosting/VST3Host.cpp)

add_subdirectory(external/vst3sdk)

target_link_libraries(KJ PRIVATE sdk)

add_subdirectory(src/core)
add_subdirectory(src/gui)

target_link_libraries(KJ PRIVATE kj_core kj_gui)
