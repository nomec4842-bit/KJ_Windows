add_library(kj_core audio_engine.cpp delay_effect.cpp midi_output.cpp midi_ports.cpp mod_matrix.cpp project_io.cpp sample_loader.cpp sequencer.cpp sidechain_processor.cpp tracks.cpp)
target_sources(kj_core PRIVATE audio_device_handler.cpp)
target_include_directories(kj_core PRIVATE ../../include)

if (WIN32)
    target_link_libraries(kj_core PRIVATE ole32 uuid winmm dsound mmdevapi avrt)
endif()
target_compile_definitions(kj_core PRIVATE $<$<CONFIG:Debug>:DEBUG_AUDIO>)
