# Tareas: Spec 003 — CI y Verificación End-to-End

> Estado: **EN PROGRESO** (implementación inicial 2026-11-15). Ver notas por
> tarea; lo marcado `[x]` está construido y verificado localmente (build +
> ctest, incluyendo una simulación de clon limpio sin `roms/`).

## Tarea 1: Resolución de ROMs y "skip ≠ pass" (ADR-007)

- [x] 1.1 `find_rom_by_sha1(dir, sha1)` en `common/` sobre `common/src/sha1.cpp`.
      → `common/include/dragon/rom_locator.h` + `src/rom_locator.cpp`.
- [x] 1.2 Leer `DRAGON_ROM_DIR` (default `roms/`) en boot/recomp tests.
      Verificado contra la biblioteca real: resuelve `Donkey Kong (JU).nes`
      por SHA-1 sin que el nombre coincida con `dk.nes`.
- [x] 1.3 Tests ROM-gated devuelven `77`; `SKIP_RETURN_CODE 77` — **ya estaba
      correcto** en el código (mi afirmación previa de "return 0" en el
      análisis fue incorrecta); confirmado con `set_tests_properties`.
- [x] 1.4 `DRAGON_REQUIRE_ROMS=1` convierte skip en fallo
      (`dragon::roms_required()`).
- [ ] 1.5 Studio (no solo los tests de CTest) usando el mismo resolver; los
      `add_nes_recomp_executable`/`add_snes_recomp_executable` de CMake siguen
      gateados por `EXISTS` del path fijo (sin SHA-1 a nivel de configure).
      Descompresión del `.zip` de SMB SNES de la biblioteca: no implementada.

## Tarea 2: CI hosted — GitHub Actions (ADR-006, Nivel A)

- [x] 2.1 `.github/workflows/ci.yml` matriz Linux `{clang, gcc}`: configure,
      build, ctest. Verificado localmente simulando un clon limpio
      (`git archive` + rsync sin `roms/`): 100% passed, 6 tests `Skipped`
      (no `Passed`) — confirma el criterio de aceptación 1/2.
- [x] 2.2 Job Windows MSVC: configure + build (no verificable desde este
      entorno Linux; la receta usa `ilammy/msvc-dev-cmd` + Ninja, sin probar
      en un runner Windows real).
- [x] 2.3 `-DDRAGON_BUILD_STUDIO=OFF` en ambos jobs hosted (Studio exige SDL3
      `REQUIRED`, no garantizado en runners hosted; los 4 recompiladores/
      runtimes — la superficie real de los milestones — no lo necesitan,
      `find_package(SDL3 CONFIG QUIET)`). Studio queda cubierto por el nivel
      self-hosted, donde SDL3 ya está instalado.
- [x] `.github/workflows/e2e-roms.yml` (Nivel B, `workflow_dispatch`, sin
      `schedule` activo): documentado con los pasos de setup del runner
      self-hosted; no probado contra un runner real (no existe aún).

## Tarea 3: E2E Nivel 0 — ROM sintética (ADR-008)

- [x] 3.1 NES: ya existía (`recomp_diff_tests.cpp`, `mapper_unit_tests.cpp`).
- [x] 3.2 SNES: ya existía (`snes_recomp_diff_tests.cpp`).
- [ ] 3.3 GB: no añadido en esta pasada.
- [ ] 3.4 GBA: no añadido en esta pasada.

## Tarea 4: E2E Nivel 1 — ROMs públicas de precisión (ADR-008)

- [x] 4.1 NES `nestest.nes`/`nestest.log`: **corrección real encontrada y
      cerrada** — estaban en disco pero NO committeados (`roms/` los
      gitignoraba por completo). Se cambió `.gitignore` de `roms/` a
      `roms/*` + excepciones explícitas para estos dos archivos. Verificado:
      en el clon-limpio simulado, `nes_nestest` corre y pasa (ya no aparece
      en la lista de `Skipped`). blargg `cpu_instrs`/`apu_test`: no añadidos
      (no se descargó ni se fabricó ningún binario de terceros en esta
      pasada — ver "Bloqueado" abajo).
- [ ] 4.2 blargg/mooneye GB + runner de firma de salida: **bloqueado**.
- [ ] 4.3 `gba-suite` (jsmolka) + runner: **bloqueado**.
- [ ] 4.4 Test ROMs SNES públicas + runner: **bloqueado**.
- [ ] 4.5 Registrar `add_test` de cada runner de Nivel 1: pendiente de 4.2-4.4.

  **Bloqueado, no silenciosamente omitido**: estas ROMs de terceros
  (redistribuibles pero no presentes en este entorno ni descargadas por esta
  sesión) requieren que el usuario las aporte o confirme una fuente de
  descarga concreta antes de committearlas. Ningún binario fue fabricado ni
  descargado sin esa confirmación.

## Tarea 5: Verify lock-step GB/GBA + extensión (ADR-008)

- [ ] 5.1 Portar `run_verify_mode` a `gb/runtime/src/frontend.cpp`: **no
      implementado en esta pasada** (requiere exponer `fallback_count()`
      público en `gb::Bus`/`gba::Bus`, que hoy solo tienen `log_fallback`
      privado-ish sin contador expuesto, más una ejecución paralela
      recompilado-vs-intérprete). Alcance identificado y factible; queda para
      una siguiente iteración dedicada.
- [ ] 5.2 Ídem GBA: no implementado (ARM/Thumb añade complejidad de modos).
- [ ] 5.3 Extensión (audio + registros PPU/IO): no implementada en ninguna
      plataforma — depende de 5.1/5.2 y de NES/SNES existentes.
- [x] 5.4 Mientras no se extienda: el `--help` de NES/SNES ya describe
      `--verify` correctamente como "lock-step vs interpreter" sin prometer
      audio; no se requirió cambio.

## Tarea 6: E2E Nivel 2 — golden-hash

- [x] 6.1 Infra de golden-hash: `hash_framebuffer()` (SHA-1 sobre el RGBA32
      crudo) en `dk_boot_test.cpp`; reutiliza `dragon::sha1_hex`.
- [x] 6.2 **DK (NES): hecho y verificado con la ROM real** (biblioteca del
      usuario, vía el `roms/nes/dk.nes` local) — hashes capturados de una
      corrida real, no inventados: título y 25m de la Fase 1. Las aserciones
      débiles (`distinct_colors`, etc.) se conservan como diagnóstico legible
      si el hash llega a fallar, en vez de borrarse.
- [ ] 6.3 Contrato GB nativo: no implementado (sin config GB en el repo).
- [ ] 6.4 Contrato GBC/GBA nativo: no implementado (sin configs en el repo).
- [ ] 6.5 Contrato `interpreter_only` para Emerald: no implementado como test
      automatizado (el mecanismo ya existe en `pipeline.cpp`, ver Tarea 9).

## Tarea 7: Honestidad — reporte y milestones

- [x] 7.1 `report.toml` por juego: implementado en los 4 recompiladores
      (`nesrecomp`, `snesrecomp`, `gbrecomp`, `gbarecomp`), cada uno con sus
      propios campos relevantes (`interpreter_only`, funciones, bloques,
      instrucciones, jump tables donde aplica). Verificado ejecutando
      `nesrecomp` sobre SMB real y leyendo el `report.toml` resultante.
- [x] 7.2 `docs/MILESTONES.md`: añadida sección "2026-11-15 — infraestructura
      de verificación" con tabla de modo real por juego (nativo/híbrido/
      intérprete) y la regla de oro de la spec.

## Tarea 8: Finalizar honestidad del auto-diagnóstico (cierra ADR-001)

- [x] 8.1 `studio/src/app.cpp`: el *backend* (`Pipeline::diagnose_and_repair`)
      **ya era honesto** (ADR-001 ya estaba `Implementado`, contrario a mi
      lectura inicial de la auditoría) — reporta la acción real o dice
      explícitamente "No hay una reparación automática para este síntoma".
      Lo que faltaba era la *UI*: el panel decía "ASISTENTE DE DIAGNOSTICO Y
      AUTO-REPARACION" / "Intentar Reparar" / "selecciónalo para repararlo
      automáticamente", lo que sí sobrevendía. Renombrado a "ACCIONES DE
      RECUPERACION" / "Intentar Accion", con texto que dice explícitamente
      que no todos los síntomas tienen una acción automática.

## Tarea 9: Deuda técnica dirigida

- [ ] 9.1 Heurístico determinista de `interpreter_only` en el recompilador:
      no implementado.
- [x] 9.2 Invalidación de la caché de knowledge / config-gana-sobre-caché:
      **ya existía** (`requires_interpreter_only()` + `ensure_interpreter_only()`
      en `pipeline.cpp`, aplicado en los 4 puntos donde se lee la caché de
      usuario o el config del repo) — es un allowlist de SHA-1/nombre
      hardcodeado, no un esquema de versión de caché genérico. Cierra el caso
      concreto de Pokémon Emerald/SMW que motivó esta tarea; un juego
      `interpreter_only` nuevo sigue necesitando una entrada de código, no
      solo un cambio de config — limitación conocida, no resuelta aquí.
- [ ] 9.3 Medir ADSR SPC700 / timing APU GB con ROMs de audio: bloqueado por
      la Tarea 4 (sin ROMs de audio de Nivel 1 disponibles todavía).

## Tarea 10: Cierre

- [x] 10.1 `ctest` verde: 29/29 local (con ROMs reales del usuario) y 28/28 +
      6 `Skipped` en clon limpio simulado (`git archive` + rsync sin `roms/`).
      Build *warning-free* en ambos casos (`-Wall -Wextra -Wpedantic`, clang
      y gcc en este entorno; MSVC no verificado aquí).
- [x] 10.2 `docs/MILESTONES.md` actualizado (Tarea 7.2). `decisions/adr-000-index.md`
      ya tenía ADR-006/007/008 desde la creación de la spec. Specs de
      plataforma (NES_SPEC.md, etc.) no tocadas en esta pasada.

## Tarea 11: Portabilidad real de los paquetes exportados (añadida 2026-11-15)

Fuera del alcance original de la spec, pero reutiliza directamente el
resolver de ADR-007: se descubrió que el ejecutable que exporta Studio graba
la **ruta absoluta del ROM en la máquina que lo compiló** (`DRAGON_DEFAULT_ROM_PATH`),
confirmado con `strings` sobre un export real (`ExportedGames/smb_Linux/smb`).
Compartir esa carpeta con otra persona, o mover el proyecto, rompía el
arranque. Igual que Zelda64Recomp/N64Recomp/nxnative, la solución correcta
**no es** incluir el ROM en el paquete (seguiría siendo distribuir datos con
copyright) sino que el ejecutable resuelva su propio ROM por SHA-1.

- [x] 11.1 Los 4 `*_recomp_game_main.cpp` (nes/snes/gb/gba) ahora llaman a
      `dragon::resolve_rom_path` tras parsear los argumentos: si la ruta
      grabada en compilación no existe, buscan por SHA-1 en `roms/` junto al
      ejecutable (misma función que ya usan los tests de CI, ADR-007), y si
      no la encuentran, un mensaje claro en vez de un fallo críptico.
- [x] 11.2 `studio/src/pipeline.cpp`: la rama de cross-compilación Windows vía
      mingw-w64 no enlazaba `libdragon_common.a` (compilada para el triple
      del host, no mingw); se añadió compilar `rom_locator.cpp`/`sha1.cpp`/
      `file_io.cpp` con `x86_64-w64-mingw32-g++` y enlazarlos. Las rutas
      nativas (Linux y Windows-nativo) ya enlazaban `libdragon_common.a`
      completo, sin cambios.
- [x] 11.3 `studio/src/packager.cpp`: nuevo `Packager::create_rom_placeholder`
      — crea `<paquete>/roms/` con un `LEEME.txt` explicando que el ROM no
      viene incluido y cómo aportar la copia propia; invocado en ambos flujos
      de empaquetado (Linux y Windows). Los dos `README.txt` generados
      también mencionan la carpeta `roms/`. **Nunca copia el ROM.**
- [x] 11.4 Verificado end-to-end con el `dk_recomp` real: con `--rom` apuntando
      a una ruta inexistente (simula la ruta grabada no existiendo en otra
      máquina) falla con el mensaje nuevo; con una copia del ROM bajo
      `./roms/` con **otro nombre de archivo**, arranca igual
      (`exit=0`) — confirma la resolución por SHA-1, no por nombre.
- [x] 11.5 Documentado en `README.md` (aviso superior + sección dedicada
      "📦 ROM original en paquetes exportados").
