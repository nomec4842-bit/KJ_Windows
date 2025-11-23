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
    set(_kj_output_platform "${CMAKE_VS_PLATFORM_NAME}")
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

add_executable(KJ
    src/main.cpp
    src/hosting/VST3Host.cpp

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
    PRIVATE kj_core kj_gui
    sdk_common sdk_hosting pluginterfaces base
)

message(STATUS "KJ configured with VST3 SDK: ${VST3_SDK_DIR}")
