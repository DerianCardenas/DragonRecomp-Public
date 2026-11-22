add_library(ndsrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(ndsrecomp_lib PUBLIC include)
target_link_libraries(ndsrecomp_lib PUBLIC dragon_common nds_runtime)
target_link_libraries(ndsrecomp_lib PRIVATE dragon_warnings)

add_executable(ndsrecomp src/main.cpp)
target_link_libraries(ndsrecomp PRIVATE ndsrecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME ndsrecomp_version COMMAND ndsrecomp --version)

  add_executable(nds_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(nds_recomp_unit_tests PRIVATE ndsrecomp_lib dragon_warnings)
  add_test(NAME nds_recomp_unit_tests COMMAND nds_recomp_unit_tests)
endif()
