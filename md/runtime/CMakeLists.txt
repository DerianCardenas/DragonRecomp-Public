# Sega Genesis / Mega Drive Runtime (M68000, Z80, VDP Mode 5, YM2612, SN76489 PSG, SDL3)

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "MD/Genesis runtime: SDL3 ${SDL3_VERSION} found")
else()
  message(STATUS "MD/Genesis runtime: SDL3 not found (building headless runtime)")
endif()

add_library(md_runtime STATIC
  src/cartridge.cpp
  src/vdp.cpp
  src/ym2612.cpp
  src/z80.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/runtime.cpp
  src/frontend.cpp
)

target_include_directories(md_runtime PUBLIC
  include
  ${CMAKE_SOURCE_DIR}/common/include
  ${CMAKE_SOURCE_DIR}/sms/runtime/include
)

target_link_libraries(md_runtime PUBLIC
  dragon_common
  sms_runtime
  dragon_warnings
)

if(SDL3_FOUND)
  target_compile_definitions(md_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(md_runtime PUBLIC SDL3::SDL3)
endif()

add_executable(md_runner src/md_runner_main.cpp)
target_link_libraries(md_runner PRIVATE md_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(md_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(md_cpu_unit_tests PRIVATE md_runtime dragon_warnings)
  add_test(NAME md_cpu_unit_tests COMMAND md_cpu_unit_tests)

  add_executable(md_vdp_unit_tests tests/vdp_unit_tests.cpp)
  target_link_libraries(md_vdp_unit_tests PRIVATE md_runtime dragon_warnings)
  add_test(NAME md_vdp_unit_tests COMMAND md_vdp_unit_tests)

  add_executable(md_ym2612_unit_tests tests/ym2612_unit_tests.cpp)
  target_link_libraries(md_ym2612_unit_tests PRIVATE md_runtime dragon_warnings)
  add_test(NAME md_ym2612_unit_tests COMMAND md_ym2612_unit_tests)
endif()
