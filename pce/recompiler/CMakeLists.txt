add_library(pcerecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(pcerecomp_lib PUBLIC include)
target_link_libraries(pcerecomp_lib PUBLIC pce_runtime dragon_common PRIVATE dragon_warnings)

add_executable(pcerecomp src/main.cpp)
target_link_libraries(pcerecomp PRIVATE pcerecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME pcerecomp_version COMMAND pcerecomp --version)

  add_executable(pce_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(pce_recomp_unit_tests PRIVATE pcerecomp_lib dragon_warnings)
  add_test(NAME pce_recomp_unit_tests COMMAND pce_recomp_unit_tests)
endif()
