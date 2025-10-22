add_library(kj_gui STATIC
    gui_main.cpp
    ../../external/wdl/lice/lice.cpp
    ../../external/wdl/lice/lice_arc.cpp
    ../../external/wdl/lice/lice_bmp.cpp
    ../../external/wdl/lice/lice_colorspace.cpp
    ../../external/wdl/lice/lice_gif.cpp
    ../../external/wdl/lice/lice_gif_write.cpp
    ../../external/wdl/lice/lice_gl_ctx.cpp
    ../../external/wdl/lice/lice_glbitmap.cpp
    ../../external/wdl/lice/lice_ico.cpp
    ../../external/wdl/lice/lice_image.cpp
    ../../external/wdl/lice/lice_jpg.cpp
    ../../external/wdl/lice/lice_jpg_write.cpp
    ../../external/wdl/lice/lice_lcf.cpp
    ../../external/wdl/lice/lice_line.cpp
    ../../external/wdl/lice/lice_lvg.cpp
    ../../external/wdl/lice/lice_palette.cpp
    ../../external/wdl/lice/lice_pcx.cpp
    ../../external/wdl/lice/lice_png.cpp
    ../../external/wdl/lice/lice_png_write.cpp
    ../../external/wdl/lice/lice_texgen.cpp
    ../../external/wdl/lice/lice_text.cpp
    ../../external/wdl/lice/lice_textnew.cpp
)
target_include_directories(kj_gui PRIVATE ../../include ../../external)
target_link_libraries(kj_gui PRIVATE opengl32 gdi32 user32)
