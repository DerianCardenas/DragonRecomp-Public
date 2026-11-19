cmake_minimum_required(VERSION 3.25)
project(DragonRecompStudioSlint VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Slint QUIET)
if (NOT Slint_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        Slint
        GIT_REPOSITORY https://github.com/slint-ui/slint.git
        GIT_TAG release/1
        SOURCE_SUBDIR api/cpp
    )
    FetchContent_MakeAvailable(Slint)
endif()

add_executable(dragon-studio-slint main.cpp)
slint_target_sources(dragon-studio-slint ui/app.slint)
target_link_libraries(dragon-studio-slint PRIVATE Slint::Slint)
