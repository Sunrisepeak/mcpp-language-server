cmake_minimum_required(VERSION 3.28)
project(fetching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A dependency the build fetches: nothing of it is on the machine until someone lets it download.
include(FetchContent)
FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 11.0.2)
FetchContent_MakeAvailable(fmt)

add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE fmt::fmt)
