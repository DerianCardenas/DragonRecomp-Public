# Tasks: 001 — Correcciones Studio + runtime SNES

## Estado general: ✅ IMPLEMENTADA — build limpio, 19/19 CTest en verde

> Notas de implementación (ver también plan.md):
> - **F1**: bastó con quitar la clave `mode`; NO se necesitó `[[mx_hints]]`
>   (la config curada ya usa entry_points sin `mode` y el recompilador deriva
>   M/X del seed). Confirmado por `smb_snes_boot_test`.
> - **F3**: `0x0884` resultó ser WRAM `$00:0884` (rutina residente en RAM con
>   truco de pila). Se expuso vía `stack_tricks` en config → tabla generada
>   `kRecompStackTrickTargets[]` → `Bus::is_stack_trick_target(bank, addr)`.
>   Un valor de 16 bits casa cualquier banco (paridad con el literal previo);
>   un valor de 24 bits casa `(bank, addr)` exacto. `smb_snes.toml` ahora lleva
>   `stack_tricks = [0x0884]`.
> - **F7**: el divisor de bancos depende del mapper (16 KiB para 1/2, 8 KiB
>   para 4, solo banco 0 para 0/3), según `Bus::current_prg_bank`.
> - Se incluyó además la **carrera de datos de `active_rom_`** (hallazgo #5 del
>   panel), que la primera versión de esta spec había omitido: la asignación va
>   bajo mutex y el hilo monitor recibe la plataforma por parámetro.
> - F5/F6/F8 convergieron en el helper `run_process` (argv explícito, sin
>   shell), usado por compilación, recompilador y lanzamiento del juego.

Partición por área de código (no hay front/back). **Subagente A** y
**Subagente B** no comparten archivos, así que pueden ejecutarse en paralelo.
Marcar cada casilla en tiempo real durante la implementación (no ahora).

---

## Subagente A — Studio (`studio/`)
Archivos: `studio/src/pipeline.cpp`, `studio/include/studio/pipeline.h`,
`studio/src/app.cpp`, `studio/tests/studio_unit_tests.cpp`.
No tocar archivos de `snes/` ni `common/`.

### F1 — Clave `mode` inválida (grave)
- [x] (lectura) Confirmar en `snes/recompiler/src/disassembler.cpp` si los
      entry points aprendidos derivan M/X por rastreo desde el seed.
- [x] En `optimize_and_recompile()` quitar `mode = 0x%02X` y el sufijo `_m%02X`
      del `name` en el bloque SNES; emitir solo `addr/bank/name`.
- [x] Si el rastreo NO basta: emitir además `[[mx_hints]]` (`addr/m/x`)
      decodificando `ev.mode` según `cpu_mode_flags()`.
- [x] CTest: generar config con una entrada "aprendida" SNES y verificar que
      `dragon::load_game_config` la parsea sin lanzar (hoy lanza).
- **Aceptación**: tras aprender un fallback SNES, `optimize_and_recompile`
  produce un TOML que el recompilador parsea y las entradas sobreviven.

### F4 — Dedup estructural (media)
- [x] Reemplazar la búsqueda de texto `needle`/`needle_lower` por: cargar la
      config con `dragon::load_game_config` y comparar `(bank, addr)` contra
      `cfg.entry_points`.
- [x] Manejar TOML no parseable (conjunto vacío → anexar todas).
- [x] CTest: dos entradas con misma `addr` y distinto `bank` no se deduplican
      entre sí; una `addr` presente en otra tabla no bloquea un entry point
      nuevo.
- **Aceptación**: entradas de bancos distintos con igual `addr` se conservan.

### F5+F6+F8 — Helper de subproceso sin shell (medias + baja)
- [x] Crear helper `run_process(argv, &err_detail)` → `{exit_code, output,
      spawned}` sin `/bin/sh` (`posix_spawn` o `fork`+`execvp` + `pipe`),
      con argv y rutas construidos ANTES del fork; en el hijo solo llamadas
      async-signal-safe. Rellenar `err_detail` siempre que falle el spawn (F8).
- [x] Reescribir las 3 fases de `compile_c_to_binary` (clang de cada `.c`,
      clang++ del runner, enlace) como vectores de argumentos; pasar
      `-DDRAGON_GAME_TITLE=<valor>` como un solo argv sin comillas anidadas.
- [x] Reescribir la ejecución del recompilador en `run_worker` con el helper.
- [x] Reescribir `launch_game` para usar argv pre-construido (variante con pid
      para monitoreo), eliminando el trabajo no async-signal-safe tras `fork`.
- [x] CTest: `run_process` con un argv cuyo nombre contiene `"`/espacio/`$` se
      pasa como dato literal (sin interpretación de shell) y sin romperse.
- **Aceptación**: ROM con nombre "raro" compila/lanza sin romper ni inyectar;
      fallo de spawn deja `error_msg_` con mensaje útil.

### F7 — Nº de bancos NES real (media)
- [x] (lectura) Determinar el modelo de bancos de `nesrecomp`/runtime
      (`nes/runtime/src/bus.cpp::current_prg_bank`, cartridge, mappers 1/2/4).
- [x] Ajustar `total_banks` al tamaño de banco real (o derivarlo del
      cartridge/mapper en vez de un divisor fijo).
- [x] CTest: para una ROM de N KB de PRG con mapper banked, el límite de banco
      coincide con el nº real de bancos del modelo.
- **Aceptación**: entradas aprendidas en bancos altos válidos no se descartan.

### F2 — Diagnóstico honesto (grave) — ver ADR-001
- [x] `DiagnosticResult`: `success` solo si hubo acción con efecto comprobable
      y `start_pipeline()/optimize_and_recompile()` no fallaron.
- [x] `summary`/`details` describen la acción real; eliminar textos de
      "arreglo de hardware" inventados; reflejar asincronía ("recompilando
      con <ajuste>").
- [x] Ramas sin auto-fix real devuelven estado neutro/`false` con texto claro.
- [x] Dejar de escribir "reparado" en `*_feedback.toml` (registrar "intento").
- [x] `app.cpp`: el render refleja el resultado del pipeline (`Ready`/`Failed`),
      no un "✔" fijo.
- **Aceptación**: ningún síntoma reporta éxito sin una acción real; la UI no
  afirma reparaciones que no ocurrieron.

---

## Subagente B — Runtime + Recompilador SNES — ver ADR-002
Archivos: `snes/runtime/src/bus.cpp`, `snes/runtime/include/snes/bus.h`,
`common/include/dragon/config.h`, y (opción A) `snes/recompiler/src/emitter.cpp`
o (opción B) `snes/runtime/src/snes_recomp_game_main.cpp`.
No tocar `studio/`.

### F3 — 0x0884 por configuración (grave)
- [x] (investigación) Identificar qué rutina es `0x0884` en SMB SNES (banco,
      ROM/RAM, por qué es motor de saltos / truco RTS) y a qué entrada de
      config corresponde (`jump_engine`/`inline_data_calls`/`stack_tricks`).
- [x] Decidir plumbing (ADR-002): (A) tabla generada
      `kRecompJumpEngineTargets[]` por el recompilador, o (B) setter en el
      `Bus` desde el `main` por juego. Preferida: A.
- [x] Añadir `bool Bus::is_jump_engine_target(uint8_t bank, uint16_t addr)`.
- [x] En `runtime_dispatch`, sustituir `target != 0x0884U` por la consulta por
      `(banco, addr)`. Comprobar banco, no solo addr.
- [x] (opción A) Emitir la tabla en el recompilador desde la config.
- [x] CTest (sin ROM): la tabla/el setter marca correctamente un target de
      ejemplo como jump-engine y `is_jump_engine_target` lo refleja.
- [x] Añadir al `smb_snes.toml` la entrada de config que reproduce el 0x0884
      (si no está ya cubierta por `jump_engine`/`inline_data_calls`).
- [x] Documentar en `docs/SNES_SPEC.md` el origen de los targets de jump-engine.
- **Aceptación**: el literal `0x0884` desaparece del runtime; el
      comportamiento proviene de la config.

---

## Principal (post-integración)
Requiere A y B listos.
- [x] `cmake --build build` sin warnings (`-Wall -Wextra`).
- [x] `ctest --test-dir build --output-on-failure` — toda la suite pasa.
- [x] **Regresión F3**: `snes_smb_snes_boot_test` confirma boot+gameplay de
      SMB SNES idéntico (sin fallbacks) tras quitar el 0x0884.
- [x] Smoke test del Studio: cargar SMB (NES) y SMB SNES, recompilar, aprender
      un fallback, optimizar, y comprobar que las entradas sobreviven (F1/F4).
- [x] Actualizar `plan.md` → "Hallazgos durante implementación" si surgió algo.
- [x] (paso 10) Capturar convenciones nuevas en `AGENTS.md`/`CLAUDE.md`
      (p. ej. "lanzar procesos con argv explícito, nunca `popen`/shell").

## Documentación
- [x] Crear spec.md
- [x] Crear plan.md
- [x] Crear tasks.md
- [x] Crear ADR-001 / ADR-002 / ADR-003 + índice
- [x] Actualizar `docs/MILESTONES.md` si procede al cerrar

---

## Pendiente (fuera de scope)
- [ ] Revisión a fondo del emisor SNES (variantes M/X), PPU (NES/SNES) y
      APU/SPC700 — no cubiertos en la revisión; abrir spec aparte.
- [ ] Diagnóstico "real" basado en señales (opción 2 de ADR-001) — feature
      futura sobre la base honesta.
- [ ] `runtime_poll` tras cada instrucción interpretada en el dispatch SNES
      (coste) — optimización, no bug; evaluar si vale la pena.
