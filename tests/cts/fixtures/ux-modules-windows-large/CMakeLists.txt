cmake_minimum_required(VERSION 3.28)
project(bigwin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(big)
target_sources(big PUBLIC FILE_SET CXX_MODULES FILES src/m00.cppm src/m01.cppm src/m02.cppm src/m03.cppm src/m04.cppm src/m05.cppm src/m06.cppm src/m07.cppm src/m08.cppm src/m09.cppm src/m10.cppm src/m11.cppm src/m12.cppm src/m13.cppm src/m14.cppm src/m15.cppm src/m16.cppm src/m17.cppm src/m18.cppm src/m19.cppm src/m20.cppm src/m21.cppm src/m22.cppm src/m23.cppm src/m24.cppm src/m25.cppm src/m26.cppm src/m27.cppm src/m28.cppm src/m29.cppm)
target_compile_definitions(big PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE big)
