add_library(psprecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(psprecomp_lib PUBLIC include)
target_link_libraries(psprecomp_lib PUBLIC dragon_common dragon_mips psp_runtime)
target_link_libraries(psprecomp_lib PRIVATE dragon_warnings)

add_executable(psprecomp src/main.cpp)
target_link_libraries(psprecomp PRIVATE psprecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(psp_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(psp_recomp_unit_tests PRIVATE psprecomp_lib dragon_warnings)
  add_test(NAME psp_recomp_unit_tests COMMAND psp_recomp_unit_tests)
endif()
