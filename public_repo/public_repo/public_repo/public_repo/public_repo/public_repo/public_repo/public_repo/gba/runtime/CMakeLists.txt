# Game Boy Advance runtime: ARM7TDMI CPU state, bus, BIOS HLE, interpreter, PPU, APU, frontend.
add_library(gba_runtime STATIC
  src/cartridge.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/ppu.cpp
  src/apu.cpp
  src/frontend.cpp
  src/runtime.cpp)
target_include_directories(gba_runtime PUBLIC include)
target_link_libraries(gba_runtime PUBLIC dragon_common PRIVATE dragon_warnings)
# The GBA runtime (interpreter + per-step PPU/APU) must run faster than real time even in the
# default Debug configuration: Studio links game binaries against this archive, and an -O0 build
# cannot keep the audio device fed (crackling/stutter). Keep -g for debugging.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(gba_runtime PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

# SDL3 is optional for headless tests, enabled for frontend when present.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "GBA runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(gba_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(gba_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "GBA runtime: SDL3 not found (frontend headless only)")
endif()

add_executable(gba_runner src/gba_runner_main.cpp)
target_link_libraries(gba_runner PRIVATE gba_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(gba_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(gba_cpu_unit_tests PRIVATE gba_runtime dragon_warnings)
  add_test(NAME gba_cpu_unit_tests COMMAND gba_cpu_unit_tests)

  add_executable(gba_ppu_unit_tests tests/ppu_unit_tests.cpp)
  target_link_libraries(gba_ppu_unit_tests PRIVATE gba_runtime dragon_warnings)
  add_test(NAME gba_ppu_unit_tests COMMAND gba_ppu_unit_tests)

  add_executable(gba_apu_unit_tests tests/apu_unit_tests.cpp)
  target_link_libraries(gba_apu_unit_tests PRIVATE gba_runtime dragon_warnings)
  add_test(NAME gba_apu_unit_tests COMMAND gba_apu_unit_tests)

  add_executable(gba_rtc_unit_tests tests/rtc_unit_tests.cpp)
  target_link_libraries(gba_rtc_unit_tests PRIVATE gba_runtime dragon_warnings)
  add_test(NAME gba_rtc_unit_tests COMMAND gba_rtc_unit_tests)
endif()
