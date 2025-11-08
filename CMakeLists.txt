cmake_minimum_required(VERSION 3.20)

project(KJ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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

add_subdirectory(external/vst3sdk)
add_subdirectory(src/core)
add_subdirectory(src/gui)

add_executable(KJ src/main.cpp)
target_include_directories(KJ PRIVATE include ${CMAKE_SOURCE_DIR}/external/vst3sdk)
target_sources(KJ PRIVATE src/hosting/VST3Host.cpp)
target_link_libraries(KJ PRIVATE kj_core kj_gui sdk)
