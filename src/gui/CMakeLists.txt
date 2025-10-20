add_library(kj_gui gui_main.cpp ../../external/wdl/lice/lice.cpp)
target_include_directories(kj_gui PRIVATE ../../include ../../external)
target_link_libraries(kj_gui PRIVATE opengl32 gdi32 user32)
