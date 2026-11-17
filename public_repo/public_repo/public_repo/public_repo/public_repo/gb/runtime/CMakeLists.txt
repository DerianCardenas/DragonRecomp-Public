# Game Boy runtime: CPU state, bus, mappers, interpreter, opcodes, PPU, frontend.
add_library(gb_runtime STATIC
  src/cartridge.cpp
  src/opcodes.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/ppu.cpp
  src/apu.cpp
  src/frontend.cpp
  src/runtime.cpp)
target_include_directories(gb_runtime PUBLIC include)
target_link_libraries(gb_runtime PUBLIC dragon_common PRIVATE dragon_warnings)

# SDL3 is optional for headless tests, enabled for frontend when present.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "Game Boy runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(gb_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(gb_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "Game Boy runtime: SDL3 not found (frontend headless only)")
endif()

add_executable(gb_runner src/gb_runner_main.cpp)
target_link_libraries(gb_runner PRIVATE gb_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(gb_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(gb_cpu_unit_tests PRIVATE gb_runtime dragon_warnings)
  add_test(NAME gb_cpu_unit_tests COMMAND gb_cpu_unit_tests)

  add_executable(gb_ppu_unit_tests tests/ppu_unit_tests.cpp)
  target_link_libraries(gb_ppu_unit_tests PRIVATE gb_runtime dragon_warnings)
  add_test(NAME gb_ppu_unit_tests COMMAND gb_ppu_unit_tests)

  add_executable(gb_apu_unit_tests tests/apu_unit_tests.cpp)
  target_link_libraries(gb_apu_unit_tests PRIVATE gb_runtime dragon_warnings)
  add_test(NAME gb_apu_unit_tests COMMAND gb_apu_unit_tests)
endif()
