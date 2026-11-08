cmake_minimum_required(VERSION 3.28)
project(shapes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shapes)
target_sources(shapes PUBLIC FILE_SET CXX_MODULES FILES src/shapes.cppm src/circle.cppm)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE shapes)
