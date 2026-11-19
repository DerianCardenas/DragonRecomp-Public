# smsrecomp: Sega Master System / Game Gear ROM + game TOML -> C99 sources.
add_library(smsrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(smsrecomp_lib PUBLIC include)
target_link_libraries(smsrecomp_lib PUBLIC sms_runtime dragon_common PRIVATE dragon_warnings)

add_executable(smsrecomp src/main.cpp)
target_link_libraries(smsrecomp PRIVATE smsrecomp_lib dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_test(NAME smsrecomp_version COMMAND smsrecomp --version)

  add_executable(sms_recomp_unit_tests tests/recomp_unit_tests.cpp)
  target_link_libraries(sms_recomp_unit_tests PRIVATE smsrecomp_lib dragon_warnings)
  add_test(NAME sms_recomp_unit_tests COMMAND sms_recomp_unit_tests)
endif()
