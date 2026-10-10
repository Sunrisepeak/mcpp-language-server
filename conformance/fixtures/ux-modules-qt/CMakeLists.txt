cmake_minimum_required(VERSION 3.28)
project(inventory LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 5 (qtbase5-dev): its headers in the global module fragments, as a Qt project written with modules has them.
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)

add_library(views)
target_sources(views PUBLIC FILE_SET CXX_MODULES FILES src/model.cppm src/view.cppm)
target_link_libraries(views PUBLIC Qt5::Widgets)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE views)
