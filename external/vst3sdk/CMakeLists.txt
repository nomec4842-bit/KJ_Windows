cmake_minimum_required(VERSION 3.20)
project(vstsdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The trimmed SDK snapshot keeps only the pieces required for hosting VST3 plug-ins:
# - pluginterfaces/:    Public VST interfaces shared between hosts and plug-ins.
# - base/:              Steinberg foundation utilities depended on by the interfaces.
# - public.sdk/...      Hosting helpers and utility code (including the former
#                       public.sdk/source/common/ tree relocated under utility/common).
#
# Everything else from the upstream repository (samples, validators, wrappers, etc.)
# has been removed to keep the vendored dependency light-weight for the KJ host build.

set(VST3SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(VST3SDK_INCLUDE_DIRS
    ${VST3SDK_ROOT}
    ${VST3SDK_ROOT}/pluginterfaces
    ${VST3SDK_ROOT}/base
    ${VST3SDK_ROOT}/base/source
    ${VST3SDK_ROOT}/base/thread/include
    ${VST3SDK_ROOT}/public.sdk/source/vst/hosting
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/common
)

file(GLOB_RECURSE base_sources CONFIGURE_DEPENDS
    ${VST3SDK_ROOT}/base/source/*.cpp
    ${VST3SDK_ROOT}/base/source/*.c
    ${VST3SDK_ROOT}/base/thread/source/*.cpp
    ${VST3SDK_ROOT}/base/thread/source/*.c
)

file(GLOB_RECURSE pluginterfaces_sources CONFIGURE_DEPENDS
    ${VST3SDK_ROOT}/pluginterfaces/*.cpp
)

file(GLOB_RECURSE hosting_sources CONFIGURE_DEPENDS
    ${VST3SDK_ROOT}/public.sdk/source/vst/hosting/*.cpp
    ${VST3SDK_ROOT}/public.sdk/source/vst/hosting/*.mm
)

file(GLOB_RECURSE utility_sources CONFIGURE_DEPENDS
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/*.cpp
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/*.c
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/*.mm
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/common/*.cpp
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/common/*.c
    ${VST3SDK_ROOT}/public.sdk/source/vst/utility/common/*.mm
)

set(hosting_common_sources)
set(hosting_platform_sources)
foreach(src ${hosting_sources})
    get_filename_component(filename ${src} NAME)
    if(filename MATCHES "_win32\\.cpp$")
        if(WIN32)
            list(APPEND hosting_platform_sources ${src})
        endif()
    elseif(filename MATCHES "_linux\\.cpp$")
        if(UNIX AND NOT APPLE)
            list(APPEND hosting_platform_sources ${src})
        endif()
    elseif(filename MATCHES "_mac\\.(mm|cpp)$")
        if(APPLE)
            list(APPEND hosting_platform_sources ${src})
        endif()
    else()
        list(APPEND hosting_common_sources ${src})
    endif()
endforeach()

set(utility_common_sources)
set(utility_platform_sources)
foreach(src ${utility_sources})
    get_filename_component(filename ${src} NAME)
    if(filename MATCHES "_win32\\.cpp$")
        if(WIN32)
            list(APPEND utility_platform_sources ${src})
        endif()
    elseif(filename MATCHES "_linux\\.cpp$")
        if(UNIX AND NOT APPLE)
            list(APPEND utility_platform_sources ${src})
        endif()
    elseif(filename MATCHES "_mac\\.(mm|cpp)$")
        if(APPLE)
            list(APPEND utility_platform_sources ${src})
        endif()
    else()
        list(APPEND utility_common_sources ${src})
    endif()
endforeach()

add_library(sdk STATIC
    ${base_sources}
    ${pluginterfaces_sources}
    ${hosting_common_sources}
    ${utility_common_sources}
    ${hosting_platform_sources}
    ${utility_platform_sources}
)

target_include_directories(sdk
    PUBLIC
        ${VST3SDK_INCLUDE_DIRS}
)

target_compile_features(sdk PUBLIC cxx_std_17)

target_compile_definitions(sdk
    PUBLIC
        DEVELOPMENT=1
        $<$<CONFIG:Debug>:DEVELOPMENT=1>
        $<$<CONFIG:RelWithDebInfo>:DEVELOPMENT=1>
        $<$<CONFIG:Release>:DEVELOPMENT=0>
        $<$<CONFIG:Release>:RELEASE=1>
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:MinSizeRel>:DEVELOPMENT=0>
        $<$<CONFIG:MinSizeRel>:RELEASE=1>
        $<$<CONFIG:MinSizeRel>:NDEBUG>
)

if(MSVC)
    target_compile_definitions(sdk PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
    target_link_libraries(sdk PUBLIC user32)
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(sdk PUBLIC Threads::Threads dl)
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    target_link_libraries(sdk PUBLIC ${COCOA_LIBRARY})
endif()

add_library(vstsdk::sdk ALIAS sdk)

install(TARGETS sdk EXPORT vstsdkTargets)
