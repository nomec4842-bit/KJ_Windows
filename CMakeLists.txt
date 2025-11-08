cmake_minimum_required(VERSION 3.20)

project(KJ)

add_executable(KJ src/main.cpp)

# --- Force global include for Steinberg VST3 SDK ---
set(VST3_SDK_DIR "${CMAKE_SOURCE_DIR}/external/vst3sdk")

include_directories(${VST3_SDK_DIR})
include_directories(${VST3_SDK_DIR}/pluginterfaces)
include_directories(${VST3_SDK_DIR}/public.sdk/source/vst)
include_directories(${VST3_SDK_DIR}/public.sdk/source/vst/hosting)

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

if(NOT MSVC)
    message(FATAL_ERROR "This project is configured for MSVC on Windows only when using the trimmed VST3 SDK.")
endif()

add_compile_options(/O2 /EHsc /permissive-)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

target_sources(KJ PRIVATE src/hosting/VST3Host.cpp)

# Add Steinberg SDK
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "Enable Steinberg VST3 plug-in examples" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "Enable Steinberg VST3 hosting examples" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "Enable Steinberg VSTGUI support" FORCE)
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
