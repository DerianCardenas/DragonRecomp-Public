# nesrecomp: ROM + game TOML -> C99 sources.
add_library(nesrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(nesrecomp_lib PUBLIC include)
target_link_libraries(nesrecomp_lib PUBLIC nes_runtime dragon_common PRIVATE dragon_warnings)

add_executable(nesrecomp src/main.cpp)
target_link_libraries(nesrecomp PRIVATE nesrecomp_lib dragon_warnings)

# Helper to build a standalone native NES game executable from a game TOML + ROM.
function(add_nes_recomp_executable target_name toml_path rom_path rom_sha1 game_title)
  if(EXISTS "${rom_path}")
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_gen")
    set(gen_hdr "${gen_dir}/recomp_functions.h")
    set(gen_src "${gen_dir}/recomp_funcs_0.c")
    set(gen_disp "${gen_dir}/recomp_dispatch.c")

    add_custom_command(
      OUTPUT "${gen_hdr}" "${gen_src}" "${gen_disp}"
      COMMAND nesrecomp "${toml_path}" --rom "${rom_path}" --out "${gen_dir}" --single-file
      DEPENDS nesrecomp "${toml_path}" "${rom_path}"
      COMMENT "Recompiling ${game_title} -> C99"
      VERBATIM)

    add_executable(${target_name}
      "${CMAKE_SOURCE_DIR}/nes/runtime/src/nes_recomp_game_main.cpp"
      "${gen_src}"
      "${gen_disp}")
    target_include_directories(${target_name} PRIVATE "${gen_dir}")
    target_link_libraries(${target_name} PRIVATE nes_runtime dragon_warnings)
    target_compile_definitions(${target_name} PRIVATE
      DRAGON_GAME_TITLE="${game_title}"
      DRAGON_DEFAULT_ROM_PATH="${rom_path}"
      DRAGON_DEFAULT_ROM_SHA1="${rom_sha1}")
    if(WIN32)
      set_target_properties(${target_name} PROPERTIES SUFFIX ".exe")
    endif()
  endif()
endfunction()

add_nes_recomp_executable(smb_recomp
  "${CMAKE_SOURCE_DIR}/nes/games/smb/smb.toml"
  "${CMAKE_SOURCE_DIR}/roms/nes/smb.nes"
  "ea343f4e445a9050d4b4fbac2c77d0693b1d0922"
  "Super Mario Bros. (DragonRecomp)")

add_nes_recomp_executable(dk_recomp
  "${CMAKE_SOURCE_DIR}/nes/games/dk/dk.toml"
  "${CMAKE_SOURCE_DIR}/roms/nes/dk.nes"
  "d8dfacbfec34cdc871d73c901811551fe1706923"
  "Donkey Kong (DragonRecomp)")

if(DRAGON_BUILD_TESTS)
  add_test(NAME nesrecomp_version COMMAND nesrecomp --version)

  add_executable(nes_recomp_diff_tests tests/recomp_diff_tests.cpp)
  set_target_properties(nes_recomp_diff_tests PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(nes_recomp_diff_tests PRIVATE nesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(nes_recomp_diff_tests PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/nes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME nes_recomp_diff_tests COMMAND nes_recomp_diff_tests)

  add_executable(nes_mapper_unit_tests tests/mapper_unit_tests.cpp)
  set_target_properties(nes_mapper_unit_tests PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(nes_mapper_unit_tests PRIVATE nesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(nes_mapper_unit_tests PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/nes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME nes_mapper_unit_tests COMMAND nes_mapper_unit_tests)

  add_executable(nes_smb_recomp_test tests/smb_recomp_test.cpp)
  target_link_libraries(nes_smb_recomp_test PRIVATE nesrecomp_lib dragon_warnings)
  target_compile_definitions(nes_smb_recomp_test PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/nes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME nes_smb_recomp_test COMMAND nes_smb_recomp_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(nes_smb_recomp_test PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(smb_boot_test tests/smb_boot_test.cpp)
  set_target_properties(smb_boot_test PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(smb_boot_test PRIVATE nesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(smb_boot_test PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/nes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME smb_boot_test COMMAND smb_boot_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(smb_boot_test PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(dk_boot_test tests/dk_boot_test.cpp)
  set_target_properties(dk_boot_test PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(dk_boot_test PRIVATE nesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(dk_boot_test PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/nes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME dk_boot_test COMMAND dk_boot_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(dk_boot_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
