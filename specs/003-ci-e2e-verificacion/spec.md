# Spec 003: Integración Continua y Verificación End-to-End

> Estado: **EN PROGRESO** (implementación inicial 2026-11-15, ver
> `tasks.md` para el detalle de qué está hecho/verificado vs. bloqueado).
> Esta spec define la infraestructura de verificación
> del proyecto: un contrato de pruebas End-to-End (E2E) en tres niveles, una
> topología de CI en dos niveles (hosted sin ROMs comerciales + local/self-hosted
> con ROMs), resolución de ROMs por variable de entorno, verificación lock-step
> (`--verify`) para Game Boy y GBA, y honestidad en los milestones y en el
> "auto-diagnóstico" de Studio.

---

## 1. Contexto y Objetivos

La auditoría de 2026-11-14 (abogado del diablo) detectó que, pese a que los 24
milestones figuran `done`, la verificación real es frágil:

1. **No existe CI.** No hay `.github/workflows/`. "Build limpio y 29 tests en
   verde" es cierto solo en la máquina del autor, con sus ROMs. Gemini también
   commitea a `main` sin ningún gate automático (ver memoria del proyecto).
2. **Los E2E que sostienen la tesis se auto-saltan.** `smb_boot_test`,
   `dk_boot_test`, `smb_snes_boot_test`, `*_recomp_test` hacen `return 0` cuando
   falta la ROM (`roms/` está gitignored). En un clon limpio, "SMB arranca",
   "DK arranca", "SMW corre" **no se validan nunca**, y un test que se salta es
   indistinguible de uno que pasa (CTest los cuenta como verdes).
3. **Aserciones E2E demasiado débiles.** `dk_boot_test.cpp` solo exige
   `distinct_colors >= 5` y `non_backdrop_pixels > 4000`: prueba "pintó algo",
   no "pintó la pantalla correcta". Una pantalla corrupta también los cumple.
4. **GB y GBA sin red de seguridad de corrección.** NES y SNES tienen
   `run_verify_mode` (lock-step recompilado vs intérprete); **GB y GBA no tienen
   `--verify` en absoluto**, y son las arquitecturas más nuevas y frágiles
   (ráfaga de `fix(gba)` en el historial: "function shattering", "fragmented
   entry points", "CPU spinning"). GB tampoco tiene boot tests E2E.
5. **El `--verify` existente es superficial.** Compara solo RAM y framebuffer;
   no cubre audio, registros PPU/IO ni timing. Un juego puede "matchear
   framebuffer" y divergir en audio o timing.
6. **Milestones sobrevenden.** `MILESTONES.md` marca `done` plano sin distinguir
   "recompila nativo con 0 fallbacks" de "corre vía intérprete" (SMW, SMB2(J),
   Emerald caen al intérprete). No hay reporte por juego del modo real.
7. **"Auto-diagnóstico" de Studio** ya se hizo honesto (ADR-001) pero conserva
   el framing de "reparación de hardware" que no refleja lo que hace.

**Objetivo:** convertir la verificación de "confío en mi máquina" a un contrato
reproducible, legal (sin ROMs comerciales en git) y honesto, sin frenar el ritmo
de desarrollo.

### 1.1 Inventario de ROMs disponibles (biblioteca local del autor)

Biblioteca en `/home/derian/JUEGOS/EMULADORES/ROMS/` (fuera del repo, nunca se
commitea ni se modifica — HARD RULE). Subcarpetas relevantes y juegos
representativos confirmados:

| Plataforma | Carpeta | Juegos representativos presentes |
|------------|---------|----------------------------------|
| NES  | `NES/`  | `Super Mario Bros. (Europe).nes`, `Donkey Kong (JU).nes`, `Battle City (VS).nes`, `Super Mario Bros. 2 (J) [hM04].nes`, Adventure Island, Bomberman, Castlevania |
| SNES | `SNES/` | **solo** `Super Mario Bros. SNES (NTSC).zip` (requiere descompresión; SMW **no está** en la biblioteca → su config existe pero sin ROM) |
| GB   | `GB/`   | `Pokemon - Blue NA.gb`, `Legend of Zelda, The - Link's Awakening.gb`, Kirby's Dream Land, Contra, Final Fantasy Legend |
| GBC  | `GBC/`  | `Pokemon Amarillo (ESP).gbc`, `Pokemon Oro (ESP).gbc` |
| GBA  | `GBA/`  | `Metroid - Zero Mission (USA).gba`, `Metroid Fusion (USA).gba`, `Pokemon - Edicion Esmeralda (Spain).gba`, Ruby, LeafGreen, Sapphire |

Gaps a registrar: **SNES** solo cubre SMB SNES (y comprimido); **SMW no tiene
ROM** en la biblioteca, por lo que su E2E de Nivel 2 queda `blocked` hasta
conseguir la ROM. Los nombres reales difieren de los `rom_path` de los configs
(`smb.nes`, `dk.nes`), así que la resolución de ROMs (ADR-007) debe mapear por
SHA-1, no por nombre fijo.

---

## 2. Decisiones de Arquitectura (ADRs)

* **ADR-006**: Topología de CI en dos niveles — *hosted* (GitHub Actions,
  público, sin ROMs comerciales) para build limpio + unit tests + E2E de Nivel 0
  y 1; *local/self-hosted* (con la biblioteca de ROMs) para E2E de Nivel 2.
* **ADR-007**: Resolución de ROMs vía `DRAGON_ROM_DIR` por SHA-1 y principio
  "skip ≠ pass" (`SKIP_RETURN_CODE 77` + `DRAGON_REQUIRE_ROMS=1` convierte el
  skip en fallo en el nivel local).
* **ADR-008**: Contrato E2E de tres niveles con *golden-hash* de framebuffer y
  `--verify` lock-step como prueba de equivalencia; extensión de `--verify` a
  GB y GBA y a audio + registros PPU/IO.

---

## 3. Especificación Técnica por Componente

### 3.1 Contrato E2E de tres niveles (ADR-008)

El término "E2E" hoy mezcla tres garantías distintas. Se separan:

**Nivel 0 — ROM sintética (CI hosted, siempre).**
Micro-ROMs construidas en memoria (patrón ya existente en
`nes/recompiler/tests/recomp_diff_tests.cpp` y `mapper_unit_tests.cpp`) que
ejercitan construcciones concretas (cada mapper/MBC, interworking ARM/Thumb,
jump tables, banking) por el pipeline completo
`disassemble → emit C99 → compilar con -Werror → cargar → ejecutar`.
*Garantía:* el recompilador no se rompe ante construcciones conocidas, sin
depender de ninguna ROM comercial.

**Nivel 1 — ROM pública de precisión (CI hosted, siempre).**
Test ROMs redistribuibles committeadas en `roms/test/` (como ya se hace con
`nestest.nes`/`nestest.log`): blargg (`cpu_instrs`, `instr_timing`, `apu_test`,
`dmg_sound`, `cgb_sound`), mooneye-gb, `gba-suite` (jsmolka), `gbmicrotest`.
Cada test valida la **firma de salida** conocida (texto/serial/puntero de
resultado) de la ROM. *Garantía:* precisión de hardware medida y versionada en
CI; convierte "G1: blargg pasa" en prueba y no en promesa local.

**Nivel 2 — Juego comercial (local / self-hosted, ROM-gated).**
Por cada juego objetivo, con la ROM resuelta por `DRAGON_ROM_DIR` (ADR-007):
1. **Recompilar con 0 fallbacks** (o presupuesto declarado explícito en el
   contrato del juego). Si el juego es `interpreter_only`, el contrato lo declara
   y el criterio pasa a "arranca en intérprete sin cuelgue".
2. **Ejecutar M frames con input scripteado** — determinista, sin reloj de pared,
   sin device de audio real (buffer en memoria).
3. **Golden-hash de framebuffer**: en frames-checkpoint, `hash(framebuffer)` debe
   igualar el *golden* committeado. Se committea el **hash** (CRC32/SHA-1), nunca
   un PNG (el hash es un número derivado y no reproduce la obra; el PNG sí la
   reproduce).
4. **Equivalencia lock-step**: correr `--verify` recompilado-vs-intérprete los
   primeros N frames → prueba *equivalencia*, no solo "una pantalla conocida".

*Garantía:* el juego real recompilado produce exactamente la salida de
referencia y es equivalente al intérprete en los primeros N frames.

Contrato por juego (checkpoints e input) a definir en implementación; candidatos
representativos por plataforma, ya presentes en la biblioteca:

| Plataforma | Juego objetivo | Config existente | Notas |
|------------|----------------|------------------|-------|
| NES  | Super Mario Bros., Donkey Kong | `nes/games/smb`, `nes/games/dk` | ya tienen boot tests; subir aserciones a golden-hash |
| NES  | Battle City (VS) | — | Mapper 99 / Vs. System (ver memoria), config nuevo |
| SNES | SMB SNES | `snes/games/smb_snes` | ROM comprimida en biblioteca; definir descompresión |
| SNES | SMW | `snes/games/smw` | **blocked**: sin ROM en biblioteca; corre en intérprete |
| GB   | Zelda: Link's Awakening, Pokémon Blue | — | configs nuevos; primera cobertura E2E de GB |
| GBC  | Pokémon Oro | — | MBC3 + RTC + doble velocidad CGB |
| GBA  | Metroid Zero Mission | — | config nuevo; recompilación nativa |
| GBA  | Pokémon Emerald | — | `interpreter_only` (ver memoria); contrato "arranca en intérprete" |

### 3.2 Resolución de ROMs y "skip ≠ pass" (ADR-007)

- **Variable de entorno `DRAGON_ROM_DIR`**: raíz de la biblioteca de ROMs. Por
  defecto `roms/` (comportamiento actual intacto). Los tests y los configs
  resuelven la ROM buscando por **SHA-1** dentro de `DRAGON_ROM_DIR` (recursivo),
  no por nombre de archivo (los nombres de la biblioteca difieren de `smb.nes`).
  - CMake: pasar `DRAGON_ROM_DIR` a los tests vía `ENVIRONMENT` o definición;
    los `add_test` con `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` siguen válidos.
  - Helper compartido en `common/` (`find_rom_by_sha1(dir, sha1)`), usado por
    boot tests, recomp tests y por Studio.
- **Código de skip real**: los tests que no encuentran la ROM devuelven `77`; en
  CMake, `set_tests_properties(... PROPERTIES SKIP_RETURN_CODE 77)`. CTest los
  reporta como *Skipped*, no como *Passed*.
- **`DRAGON_REQUIRE_ROMS=1`**: cuando está activa (nivel local/self-hosted), un
  ROM ausente deja de ser skip y pasa a **fallo** (`return 1`). Así el nivel
  hosted muestra "N passed, M skipped" (honesto) y el nivel local falla si falta
  una ROM que debería estar.
- **Nunca** se committean ROMs comerciales ni capturas de pantalla; solo hashes
  golden (derivados) y las ROMs públicas redistribuibles del Nivel 1.

### 3.3 Topología de CI (ADR-006)

**Nivel A — GitHub Actions (hosted, por cada push/PR):**
- `.github/workflows/ci.yml`:
  - Matriz Linux `{clang, gcc}`: instalar `ninja-build`, `libsdl3-dev`;
    `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build`;
    `ctest --test-dir build --output-on-failure`.
  - Job Windows (`windows-latest`, MSVC): al menos *configure + build* —hoy
    **nada** verifica la compilación Windows que M6 afirma soportar— y, si es
    viable, los unit tests.
  - Enforcar `-Wall -Wextra -Werror` (ya lo usa el pipeline de compilación C de
    los boot tests) para mantener el build *warning-free* que exige AGENTS.md.
  - Cachear dependencias.
- Cubre Nivel 0 y Nivel 1 (sin ROMs comerciales), build limpio en entorno
  aislado, y protege contra regresiones de cualquier committer (incl. Gemini).

**Nivel B — local / self-hosted (manual o nocturno):**
- `.github/workflows/e2e-roms.yml` con `workflow_dispatch` (+ opcional
  `schedule` nocturno) que corre en un runner self-hosted con `DRAGON_ROM_DIR`
  apuntando a la biblioteca y `DRAGON_REQUIRE_ROMS=1`.
- Alternativa sin runner: un git hook `pre-push` (script en `scripts/`) que
  ejecute la suite ROM-gated antes de subir. Documentado como opcional y
  bypasseable (`--no-verify`), por lo que **no sustituye** al Nivel A.

### 3.4 Verificación lock-step para GB y GBA + extensión (ADR-008)

- **Portar `run_verify_mode` a GB y GBA** replicando NES/SNES
  (`*/runtime/src/frontend.cpp`): opción `--verify`, ejecución paralela
  recompilado-vs-intérprete, comparación por frame, fallo ante divergencia o
  fallback.
- **Extender el contenido verificado** (todas las plataformas) más allá de RAM +
  framebuffer:
  - Hash del **ring buffer de audio** (APU/DirectSound) acumulado por frame.
  - Subconjunto de **registros PPU/IO** clave (scroll, control, status, paletas).
  - Ejecución de **N frames con input scripteado**, no solo boot.
- Si no se extiende, **no** anunciar `--verify` como prueba de equivalencia:
  degradar su descripción en `--help` y docs a "humo-test de framebuffer".

### 3.5 Honestidad de milestones y reporte por juego

- El recompilador emite un `report.toml` por juego (en `generated/`): nº de
  funciones emitidas, nº de fallbacks, `interpreter_only` sí/no, tamaño de la
  dispatch table, advertencias de opcodes no oficiales.
- `MILESTONES.md` y Studio muestran el **modo real por juego**:
  `nativo-0-fallbacks` / `híbrido-N-fallbacks` / `intérprete`. Regla de oro: un
  juego cuenta como "recompilado" solo si pasa `--verify ... 0 fallbacks`.

### 3.6 Finalización de la honestidad del "auto-diagnóstico" (cierra ADR-001)

- Renombrar en Studio (`studio/src/app.cpp`) el "Asistente de Diagnóstico /
  Intentar Reparar" a lo que hace de verdad (p. ej. `Reintentar · Alternar
  mirroring · Recompilar`). Quitar cualquier texto residual que afirme un arreglo
  de hardware que no ocurre. El "diagnóstico real por señales" queda como posible
  spec futura, no como promesa de UI.

### 3.7 Deuda técnica dirigida

- **Detección determinista de `interpreter_only`**: heurístico en el recompilador
  que marque juegos con trucos de stack / espera-NMI y lo deje en el `report.toml`
  (en vez de descubrirlo juego a juego).
- **Invalidación de la caché de knowledge de Studio**: versionar la caché e
  invalidarla al recompilar el tool; el `interpreter_only` del config **siempre**
  gana sobre la caché (cierra el gotcha de Pokémon Emerald, ver memoria).
- **Medir los modelos "simplificados"** (ADSR del SPC700 `snes/runtime/src/apu.cpp`,
  timing APU GB `gb/runtime/src/bus.cpp`) con las ROMs de audio del Nivel 1 para
  saber cuándo la aproximación empieza a importar; documentarlos como conscientes
  en los specs de plataforma.

---

## 4. Criterios de Aceptación

1. `.github/workflows/ci.yml` configura, compila (`clang` + `gcc` Linux, MSVC
   Windows configure+build) y corre `ctest` en verde en un clon limpio, sin
   ninguna ROM comercial.
2. Los tests ROM-gated devuelven `77` y CTest los reporta como **Skipped** en
   ausencia de ROM; con `DRAGON_REQUIRE_ROMS=1` y `DRAGON_ROM_DIR` apuntando a la
   biblioteca, esos tests **corren y pasan** (SMB, DK, SMB SNES como mínimo).
3. Nivel 0 y Nivel 1 operativos: al menos una micro-ROM sintética por
   plataforma por el pipeline completo, y al menos una ROM pública de precisión
   por plataforma committeada y verificada (NES nestest ya cuenta).
4. Al menos un E2E de Nivel 2 con **golden-hash** sustituye las aserciones
   débiles (`distinct_colors >= 5`) en NES (SMB o DK).
5. `--verify` disponible y verde para GB y GBA sobre al menos un juego
   recompilado nativo, incluyendo hash de audio y registros PPU/IO; o, si no se
   extiende, su `--help`/docs ya no lo anuncian como prueba de equivalencia.
6. `report.toml` por juego generado y `MILESTONES.md` reflejando el modo real
   (nativo / híbrido / intérprete) por juego.
7. Studio ya no presenta el asistente como "diagnóstico/reparación de hardware";
   la UI refleja las acciones reales.
8. Build *warning-free* (`-Wall -Wextra`) en todos los niveles.
