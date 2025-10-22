add_library(kj_core audio_engine.cpp sample_loader.cpp sequencer.cpp tracks.cpp)
target_include_directories(kj_core PRIVATE ../../include)
target_link_libraries(kj_core PRIVATE ole32 uuid winmm dsound)
