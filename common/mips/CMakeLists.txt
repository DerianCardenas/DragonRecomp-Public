add_library(dragon_mips STATIC
  src/decoder.cpp)
target_include_directories(dragon_mips PUBLIC include)
target_link_libraries(dragon_mips PRIVATE dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(dragon_mips_tests tests/mips_unit_tests.cpp)
  target_link_libraries(dragon_mips_tests PRIVATE dragon_mips dragon_warnings)
  add_test(NAME dragon_mips_tests COMMAND dragon_mips_tests)
endif()
