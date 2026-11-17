# snesrecomp: ROM + game TOML -> C99 sources.
add_library(snesrecomp_lib STATIC
  src/disassembler.cpp
  src/emitter.cpp)
target_include_directories(snesrecomp_lib PUBLIC include)
target_link_libraries(snesrecomp_lib PUBLIC snes_runtime dragon_common PRIVATE dragon_warnings)

add_executable(snesrecomp src/main.cpp)
target_link_libraries(snesrecomp PRIVATE snesrecomp_lib dragon_warnings)

# Helper to build a standalone native SNES game executable from a game TOML + ROM.
function(add_snes_recomp_executable target_name toml_path rom_path rom_sha1 game_title)
  if(EXISTS "${rom_path}")
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_gen")
    set(gen_hdr "${gen_dir}/recomp_functions.h")
    set(gen_src "${gen_dir}/recomp_funcs_0.c")
    set(gen_disp "${gen_dir}/recomp_dispatch.c")

    add_custom_command(
      OUTPUT "${gen_hdr}" "${gen_src}" "${gen_disp}"
      COMMAND snesrecomp "${toml_path}" --rom "${rom_path}" --out "${gen_dir}" --single-file
      DEPENDS snesrecomp "${toml_path}" "${rom_path}"
      COMMENT "Recompiling ${game_title} -> C99"
      VERBATIM)

    add_executable(${target_name}
      "${CMAKE_SOURCE_DIR}/snes/runtime/src/snes_recomp_game_main.cpp"
      "${gen_src}"
      "${gen_disp}")
    target_include_directories(${target_name} PRIVATE "${gen_dir}")
    target_link_libraries(${target_name} PRIVATE snes_runtime dragon_warnings)
    target_compile_definitions(${target_name} PRIVATE
      DRAGON_GAME_TITLE="${game_title}"
      DRAGON_DEFAULT_ROM_PATH="${rom_path}"
      DRAGON_DEFAULT_ROM_SHA1="${rom_sha1}")
    if(WIN32)
      set_target_properties(${target_name} PROPERTIES SUFFIX ".exe")
    endif()
  endif()
endfunction()

add_snes_recomp_executable(smb_snes_recomp
  "${CMAKE_SOURCE_DIR}/snes/games/smb_snes/smb_snes.toml"
  "${CMAKE_SOURCE_DIR}/roms/snes/smb_snes.sfc"
  "d685be2360730c97e4b57406e62b13919cbada58"
  "Super Mario Bros. SNES (DragonRecomp)")

if(DRAGON_BUILD_TESTS)
  add_test(NAME snesrecomp_version COMMAND snesrecomp --version)

  add_executable(snes_recomp_diff_tests tests/snes_recomp_diff_tests.cpp)
  set_target_properties(snes_recomp_diff_tests PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(snes_recomp_diff_tests PRIVATE snesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(snes_recomp_diff_tests PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/snes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME snes_recomp_diff_tests COMMAND snes_recomp_diff_tests)

  add_executable(snes_smb_recomp_test tests/snes_smb_recomp_test.cpp)
  target_link_libraries(snes_smb_recomp_test PRIVATE snesrecomp_lib dragon_warnings)
  target_compile_definitions(snes_smb_recomp_test PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/snes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME snes_smb_recomp_test COMMAND snes_smb_recomp_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(snes_smb_recomp_test PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(smb_snes_boot_test tests/smb_snes_boot_test.cpp)
  set_target_properties(smb_snes_boot_test PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(smb_snes_boot_test PRIVATE snesrecomp_lib ${CMAKE_DL_LIBS} dragon_warnings)
  target_compile_definitions(smb_snes_boot_test PRIVATE
    DRAGON_C_COMPILER="${CMAKE_C_COMPILER}"
    DRAGON_RUNTIME_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/snes/runtime/include"
    DRAGON_BINARY_DIR="${CMAKE_BINARY_DIR}")
  add_test(NAME smb_snes_boot_test COMMAND smb_snes_boot_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(smb_snes_boot_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
