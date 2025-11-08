cmake_minimum_required(VERSION 3.20)

project(KJ)

add_executable(KJ src/main.cpp)

# --- Force global include for Steinberg VST3 SDK ---
set(VST3_SDK_DIR "${CMAKE_SOURCE_DIR}/external/vst3sdk")

include_directories(
    ${VST3_SDK_DIR}
    ${VST3_SDK_DIR}/pluginterfaces
    ${VST3_SDK_DIR}/base
    ${VST3_SDK_DIR}/base/source
    ${VST3_SDK_DIR}/public.sdk
    ${VST3_SDK_DIR}/public.sdk/source
    ${VST3_SDK_DIR}/public.sdk/source/vst
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting
    ${VST3_SDK_DIR}/public.sdk/source/vst/utility
)

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

target_sources(KJ PRIVATE src/hosting/VST3Host.cpp)

# Add Steinberg SDK
add_subdirectory(external/vst3sdk)
target_link_libraries(KJ PRIVATE sdk)

# Core and GUI
add_subdirectory(src/core)
add_subdirectory(src/gui)
target_link_libraries(KJ PRIVATE kj_core kj_gui)

# --- Fix include paths for your project headers ---
target_include_directories(KJ PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/core
    ${CMAKE_SOURCE_DIR}/include/hosting
    ${CMAKE_SOURCE_DIR}/src
)
