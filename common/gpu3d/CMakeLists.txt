add_library(dragon_gpu3d STATIC
  src/soft_gpu3d.cpp)
target_include_directories(dragon_gpu3d PUBLIC include)
target_link_libraries(dragon_gpu3d PRIVATE dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(dragon_gpu3d_tests tests/gpu3d_unit_tests.cpp)
  target_link_libraries(dragon_gpu3d_tests PRIVATE dragon_gpu3d dragon_warnings)
  add_test(NAME dragon_gpu3d_tests COMMAND dragon_gpu3d_tests)
endif()
