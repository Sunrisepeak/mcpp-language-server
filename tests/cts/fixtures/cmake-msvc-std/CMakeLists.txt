cmake_minimum_required(VERSION 3.30)
project(greet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

add_library(greet)
target_sources(greet PUBLIC FILE_SET CXX_MODULES FILES src/greet.ixx)
target_compile_features(greet PUBLIC cxx_std_23)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE greet)
