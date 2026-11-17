# Plan técnico: 001 — Correcciones Studio + runtime SNES

> NO EJECUTAR hasta aprobación del gate (ver `spec.md`).

Convenciones aplicables (de `AGENTS.md`): C++20 en Studio/runtime, C99 en
código generado; `snake_case` funciones/vars, `PascalCase` tipos; sin
excepciones/heap/virtual en hot paths (no aplica a Studio ni a la ruta de
config, sí al runtime_dispatch); build sin warnings; **un CTest por feature
testeable sin ROM**; los tests que necesiten ROM deben *saltarse* (skip), no
fallar, si falta la ROM.

---

## F1 — Clave `mode` inválida en entry points SNES

### Flujo actual
`optimize_and_recompile()` → por cada fallback SNES válido emite
`[[entry_points]] addr/bank/mode/name` → al recompilar, `load_game_config`
lanza `Unknown key 'mode'` → `run_worker` regenera TOML limpio → se pierden
las entradas aprendidas.

### Cambio
- En `optimize_and_recompile()` (`pipeline.cpp:~805-815`), **eliminar** el
  campo `mode = 0x%02X` del bloque SNES y el sufijo `_m%02X` del `name`.
  Emitir únicamente `addr`, `bank`, `name` (esquema válido).
- Evaluar si el estado M/X capturado (`ev.mode`) es necesario para que el
  recompilador genere la variante correcta. El recompilador SNES ya deriva
  `(m, x, e)` por rastreo estático desde cada seed; si el rastreo desde un
  entry point aprendido es suficiente, basta con quitar `mode`. Si NO lo es,
  emitir además un `[[mx_hints]]` (tabla **ya soportada**:
  `common/src/config.cpp:353`, claves `addr/m/x`) decodificando `ev.mode`
  según `cpu_mode_flags()` (bit de `m` y bit de `x`).
- **Tarea de verificación previa**: leer `snes/recompiler/src/disassembler.cpp`
  para confirmar cómo se siembran los entry points y si respetan el M/X del
  rastreo. De ahí sale si `[[mx_hints]]` es opcional u obligatorio.

### Decisión técnica
Preferir el esquema existente (`[[mx_hints]]`) sobre añadir un campo `mode` a
`EntryPoint` + loader + recompilador: menos superficie, sin tocar el contrato
de config. (No llega a ADR: es uso de una tabla ya existente.)

---

## F2 — Diagnóstico honesto

### Flujo actual
`diagnose_and_repair(symptom)` ejecuta una acción (recompilar / toggle
mirroring / limpiar generated) y **siempre** devuelve `success=true` con un
texto fijo que afirma un arreglo concreto; `app.cpp` lo pinta como "✔"; se
anexa a `*_feedback.toml`.

### Cambio
- `DiagnosticResult` pasa a reflejar la acción **real**:
  - `success` solo `true` cuando se realizó una acción con efecto
    comprobable y esa acción no falló de inmediato (p. ej.
    `optimize_and_recompile()` devolvió `true` y había entradas nuevas, o se
    escribió un cambio de `mirroring`, o `start_pipeline()` devolvió `true`).
  - `summary`/`details` describen lo que de verdad se hizo, no un arreglo de
    hardware inventado. Ejemplos honestos:
    - "Se integraron N rutinas aprendidas y se inició la recompilación."
    - "Se alternó el mirroring PPU (horizontal→vertical) y se recompiló."
    - "No hay una reparación automática para este síntoma; se recompiló sin
      cambios." → `success=false` o estado neutro.
- **Asincronía**: el pipeline corre en `worker_thread_`. El diagnóstico NO
  puede afirmar éxito final sincrónicamente. El resultado honesto es "acción
  iniciada: recompilando con <ajuste>"; la UI (`app.cpp`) debe reflejar el
  resultado del pipeline (`Ready`/`Failed`) y no un "reparado" instantáneo.
- Dejar de escribir en `*_feedback.toml` afirmaciones de arreglo; si se
  conserva el log, registrar "intento / acción aplicada", no "reparado".
- Ramas sin acción real (p. ej. `AbnormalSpeed`, `ControlsUnresponsive` sin un
  cambio de config) deben decir claramente que no hay auto-fix y, si acaso,
  recompilar es opcional; no afirmar calibraciones inexistentes.
- Respetar el valor de retorno de `start_pipeline()` (si es `false`, no
  reportar éxito).

Ver **ADR-001** (por qué "honesto" y no "diagnóstico real" ni "eliminar").

### Archivos
`studio/include/studio/pipeline.h` (`DiagnosticResult`),
`studio/src/pipeline.cpp` (`diagnose_and_repair`),
`studio/src/app.cpp` (render del resultado).

---

## F3 — 0x0884 por configuración (el más grande)

### Flujo actual
`runtime_dispatch` (`snes/runtime/src/bus.cpp:919-951`) interpreta opcodes en
fallback y decide cuándo devolver el control. Para SMB SNES, un `RTS` del
motor de saltos NO debe terminar el despacho; eso se fuerza con
`target != 0x0884U`.

### Cambio (config-driven)
1. **Config**: el esquema ya tiene `jump_engine` / `inline_data_calls`
   (`kind="jump_engine"`) y `stack_tricks`. Definir que los targets de
   motor-de-saltos/truco-RTS se tomen de ahí (con banco cuando aplique).
2. **Plumbing config → runtime**. Dos opciones (ver ADR-002):
   - (A) El recompilador emite una tabla generada
     `kRecompJumpEngineTargets[] = {{bank, addr}, …}` (análoga a
     `kRecompDispatchTable`), y el runtime la consulta.
   - (B) El `main` por juego (`snes_recomp_game_main.cpp`) lee la config y
     registra los targets en el `Bus` al arrancar
     (`bus.set_jump_engine_targets(...)`).
3. **Runtime**: añadir `bool Bus::is_jump_engine_target(uint8_t bank,
   uint16_t addr) const` y reemplazar el literal:
   `if (!bus->is_jump_engine_target(bank_actual, target) && cpu->s >= entry_s + 2U) { … }`
   Comprobar **banco + addr**, no solo addr.
4. **Regresión**: antes de quitar el literal, confirmar qué rutina es 0x0884
   en SMB SNES (bank, si es RAM/ROM, por qué es jump-engine) y que la ruta de
   config reproduce exactamente el mismo comportamiento (boot + gameplay sin
   fallbacks). El test `snes_smb_snes_boot_test` es el guardián.

### Decisión técnica
Preferir (A) tabla generada por el recompilador: mantiene el runtime genérico
y la fuente de verdad en el TOML+recompilador (coherente con
`kRecompDispatchTable`). (B) queda como alternativa si no se quiere tocar el
emisor. Ver **ADR-002**.

---

## F4 — Dedup estructural de entry points

### Flujo actual
`optimize_and_recompile` arma `needle = "addr = 0x%04X"` y busca en el texto
completo del TOML (`pipeline.cpp:781-790`).

### Cambio
- Cargar la config existente con `dragon::load_game_config(toml_path_)` y
  construir un `std::set<std::pair<int,uint32_t>>` de `(bank, addr)` a partir
  de `cfg.entry_points`.
- Un fallback se considera "ya presente" solo si `(bank, addr)` coincide.
  Elimina tanto el falso positivo entre tablas como la colisión de bancos.
- Reutiliza el loader (menos frágil que parsear texto a mano).
- Manejar el caso de TOML no parseable (fallback a conjunto vacío → se anexan
  todas; el recompilador validará).

---

## F5 + F6 + F8 — Helper de subproceso sin shell

### Flujo actual
4× `popen(cmd, "r")` con comandos de shell concatenados (`pipeline.cpp:348,
386, 414, 496`) y un `fork()/execv()` manual en `launch_game`
(`pipeline.cpp:671-713`) con trabajo no async-signal-safe en el hijo.

### Cambio
- Nuevo helper interno (p. ej. `run_process`) en `pipeline.cpp` (anónimo) o en
  un pequeño util:
  ```
  struct ProcResult { int exit_code; std::string output; bool spawned; };
  ProcResult run_process(const std::vector<std::string>& argv,
                         std::string* err_detail);
  ```
  - Usa `posix_spawn` **o** `fork` + `execvp` con **argv explícito** (sin
    `/bin/sh`), `pipe()` para capturar stdout+stderr.
  - **Todo el trabajo que asigna memoria (construir argv, rutas absolutas) se
    hace ANTES del `fork`/spawn**; en el hijo solo llamadas
    async-signal-safe (`dup2`, `close`, `execvp`, `_exit`).
  - Siempre rellena `err_detail` en fallo de spawn (cubre **F8**).
- Reescribir las 3 fases de `compile_c_to_binary` (clang de cada `.c`, clang++
  del runner, enlace) y la ejecución del recompilador (`run_worker`) como
  vectores de argumentos (nada de comillas; `-DDRAGON_GAME_TITLE=<valor>` como
  elemento argv único, sin comillas anidadas).
- `launch_game` usa el mismo helper (o su variante no bloqueante con pid), con
  argv construido antes del `fork`. Elimina **F6**.

Ver **ADR-003** (cambio de patrón shell → argv).

### Nota de seguridad
Esto neutraliza la inyección de shell (F5) porque los argumentos ya no pasan
por `/bin/sh`. Los nombres con comillas/espacios/`$` se tratan como datos.

---

## F7 — Nº de bancos NES desde el modelo real

### Flujo actual
`total_banks = prg_size_kb / 8U` (`pipeline.cpp:761`).

### Cambio
- **Investigación previa**: leer cómo `nesrecomp` y el runtime numeran bancos
  PRG (`nes/runtime/src/bus.cpp::current_prg_bank`, cartridge, mappers 1/2/4).
- Ajustar el divisor al tamaño de banco real del modelo (p. ej. 16 KB →
  `prg_size_kb / 16`), **o** mejor, obtener el conteo de bancos del
  cartridge/mapper en lugar de calcularlo con un divisor fijo.
- Si el modelo mezcla tamaños por mapper, derivar por mapper.

---

## Hallazgos durante implementación
- `0x0884` es WRAM `$00:0884` (rutina residente en RAM con truco de pila), no
  el jump engine `0x8E04`. Se modeló como `stack_tricks` en config.
- El plumbing config→runtime se hizo con el mismo patrón que `kRecompDispatchTable`:
  tabla generada por el emisor + `RunnerOptions` + `Bus::set_stack_trick_targets`.
  Se cablearon también `smb_snes_boot_test` y `snes_recomp_diff_tests` para no
  perder el comportamiento (son el guardián de regresión; ambos pasan).
- F1 no necesitó `[[mx_hints]]`: los entry_points sin `mode` ya funcionan y el
  recompilador deriva M/X del seed (verificado por el boot test de SMB SNES).
- F7: `current_prg_bank` devuelve índices de 16 KiB (mappers 1/2) u 8 KiB
  (mapper 4); el límite se calcula por mapper.
- Se añadió el fix de la carrera de datos de `active_rom_` (hallazgo #5),
  omitido por error en la primera redacción de spec.md.
- Resultado: `cmake --build build` sin warnings; `ctest` 19/19 en verde.

## Archivos involucrados

| Archivo | Tipo de cambio |
|---------|---------------|
| `studio/src/pipeline.cpp` | F1,F2,F4,F5,F6,F7,F8 |
| `studio/include/studio/pipeline.h` | helper subproceso, `DiagnosticResult` |
| `studio/src/app.cpp` | render honesto F2 |
| `snes/runtime/src/bus.cpp` | F3 |
| `snes/runtime/include/snes/bus.h` | F3 API |
| `snes/recompiler/src/emitter.cpp` | F3 (opción A) |
| `snes/runtime/src/snes_recomp_game_main.cpp` | F3 (opción B) |
| `common/include/dragon/config.h` | F3 (exponer targets) |
| `studio/tests/studio_unit_tests.cpp` | CTests F1,F4,F5 |
| `snes/runtime/tests/*` | CTest F3 |
