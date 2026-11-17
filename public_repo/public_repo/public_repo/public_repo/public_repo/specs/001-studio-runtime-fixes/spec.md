# Spec: 001 — Correcciones Studio + runtime SNES (9 bugs de revisión)

> Estado: **IMPLEMENTADA Y VERIFICADA**. Esta spec ha sido completada en su totalidad;
> implementada y verificada mediante la suite de CTests (19/19 en verde) y pruebas
> de integración end-to-end con NES y SNES.

## Contexto

Revisión de código (alto esfuerzo) sobre `studio/src` y los runtimes.
Resultado: los núcleos de emulación (CPU NES, emisor NES, aritmética BCD
SNES, despacho/NMI) están correctos; la deuda real está en la **capa Studio**
(`studio/src/pipeline.cpp`) y en **un punto del runtime SNES**
(`snes/runtime/src/bus.cpp`).

Archivos clave:

- `studio/src/pipeline.cpp` — pipeline de recompilación, auto-aprendizaje de
  fallbacks, diagnóstico/auto-reparación, lanzamiento y empaquetado del juego.
- `snes/runtime/src/bus.cpp` — `runtime_dispatch` del runtime SNES.
- `common/src/config.cpp`, `common/include/dragon/config.h` — loader TOML
  (valida claves permitidas; lanza ante claves desconocidas).
- `snes/recompiler/src/emitter.cpp` — emisor C99 SNES (afectado solo por el
  fix del hardcode 0x0884, si se opta por tabla generada).

Línea base verificada contra el commit `77426e0` (último de Gemini): ninguno
de los 9 hallazgos fue corregido por ese commit; solo cambiaron números de
línea respecto a la primera revisión. Las líneas citadas aquí son las de
`77426e0`.

## Impacto estimado

- **Alcance**: `studio/` (pipeline), `snes/runtime`, `common/config`,
  `snes/recompiler` (solo Fix 3). Sin cambios en los núcleos CPU/PPU/APU.
- **Riesgo**: **medio**. Ningún cambio toca la ruta de emulación caliente;
  el de mayor riesgo es el 0x0884 (Fix 3) porque altera control de flujo del
  despacho SNES y debe reproducir exactamente el comportamiento actual de
  SMB SNES antes de quitar el literal.
- **Usuarios afectados**: usuarios del Studio (auto-aprendizaje y
  auto-reparación hoy no funcionan como anuncian); juegos SNES distintos de
  SMB potencialmente afectados por el hardcode 0x0884.
- **Dependencias**: Fix 3 cruza config → recompilador → runtime → main por
  juego. Fixes 5/6/8 convergen en un helper de subproceso común.

## Problema

El Studio ofrece dos features de automatización —**auto-aprendizaje de
fallbacks** y **asistente de auto-reparación de 11 síntomas**— que en el
estado actual no cumplen lo que prometen, y el runtime SNES contiene un
parche específico de un juego metido en infraestructura compartida. Detalle
por hallazgo en "Causa raíz".

## Causa raíz (por hallazgo)

### 🔴 Graves

**F1 — Clave `mode` inválida rompe el auto-aprendizaje SNES**
`pipeline.cpp:807` — `optimize_and_recompile()` escribe
`[[entry_points]] … mode = 0x%02X` para SNES. El loader
(`common/src/config.cpp:187`) solo admite `{addr, bank, name}` en
`[[entry_points]]` y **lanza excepción ante cualquier clave desconocida**.
La siguiente recompilación falla al parsear → `run_worker` (`pipeline.cpp:455`)
captura la excepción, pone `need_gen=true` y **regenera el TOML desde cero,
descartando todas las rutinas aprendidas**. El bucle de auto-aprendizaje SNES
nunca converge, y el respaldo en el directorio de conocimiento de usuario
también queda corrupto.

**F2 — `diagnose_and_repair` siempre reporta éxito (cosmético)**
`pipeline.cpp:~865-988` — las 8 ramas del `switch` ponen `res.success = true`
incondicionalmente. Varias (velocidad anormal, controles, sonido sin
fallbacks pendientes) solo llaman `start_pipeline()` con la config sin
cambios → recompila una entrada idéntica a un binario idéntico (cero cambio de
comportamiento). La UI muestra "✔ reparado" con textos que afirman arreglos de
hardware que nunca ocurrieron, y se escribe una línea en `*_feedback.toml`
registrando el "arreglo". Además `start_pipeline()` puede devolver `false`
(pipeline ya en curso) y aun así se reporta éxito.

**F3 — Dirección `0x0884` hardcodeada en el runtime SNES**
`snes/runtime/src/bus.cpp:935` —
`if (target != 0x0884U && cpu->s >= entry_s + 2U) { … }`. El `0x0884` es un
hack específico de SMB SNES (motor de saltos / truco RTS) incrustado en el
`runtime_dispatch` **genérico**, sin comentario, sin ruta de config y **sin
comprobar el banco**. Cualquier otro juego cuyo `target` de despacho valga
0x0884 no detectará el retorno por RTS → el bucle intérprete sigue ejecutando
más allá del RTS previsto (cuelgue o flujo corrupto). También deja que un
comportamiento específico de SMB afecte en silencio a todos los títulos.

### 🟡 Medias (todas en `pipeline.cpp`)

**F4 — Dedup de entry points ignora el banco y la tabla** — `pipeline.cpp:781`
La comprobación "ya existe" busca el texto `addr = 0x%04X` (y minúscula) en
**todo** el TOML, sin importar en qué tabla ni banco aparece. Descarta entry
points nuevos y válidos que compartan dirección de 16 bits con otro (típico en
SNES: misma `addr`, distinto banco), o que coincidan con una `addr` en otra
tabla.

**F5 — Comandos `popen` construidos con rutas/título sin escapar** —
`pipeline.cpp:348, 386, 414, 496`
Las órdenes de compilación/enlace/recompilación se arman concatenando la ruta
de la ROM y `rom.clean_name` en comandos de shell (`popen` → `/bin/sh`), con
comillas ingenuas y sin escapar comillas, `$` ni backticks. Un archivo con
`"`, `` ` `` o `$` rompe la compilación (p. ej. el `-DDRAGON_GAME_TITLE`) o,
con un nombre manipulado, permite inyección de shell.

**F6 — Trabajo no async-signal-safe entre `fork` y `execv`** —
`pipeline.cpp:671-713`
Entre `fork()` y `execv()` el hijo ejecuta `std::filesystem::absolute`,
operaciones de `std::string` y `std::vector::push_back` (heap) — ninguna
async-signal-safe — en un proceso con otros hilos (worker/monitor). Si otro
hilo tiene el lock de `malloc` en el momento del `fork`, el hijo se bloquea
antes del `execv`.

**F7 — Chequeo de bancos NES asume bancos de 8 KB** — `pipeline.cpp:761`
`total_banks = prg_size_kb / 8U` asume bancos PRG de 8 KB, pero el iNES cuenta
PRG en unidades de 16 KB y el modelo de bancos del recompilador puede diferir.
El límite puede quedar al doble: descarta entradas válidas de bancos altos o
admite un banco fuera de rango.

### ⚪ Baja

**F8 — `popen()==NULL` devuelve `false` sin fijar `error_msg_`** —
`pipeline.cpp:348, 386, 414`
Si `popen` falla, `run_worker` pone `stage_=Failed` sin mensaje y el panel de
error del Studio queda vacío o con texto viejo.

## Solución propuesta (resumen; el "cómo" va en plan.md)

| Fix | Enfoque | Decisión | ADR |
|-----|---------|----------|-----|
| F1 | Dejar de emitir `mode` en `[[entry_points]]`; si el estado M/X importa, emitir `[[mx_hints]]` (tabla ya soportada). | — | — |
| F2 | Hacer el diagnóstico **honesto**: reportar solo la acción real y su resultado; eliminar textos/feedback falsos. | Elegida sobre "diagnóstico real" y "eliminar feature". | ADR-001 |
| F3 | Mover los targets de motor-de-saltos/truco-RTS a **config por juego** y consultarlos en `runtime_dispatch` (con banco). | Elegida sobre mantener el literal. | ADR-002 |
| F4 | Dedup **estructural**: parsear la config existente con `load_game_config` y comparar `(addr, bank)`. | — | — |
| F5+F6+F8 | Helper único de subproceso **sin shell** (argv explícito, pipe de captura, errores siempre poblados), usado por compilación y lanzamiento; argv construido antes del `fork`. | Elegida sobre seguir con `popen`/shell. | ADR-003 |
| F7 | Derivar el nº de bancos del modelo real del recompilador/mapper, no de un divisor fijo. | — | — |

## Análisis de impacto

- **F1** cambia el texto TOML que emite el Studio; no afecta configs curadas
  (`nes/games`, `snes/games`). Verificar que el recompilador SNES deriva M/X
  del seed (si no, `[[mx_hints]]` es necesario, no opcional).
- **F2** es solo Studio (pipeline + `app.cpp` muestra el mensaje). Sin efecto
  en emulación. Ojo: el pipeline es asíncrono, así que "honesto" implica
  reportar "acción iniciada / recompilando con X", no "reparado" instantáneo.
- **F3** es el de mayor superficie: toca `config`, el emisor/recompilador SNES
  (si se opta por tabla generada), `snes/runtime/src/bus.cpp` y el `main` por
  juego. **Regresión obligatoria**: SMB SNES debe comportarse idéntico antes de
  quitar el literal 0x0884.
- **F4** reutiliza el loader existente (menos frágil que búsqueda de texto).
- **F5/F6/F8** convergen: un helper común reduce duplicación (hoy hay 4
  `popen` + 1 `fork/execv` casi iguales).
- **F7** requiere inspección previa del modelo de bancos del recompilador NES
  (`current_prg_bank`, cómo `nesrecomp` numera bancos).

## Archivos que se van a modificar

| Archivo | Tipo de cambio | Capa |
|---------|---------------|------|
| `studio/src/pipeline.cpp` | modificar (F1,F2,F4,F5,F6,F7,F8) | Studio |
| `studio/include/studio/pipeline.h` | modificar (firma del helper de subproceso; `DiagnosticResult`) | Studio |
| `studio/src/app.cpp` | modificar (mostrar resultado honesto de F2) | Studio |
| `snes/runtime/src/bus.cpp` | modificar (F3: quitar 0x0884, consultar config) | Runtime SNES |
| `snes/runtime/include/snes/bus.h` | modificar (F3: API `is_jump_engine_target`) | Runtime SNES |
| `common/include/dragon/config.h` | posible (F3: exponer targets de motor de saltos ya parseados) | Common |
| `snes/recompiler/src/emitter.cpp` | posible (F3: emitir tabla de targets jump-engine) | Recompilador SNES |
| `snes/runtime/src/snes_recomp_game_main.cpp` | posible (F3: registrar targets en el bus al arrancar) | Runtime SNES |
| `studio/tests/studio_unit_tests.cpp` | modificar (CTests de F1,F4,F5) | Tests |
| `snes/runtime/tests/*` o `common/tests/*` | nuevo (CTest de F3 config-driven) | Tests |

## Fuera de scope (anotado para specs futuras)

- Revisión línea por línea del **emisor SNES** (variantes M/X, 1525 líneas),
  **PPU** (NES/SNES) y **APU/SPC700** — no cubiertas a fondo en la revisión.
- Convertir el diagnóstico a "diagnóstico real basado en señales" (opción
  descartada en ADR-001); quedaría como feature futura si se desea.
