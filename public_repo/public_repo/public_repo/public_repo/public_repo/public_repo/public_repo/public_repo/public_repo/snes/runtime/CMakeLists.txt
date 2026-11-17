# SNES runtime: CPU state, bus, cartridge loader, PPU, APU, interpreter fallback, SDL3 frontend.
add_library(snes_runtime STATIC
  src/apu.cpp
  src/bus.cpp
  src/cartridge.cpp
  src/frontend.cpp
  src/interpreter.cpp
  src/ppu.cpp
  src/runtime.cpp)
target_include_directories(snes_runtime PUBLIC include)
target_link_libraries(snes_runtime PUBLIC dragon_common PRIVATE dragon_warnings)

# SDL3 is optional for headless tests, enabled for frontend when present.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "SNES runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(snes_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(snes_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "SNES runtime: SDL3 not found (frontend disabled)")
endif()

add_executable(snes_runner src/snes_runner_main.cpp)
target_link_libraries(snes_runner PRIVATE snes_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(snes_cpu_unit_tests tests/snes_cpu_unit_tests.cpp)
  target_link_libraries(snes_cpu_unit_tests PRIVATE snes_runtime dragon_warnings)
  add_test(NAME snes_cpu_unit_tests COMMAND snes_cpu_unit_tests)

  add_executable(snes_ppu_unit_tests tests/snes_ppu_unit_tests.cpp)
  target_link_libraries(snes_ppu_unit_tests PRIVATE snes_runtime dragon_warnings)
  add_test(NAME snes_ppu_unit_tests COMMAND snes_ppu_unit_tests)

  add_executable(snes_apu_unit_tests tests/snes_apu_unit_tests.cpp)
  target_link_libraries(snes_apu_unit_tests PRIVATE snes_runtime dragon_warnings)
  add_test(NAME snes_apu_unit_tests COMMAND snes_apu_unit_tests)
endif()
