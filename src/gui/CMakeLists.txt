file(GLOB LICE_SRC "${CMAKE_SOURCE_DIR}/external/wdl/lice/*.cpp")

add_library(kj_gui STATIC gui_main.cpp ${LICE_SRC})
target_include_directories(kj_gui PRIVATE ../../include ../../external)
target_link_libraries(kj_gui PRIVATE opengl32 gdi32 user32)
