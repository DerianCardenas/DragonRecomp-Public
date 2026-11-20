# DragonRecomp Studio: GUI Drag & Drop, Auto-learner pipeline, and Standalone packager.

find_package(SDL3 CONFIG REQUIRED)

add_library(imgui STATIC
  ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
  ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp
  ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)

target_include_directories(imgui SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party/imgui
  ${CMAKE_SOURCE_DIR}/third_party/imgui/backends)

# Disable all warnings for vendored ImGui library to maintain warning-free build
target_compile_options(imgui PRIVATE -w)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_library(studio_lib STATIC
  src/rom_detector.cpp
  src/pipeline.cpp
  src/packager.cpp
  src/library.cpp
  src/library_view.cpp
  src/studio_views.cpp
  src/cover_manager.cpp
  src/settings.cpp
  src/theme.cpp
  src/app.cpp)

# toml++ and stb are used privately by the game library and cover manager.
target_include_directories(studio_lib SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus
  ${CMAKE_SOURCE_DIR}/third_party/stb)

target_include_directories(studio_lib PUBLIC
  include
  ${CMAKE_SOURCE_DIR}/common/include
  ${CMAKE_SOURCE_DIR}/nes/runtime/include
  ${CMAKE_SOURCE_DIR}/snes/runtime/include)

target_link_libraries(studio_lib PUBLIC
  imgui
  dragon_common
  nes_runtime
  snes_runtime
  gb_runtime
  gba_runtime
  sms_runtime
  pce_runtime
  SDL3::SDL3
  dragon_warnings)

if(WIN32)
  target_link_libraries(studio_lib PUBLIC urlmon wininet)
endif()

add_executable(dragon-studio src/main.cpp)
target_link_libraries(dragon-studio PRIVATE studio_lib dragon_warnings)
if(WIN32)
  set_target_properties(dragon-studio PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MINGW)
    target_link_options(dragon-studio PRIVATE -mwindows)
  endif()
elseif(UNIX AND NOT APPLE)
  set_target_properties(dragon-studio PROPERTIES
    INSTALL_RPATH "$ORIGIN:$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
  )
endif()

add_custom_target(studio_assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets
    ${CMAKE_BINARY_DIR}/assets
  COMMENT "Copying assets to build directory"
)
add_dependencies(dragon-studio studio_assets)

if(DRAGON_BUILD_TESTS)
  add_executable(studio_unit_tests tests/studio_unit_tests.cpp)
  target_link_libraries(studio_unit_tests PRIVATE studio_lib dragon_warnings)
  add_test(NAME studio_unit_tests COMMAND studio_unit_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(studio_library_tests tests/library_tests.cpp)
  target_link_libraries(studio_library_tests PRIVATE studio_lib dragon_warnings)
  add_test(NAME studio_library_tests COMMAND studio_library_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
