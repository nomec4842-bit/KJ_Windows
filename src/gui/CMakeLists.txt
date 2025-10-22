add_library(kj_gui STATIC
    gui_main.cpp
    ../../external/wdl/lice/lice.cpp
    ../../external/wdl/lice/lice_text.cpp
    ../../external/wdl/lice/lice_line.cpp
    ../../external/wdl/lice/lice_rect.cpp
    ../../external/wdl/lice/lice_arc.cpp
    ../../external/wdl/lice/lice_blit.cpp
)
target_include_directories(kj_gui PRIVATE ../../include ../../external)
target_link_libraries(kj_gui PRIVATE opengl32 gdi32 user32)
