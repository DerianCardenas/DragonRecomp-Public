# Sega Master System & Game Gear runtime: Z80 CPU state, opcodes, mappers, VDP, PSG, bus, interpreter, frontend.
add_library(sms_runtime STATIC
  src/cartridge.cpp
  src/opcodes.cpp
  src/vdp.cpp
  src/psg.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/frontend.cpp
  src/runtime.cpp)
target_include_directories(sms_runtime PUBLIC include)
target_link_libraries(sms_runtime PUBLIC dragon_common PRIVATE dragon_warnings)

# SDL3 is optional for headless tests, enabled for frontend when present.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "SMS/GG runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(sms_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(sms_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "SMS/GG runtime: SDL3 not found (frontend headless only)")
endif()

add_executable(sms_runner src/sms_runner_main.cpp)
target_link_libraries(sms_runner PRIVATE sms_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(sms_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(sms_cpu_unit_tests PRIVATE sms_runtime dragon_warnings)
  add_test(NAME sms_cpu_unit_tests COMMAND sms_cpu_unit_tests)

  add_executable(sms_vdp_unit_tests tests/vdp_unit_tests.cpp)
  target_link_libraries(sms_vdp_unit_tests PRIVATE sms_runtime dragon_warnings)
  add_test(NAME sms_vdp_unit_tests COMMAND sms_vdp_unit_tests)

  add_executable(sms_psg_unit_tests tests/psg_unit_tests.cpp)
  target_link_libraries(sms_psg_unit_tests PRIVATE sms_runtime dragon_warnings)
  add_test(NAME sms_psg_unit_tests COMMAND sms_psg_unit_tests)
endif()
