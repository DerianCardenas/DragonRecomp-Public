cmake_minimum_required(VERSION 3.25)

project(DragonRecomp
  VERSION 0.1.0
  DESCRIPTION "Static recompilers for NES and SNES games"
  LANGUAGES C CXX)

option(DRAGON_BUILD_NES    "Build NES recompiler and runtime"  ON)
option(DRAGON_BUILD_SNES   "Build SNES recompiler and runtime" ON)
option(DRAGON_BUILD_GB     "Build Game Boy recompiler and runtime" ON)
option(DRAGON_BUILD_GBA    "Build Game Boy Advance recompiler and runtime" ON)
option(DRAGON_BUILD_SMS    "Build Sega Master System / Game Gear recompiler and runtime" ON)
option(DRAGON_BUILD_PCE    "Build PC Engine / TurboGrafx-16 recompiler and runtime" ON)
option(DRAGON_BUILD_MD     "Build Sega Genesis / Mega Drive recompiler and runtime" ON)
option(DRAGON_BUILD_STUDIO "Build DragonRecomp Studio GUI"     ON)
option(DRAGON_BUILD_TESTS  "Build tests"                       ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_library(dragon_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(dragon_warnings INTERFACE -Wall -Wextra -Wpedantic)
elseif(MSVC)
  target_compile_options(dragon_warnings INTERFACE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(dragon_warnings INTERFACE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()

if(DRAGON_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)

if(DRAGON_BUILD_NES)
  add_subdirectory(nes)
endif()

if(DRAGON_BUILD_SNES)
  add_subdirectory(snes)
endif()

if(DRAGON_BUILD_GB)
  add_subdirectory(gb)
endif()

if(DRAGON_BUILD_GBA)
  add_subdirectory(gba)
endif()

if(DRAGON_BUILD_SMS)
  add_subdirectory(sms)
endif()

if(DRAGON_BUILD_PCE)
  add_subdirectory(pce)
endif()

if(DRAGON_BUILD_MD)
  add_subdirectory(md)
endif()

if(DRAGON_BUILD_STUDIO)
  add_subdirectory(studio)
endif()
