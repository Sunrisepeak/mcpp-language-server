cmake_minimum_required(VERSION 3.28)
project(hello LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(app src/main.cpp)
target_sources(app PUBLIC FILE_SET CXX_MODULES FILES src/greet/greet.cppm)
