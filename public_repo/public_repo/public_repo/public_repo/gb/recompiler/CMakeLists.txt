# gbrecomp: Game Boy ROM + game TOML -> C99 sources.
add_library(gbrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(gbrecomp_lib PUBLIC include)
target_link_libraries(gbrecomp_lib PUBLIC gb_runtime dragon_common PRIVATE dragon_warnings)

add_executable(gbrecomp src/main.cpp)
target_link_libraries(gbrecomp PRIVATE gbrecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME gbrecomp_version COMMAND gbrecomp --version)

  add_executable(gb_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(gb_recomp_unit_tests PRIVATE gbrecomp_lib dragon_warnings)
  add_test(NAME gb_recomp_unit_tests COMMAND gb_recomp_unit_tests)
endif()
