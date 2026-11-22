find_package(SDL3 QUIET)

add_library(ps1_runtime STATIC
  src/loader.cpp
  src/gte.cpp
  src/gpu.cpp
  src/spu.cpp
  src/cdrom.cpp
  src/bus.cpp
  src/bios_hle.cpp
  src/interpreter.cpp
  src/runtime.cpp
  src/frontend.cpp)

target_include_directories(ps1_runtime PUBLIC include)
target_link_libraries(ps1_runtime PUBLIC dragon_common dragon_mips)
target_link_libraries(ps1_runtime PRIVATE dragon_warnings)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ps1_runtime PRIVATE -O2)
endif()

if(SDL3_FOUND)
  message(STATUS "PS1 runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(ps1_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(ps1_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "PS1 runtime: SDL3 not found (headless mode only)")
endif()

add_executable(ps1_runner src/ps1_runner_main.cpp)
target_link_libraries(ps1_runner PRIVATE ps1_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(ps1_cpu_gte_unit_tests tests/cpu_gte_unit_tests.cpp)
  target_link_libraries(ps1_cpu_gte_unit_tests PRIVATE ps1_runtime dragon_warnings)
  add_test(NAME ps1_cpu_gte_unit_tests COMMAND ps1_cpu_gte_unit_tests)

  add_executable(ps1_gpu_unit_tests tests/gpu_unit_tests.cpp)
  target_link_libraries(ps1_gpu_unit_tests PRIVATE ps1_runtime dragon_warnings)
  add_test(NAME ps1_gpu_unit_tests COMMAND ps1_gpu_unit_tests)

  add_executable(ps1_spu_bios_unit_tests tests/spu_bios_unit_tests.cpp)
  target_link_libraries(ps1_spu_bios_unit_tests PRIVATE ps1_runtime dragon_warnings)
  add_test(NAME ps1_spu_bios_unit_tests COMMAND ps1_spu_bios_unit_tests)
endif()
