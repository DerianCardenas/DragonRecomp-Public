find_package(SDL3 QUIET)
find_package(ZLIB QUIET)

add_library(psp_runtime STATIC
  src/loader.cpp
  src/bus.cpp
  src/audio.cpp
  src/ge.cpp
  src/hle.cpp
  src/interpreter.cpp
  src/runtime.cpp
  src/frontend.cpp)

target_include_directories(psp_runtime PUBLIC include)
target_link_libraries(psp_runtime PUBLIC dragon_common dragon_mips dragon_gpu3d)
target_link_libraries(psp_runtime PRIVATE dragon_warnings)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(psp_runtime PRIVATE -O2)
endif()

if(ZLIB_FOUND)
  message(STATUS "PSP runtime: ZLIB ${ZLIB_VERSION_STRING} found (CSO support enabled)")
  target_compile_definitions(psp_runtime PUBLIC DRAGON_HAVE_ZLIB=1)
  target_link_libraries(psp_runtime PUBLIC ZLIB::ZLIB)
else()
  message(STATUS "PSP runtime: ZLIB not found (CSO compressed ISO disabled)")
endif()

if(SDL3_FOUND)
  message(STATUS "PSP runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(psp_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(psp_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "PSP runtime: SDL3 not found (headless mode only)")
endif()

add_executable(psp_runner src/psp_runner_main.cpp)
target_link_libraries(psp_runner PRIVATE psp_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(psp_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(psp_cpu_unit_tests PRIVATE psp_runtime dragon_warnings)
  add_test(NAME psp_cpu_unit_tests COMMAND psp_cpu_unit_tests)

  add_executable(psp_hle_unit_tests tests/hle_unit_tests.cpp)
  target_link_libraries(psp_hle_unit_tests PRIVATE psp_runtime dragon_warnings)
  add_test(NAME psp_hle_unit_tests COMMAND psp_hle_unit_tests)

  add_executable(psp_ge_unit_tests tests/ge_unit_tests.cpp)
  target_link_libraries(psp_ge_unit_tests PRIVATE psp_runtime dragon_warnings)
  add_test(NAME psp_ge_unit_tests COMMAND psp_ge_unit_tests)
endif()
