# PC Engine / TurboGrafx-16 runtime: HuC6280 CPU, MMU, HuC6270 VDC, HuC6260 VCE, PSG, bus, interpreter, frontend.
add_library(pce_runtime STATIC
  src/cartridge.cpp
  src/opcodes.cpp
  src/vdc.cpp
  src/psg.cpp
  src/bus.cpp
  src/interpreter.cpp
  src/frontend.cpp
  src/runtime.cpp)
target_include_directories(pce_runtime PUBLIC include)
target_link_libraries(pce_runtime PUBLIC dragon_common PRIVATE dragon_warnings)

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "PCE/TG-16 runtime: SDL3 ${SDL3_VERSION} found")
  target_compile_definitions(pce_runtime PUBLIC DRAGON_HAVE_SDL3=1)
  target_link_libraries(pce_runtime PUBLIC SDL3::SDL3)
else()
  message(STATUS "PCE/TG-16 runtime: SDL3 not found (frontend headless only)")
endif()

add_executable(pce_runner src/pce_runner_main.cpp)
target_link_libraries(pce_runner PRIVATE pce_runtime dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(pce_cpu_unit_tests tests/cpu_unit_tests.cpp)
  target_link_libraries(pce_cpu_unit_tests PRIVATE pce_runtime dragon_warnings)
  add_test(NAME pce_cpu_unit_tests COMMAND pce_cpu_unit_tests)

  add_executable(pce_vdc_unit_tests tests/vdc_unit_tests.cpp)
  target_link_libraries(pce_vdc_unit_tests PRIVATE pce_runtime dragon_warnings)
  add_test(NAME pce_vdc_unit_tests COMMAND pce_vdc_unit_tests)

  add_executable(pce_psg_unit_tests tests/psg_unit_tests.cpp)
  target_link_libraries(pce_psg_unit_tests PRIVATE pce_runtime dragon_warnings)
  add_test(NAME pce_psg_unit_tests COMMAND pce_psg_unit_tests)
endif()
