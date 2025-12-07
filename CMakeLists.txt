cmake_minimum_required(VERSION 3.20)
project(KJ VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
        _CRT_SECURE_NO_WARNINGS
        SMTG_OS_WINDOWS=1 SMTG_PLATFORM_WINDOWS=1
    )
endif()

set(VST3_SDK_DIR "${CMAKE_SOURCE_DIR}/external/vst3sdk")

# Keep build products for different architectures separated to avoid mixing x86 and
# x64 artifacts when reusing a build directory.
if (MSVC)
    # Prefer the generator platform when available to avoid mixing x86 and x64
    # artifacts in the same build tree. Reusing a build directory across
    # architectures can leave behind incompatible libraries (for example,
    # base.lib), which later manifest as "invalid or corrupt file" linker
    # errors on Windows.
    if (CMAKE_GENERATOR_PLATFORM)
        set(_kj_output_platform "${CMAKE_GENERATOR_PLATFORM}")
    else()
        set(_kj_output_platform "${CMAKE_VS_PLATFORM_NAME}")
    endif()

    if (KJ_MSVC_PLATFORM_CACHE AND NOT KJ_MSVC_PLATFORM_CACHE STREQUAL _kj_output_platform)
        message(FATAL_ERROR "Build directory was previously configured for platform '${KJ_MSVC_PLATFORM_CACHE}' but is now configured for '${_kj_output_platform}'. Please use a fresh build directory per architecture to avoid stale artifacts.")
    endif()

    set(KJ_MSVC_PLATFORM_CACHE "${_kj_output_platform}" CACHE STRING "Last configured MSVC platform" FORCE)
else()
    set(_kj_output_platform "${CMAKE_SYSTEM_PROCESSOR}")
endif()

foreach(_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/bin/${_kj_output_platform}/${_config}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/lib/${_kj_output_platform}/${_config}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/lib/${_kj_output_platform}/${_config}")
endforeach()

include_directories(
    ${VST3_SDK_DIR}
    ${VST3_SDK_DIR}/pluginterfaces
    ${VST3_SDK_DIR}/public.sdk/source
    ${VST3_SDK_DIR}/public.sdk/source/vst
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

add_subdirectory(external/vst3sdk)
add_subdirectory(src/core)
add_subdirectory(src/gui)

add_library(kj_hosting STATIC
    src/hosting/VST3AsyncLoader.cpp
    src/hosting/VST3Host.cpp
    src/hosting/VST3PlugFrame.cpp
    src/hosting/VstParameterQueue.cpp
    src/hosting/VSTEditorWindow.cpp
)

if (WIN32)
    target_sources(kj_hosting PRIVATE src/hosting/VSTGuiThread.cpp)

    # Ensure the Windows-specific hosting sources are compiled with the
    # expected platform macro.  Some toolchains (for example, clang-cl) do
    # not automatically provide _WIN32, which leaves the guarded
    # implementations in VSTGuiThread.cpp and VSTEditorWindow.cpp empty and
    # results in unresolved externals when linking KJ.exe.
    target_compile_definitions(kj_hosting PRIVATE _WIN32)
endif()

target_link_libraries(kj_hosting
    PUBLIC kj_core kj_gui
    sdk_common sdk_hosting pluginterfaces base
)

add_executable(KJ
    src/main.cpp

    # Force-include full Steinberg hosting stack
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_DIR}/public.sdk/source/vst/hosting/processdata.cpp
)

target_link_libraries(KJ
    PRIVATE kj_hosting
)

if (MSVC)
    # Ensure all GUI symbols are available when linking the Windows executable.
    # This avoids missing exports like requestMainMenuRefresh() that may
    # otherwise be omitted when kj_gui is consumed as a static library.
    target_link_options(KJ PRIVATE "/WHOLEARCHIVE:kj_gui")
endif()

if (WIN32)
    add_executable(kj_hosting_tests
        src/hosting/tests/VSTGuiThreadTests.cpp
    )

    target_link_libraries(kj_hosting_tests
        PRIVATE kj_hosting
    )
endif()

message(STATUS "KJ configured with VST3 SDK: ${VST3_SDK_DIR}")
