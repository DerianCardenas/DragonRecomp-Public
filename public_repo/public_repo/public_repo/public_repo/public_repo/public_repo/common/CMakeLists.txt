# Shared C++ libs: logging, TOML config loading, file io, hashing, ROM lookup.
add_library(dragon_common STATIC
  src/log.cpp
  src/sha1.cpp
  src/file_io.cpp
  src/config.cpp
  src/rom_locator.cpp)
target_include_directories(dragon_common PUBLIC include)
target_include_directories(dragon_common SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus)
target_link_libraries(dragon_common PRIVATE dragon_warnings)

if(DRAGON_BUILD_TESTS)
  add_executable(dragon_common_tests tests/common_tests.cpp)
  target_link_libraries(dragon_common_tests PRIVATE dragon_common dragon_warnings)
  add_test(NAME dragon_common_tests COMMAND dragon_common_tests
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
