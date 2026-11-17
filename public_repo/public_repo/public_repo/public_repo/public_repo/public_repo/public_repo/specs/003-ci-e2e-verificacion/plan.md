# Plan: Spec 003 — CI y Verificación End-to-End

Orden recomendado: la resolución de ROMs y el contrato E2E son la base; el CI
hosted da la red de seguridad cuanto antes; GB/GBA verify y la honestidad cierran
los huecos de corrección y de mensaje.

## Fase 0: Fundaciones de resolución de ROMs y skip (ADR-007)

1. Helper `find_rom_by_sha1(dir, sha1)` en `common/` (reutiliza `common/src/sha1.cpp`).
2. `DRAGON_ROM_DIR` (default `roms/`) leído por boot/recomp tests y por Studio.
3. Tests ROM-gated: `return 77` al no encontrar la ROM; `set_tests_properties
   (... SKIP_RETURN_CODE 77)` en los `CMakeLists.txt` correspondientes.
4. `DRAGON_REQUIRE_ROMS=1`: convierte skip en fallo.
5. Verificar que SMB/DK/SMB-SNES resuelven contra la biblioteca por SHA-1
   (incluida la descompresión del `.zip` de SNES en un dir temporal, nunca
   modificando la biblioteca).

## Fase 1: CI hosted (ADR-006, Nivel A)

1. `.github/workflows/ci.yml`: matriz Linux `{clang, gcc}` (configure, build,
   ctest) + job Windows MSVC (configure + build).
2. Instalación de deps (`ninja-build`, `libsdl3-dev`) y caché.
3. Confirmar que, sin ROMs, la suite queda en "passed + skipped" y en verde.

## Fase 2: Contrato E2E Nivel 0 y Nivel 1 (ADR-008)

1. Nivel 0: una micro-ROM sintética por plataforma por el pipeline completo
   (extender el patrón de `recomp_diff_tests` a GB/GBA donde falte).
2. Nivel 1: committear ROMs públicas redistribuibles en `roms/test/`
   (blargg, mooneye, gba-suite, gbmicrotest) y un runner por plataforma que
   valide la firma de salida; registrar sus `add_test`.

## Fase 3: Verificación lock-step GB/GBA + extensión (ADR-008)

1. Portar `run_verify_mode` a `gb/runtime/src/frontend.cpp` y
   `gba/runtime/src/frontend.cpp`.
2. Extender la comparación (todas las plataformas): hash de audio + subconjunto
   de registros PPU/IO + N frames con input scripteado.
3. Degradar la descripción de `--verify` si alguna extensión no se completa.

## Fase 4: Nivel 2 con golden-hash

1. Infra de golden-hash: capturar `hash(framebuffer)` en checkpoints desde una
   corrida de referencia; committear los hashes.
2. Sustituir aserciones débiles en NES (SMB/DK) por golden-hash + `--verify`.
3. Definir contrato (checkpoints + input) para un juego GB, uno GBC y uno GBA
   nativos; Pokémon Emerald con contrato `interpreter_only`.

## Fase 5: Honestidad (milestones, reporte, auto-diagnóstico)

1. `report.toml` por juego en el recompilador; columna de modo real en
   `MILESTONES.md`.
2. Renombrar/limpiar el asistente de Studio (cierra ADR-001).

## Fase 6: Deuda técnica dirigida

1. Heurístico determinista de `interpreter_only` en el recompilador.
2. Versionar/invalidar la caché de knowledge de Studio; config gana sobre caché.
3. Medir modelos de audio simplificados con las ROMs del Nivel 1 y documentarlos.

## Fase 7: Verificación y documentación

1. `ctest` completo en ambos niveles; build warning-free.
2. Actualizar `docs/MILESTONES.md`, specs de plataforma y el índice de ADRs.
