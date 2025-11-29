add_library(kj_gui STATIC
    gui_main.cpp
    gui_refresh.cpp
    compressor_window.cpp
    effects_window.cpp
    lfo_window.cpp
    mod_matrix_window.cpp
    waveform_window.cpp
    ../../external/wdl/lice/lice.cpp
)

target_include_directories(kj_gui PRIVATE ../../include ../../external)
target_link_libraries(kj_gui
    PUBLIC
        kj_core
    PRIVATE
        opengl32
        gdi32
        user32
        comctl32
)
