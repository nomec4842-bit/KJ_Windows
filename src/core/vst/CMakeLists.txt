add_library(kj_delay SHARED delay_effect.cpp)
target_include_directories(kj_delay PRIVATE ../../../include)
set_target_properties(kj_delay PROPERTIES
    OUTPUT_NAME "kj_delay"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
