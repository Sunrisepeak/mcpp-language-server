cmake_minimum_required(VERSION 3.28)
project(winapp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# <windows.h> in the global module fragments, as a Windows program written with modules has it.
add_library(platform)
target_sources(platform PUBLIC FILE_SET CXX_MODULES FILES src/process.cppm src/files.cppm)
target_compile_definitions(platform PUBLIC UNICODE _UNICODE NOMINMAX)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE platform)
