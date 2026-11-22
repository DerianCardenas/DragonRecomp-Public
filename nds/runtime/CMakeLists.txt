find_package(SDL3 QUIET)

add_library(nds_runtime STATIC
  src/cartridge.cpp
  src/spu.cpp
  src/gx3d.cpp
  src/ppu2d.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/runtime.cpp
  src/frontend.cpp)

target_include_directories(nds_runtime PUBLIC include)
target_link_libraries(nds_runtime PUBLIC dragon_common dragon_gpu3d)
target_link_libraries(nds_runtime PRIVATE dragon_warnings)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nds_runtime PRIVATE -O2)
endif()

if(SDL3_FOUND)
  message(STATUS "NDS runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(nds_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(nds_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "NDS runtime: SDL3 not found (headless mode only)")
endif()

add_executable(nds_runner src/nds_runner_main.cpp)
target_link_libraries(nds_runner PRIVATE nds_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(nds_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(nds_cpu_unit_tests PRIVATE nds_runtime dragon_warnings)
  add_test(NAME nds_cpu_unit_tests COMMAND nds_cpu_unit_tests)

  add_executable(nds_ppu_gx_unit_tests tests/ppu_gx_unit_tests.cpp)
  target_link_libraries(nds_ppu_gx_unit_tests PRIVATE nds_runtime dragon_warnings)
  add_test(NAME nds_ppu_gx_unit_tests COMMAND nds_ppu_gx_unit_tests)
endif()
