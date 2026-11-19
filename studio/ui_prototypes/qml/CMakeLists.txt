cmake_minimum_required(VERSION 3.25)
project(DragonRecompStudioQML VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(dragon-studio-qml main.cpp)

qt_add_qml_module(dragon-studio-qml
    URI DragonRecomp.Studio
    VERSION 1.0
    QML_FILES
        qml/Theme.qml
        qml/GameCard.qml
        qml/Main.qml
)

target_link_libraries(dragon-studio-qml PRIVATE Qt6::Quick Qt6::QuickControls2)
