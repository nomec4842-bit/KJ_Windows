cmake_minimum_required(VERSION 3.20)
project(KJ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(src/core)
add_subdirectory(src/gui)

add_executable(KJ src/main.cpp)
target_include_directories(KJ PRIVATE include)
target_link_libraries(KJ PRIVATE kj_core kj_gui)
