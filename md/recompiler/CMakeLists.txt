# mdrecomp: Sega Genesis / Mega Drive Motorola 68000 Static Recompiler -> C99

add_library(mdrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp
)

target_include_directories(mdrecomp_lib PUBLIC
  include
  ${CMAKE_SOURCE_DIR}/common/include
  ${CMAKE_SOURCE_DIR}/md/runtime/include
)

target_link_libraries(mdrecomp_lib PUBLIC
  dragon_common
  md_runtime
  dragon_warnings
)

add_executable(mdrecomp src/main.cpp)
target_link_libraries(mdrecomp PRIVATE mdrecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME mdrecomp_version COMMAND mdrecomp --version)

  add_executable(md_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(md_recomp_unit_tests PRIVATE mdrecomp_lib dragon_warnings)
  add_test(NAME md_recomp_unit_tests COMMAND md_recomp_unit_tests)
endif()
