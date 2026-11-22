add_library(psxrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(psxrecomp_lib PUBLIC include)
target_link_libraries(psxrecomp_lib PUBLIC dragon_common dragon_mips ps1_runtime)
target_link_libraries(psxrecomp_lib PRIVATE dragon_warnings)

add_executable(psxrecomp src/main.cpp)
target_link_libraries(psxrecomp PRIVATE psxrecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(ps1_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(ps1_recomp_unit_tests PRIVATE psxrecomp_lib dragon_warnings)
  add_test(NAME ps1_recomp_unit_tests COMMAND ps1_recomp_unit_tests)
endif()
