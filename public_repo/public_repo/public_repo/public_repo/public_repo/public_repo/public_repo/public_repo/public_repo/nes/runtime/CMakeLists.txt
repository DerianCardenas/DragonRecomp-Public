# NES runtime: CPU state, bus, PPU, APU, mappers, interpreter fallback, SDL3 frontend.
add_library(nes_runtime STATIC
  src/runtime.cpp
  src/cartridge.cpp
  src/ppu.cpp
  src/apu.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/frontend.cpp)
target_include_directories(nes_runtime PUBLIC include)
target_link_libraries(nes_runtime PUBLIC dragon_common PRIVATE dragon_warnings)

# SDL3 is optional for headless tests, enabled for frontend when present.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "NES runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(nes_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(nes_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "NES runtime: SDL3 not found (frontend disabled)")
endif()

add_executable(nes_runner src/nes_runner_main.cpp)
target_link_libraries(nes_runner PRIVATE nes_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(nes_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(nes_cpu_unit_tests PRIVATE nes_runtime dragon_warnings)
  add_test(NAME nes_cpu_unit_tests COMMAND nes_cpu_unit_tests)

  add_executable(nes_ppu_unit_tests tests/ppu_unit_tests.cpp)
  target_link_libraries(nes_ppu_unit_tests PRIVATE nes_runtime dragon_warnings)
  add_test(NAME nes_ppu_unit_tests COMMAND nes_ppu_unit_tests)

  add_executable(nes_apu_unit_tests tests/apu_unit_tests.cpp)
  target_link_libraries(nes_apu_unit_tests PRIVATE nes_runtime dragon_warnings)
  add_test(NAME nes_apu_unit_tests COMMAND nes_apu_unit_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(nes_nestest tests/nestest_runner.cpp)
  target_link_libraries(nes_nestest PRIVATE nes_runtime dragon_warnings)
  add_test(NAME nes_nestest COMMAND nes_nestest
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(nes_nestest PROPERTIES SKIP_RETURN_CODE 77)
endif()
