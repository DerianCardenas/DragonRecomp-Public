# gbarecomp: Game Boy Advance ROM + game TOML -> C99 sources.
add_library(gbarecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(gbarecomp_lib PUBLIC include)
target_link_libraries(gbarecomp_lib PUBLIC gba_runtime dragon_common PRIVATE dragon_warnings)

add_executable(gbarecomp src/main.cpp)
target_link_libraries(gbarecomp PRIVATE gbarecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME gbarecomp_version COMMAND gbarecomp --version)

  add_executable(gba_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_compile_definitions(gba_recomp_unit_tests PRIVATE DRAGON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(gba_recomp_unit_tests PRIVATE gbarecomp_lib dragon_warnings)
  add_test(NAME gba_recomp_unit_tests COMMAND gba_recomp_unit_tests)
endif()
