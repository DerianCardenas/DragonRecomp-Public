# Spec 002: DragonRecomp Studio UI & GBA/GB Stability Package

> Estado: **PLANIFICADA**. Esta spec consolida la resolución de los problemas identificados en la auditoría del sistema: layout responsivo de la interfaz en Studio, autoaprendizaje de fallbacks robusto para GBA y Game Boy, soporte de Windows sin falsos positivos de error, completitud de Save States v2 en GBA y detección inteligente de Clang.

---

## 1. Contexto y Objetivos

Durante la auditoría de DragonRecomp Studio y los runtimes se detectaron varias inconsistencias que impiden una experiencia óptima de usuario y autoaprendizaje confiable:

1. **Interfaz de usuario con elementos amontonados**:
   - En el Asistente de Diagnóstico (`studio/src/app.cpp`), el Combo selector de síntomas (`380px`) y el botón `"🛠 Intentar Reparar"` (`240px`) suman `635px`, excediendo el ancho de la tarjeta derecha (`col_w ~ 500-580px`) y provocando desbordes y solapamientos.
   - Casillas de pantalla completa y filtro CRT con `SameLine(0, 20px)` que se desbordan en resoluciones estándar.
   - Mensajes de rutinas aprendidas y estados de exportación que rompen la alineación de botones.
   - Header y DropZone desactualizados (solo mencionaban NES y SNES, omitiendo Game Boy y GBA).
   - Bloque de controles sin soporte visual para GBA (falta de gatillos L/R y asignación específica).

2. **Autoaprendizaje de fallbacks en GBA y Game Boy**:
   - `gba/runtime/src/bus.cpp`: Ausencia de `std::fflush(stderr)` en `Bus::log_fallback()`, bloqueando la captura en tiempo real cuando se ejecuta mediante tuberías (`CreatePipe`/`pipe`).
   - `studio/src/pipeline.cpp`: Expresiones regulares con `\n` estricto que fallaban ante saltos de línea Windows CRLF (`\r\n`), impidiendo sanear o purgar puntos aprendidos.
   - Falso positivo de error en Windows en `monitor_game_process()`: cualquier línea de stderr (incluso un fallback informativo) marcaba el juego como fallido al cerrarse.
   - Falta de validación del rango de dirección aprendida contra el tamaño real de la ROM GBA (`rom_size_kb`).
   - Deduplicación ciega `{bank, addr}` ignorando el estado ARM vs Thumb.
   - Ausencia total de logging de fallbacks en el runtime de Game Boy (`gb/runtime`).

3. **Incompletitud crítica de Save States (`DRST`) en GBA**:
   - `gba/runtime/src/bus.cpp`: Solo guardaba CPU, RAM y VRAM/OAM. Omitía por completo los 1024 bytes de registros I/O (`io_regs_`), los 4 Timers de hardware, los 4 canales de DMA y las banderas de interrupción (`IME`, `IE`, `IF`). Al cargar partida (F7), el desfase de hardware provocaba cuelgues o congelamientos.

4. **Localización de Clang en Windows**:
   - Invocación hardcodeada de `"clang"` y `"clang++"` sin buscar en rutas típicas de Windows (`C:\Program Files\LLVM\bin`, MinGW, etc.), generando fallos opacos si Clang no está en el PATH del sistema.

---

## 2. Decisiones de Arquitectura (ADRs)

* **ADR-004**: Incrementar versión de Save State en GBA a `version = 2` serializando `GbaStatePayload` completo (I/O, timers, DMAs, interrupciones y PPU), con soporte de carga hacia atrás para estados `version = 1`.
* **ADR-005**: Adopción de diseño proporcional responsivo en la interfaz de Studio: Combo de diagnóstico al 60% y botón al 40% dinámico sobre `ImGui::GetContentRegionAvail().x`, y alineación segura de controles.
* **ADR-006**: Logging unificado con `fflush` y soporte de autoaprendizaje en Game Boy (GB/GBC) idéntico a NES/GBA.

---

## 3. Especificación Técnica por Componente

### 3.1 Studio UI (`studio/src/app.cpp`)
- **Diagnóstico**:
  ```cpp
  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float gap = 12.0f;
  const float combo_w = (avail_w - gap) * 0.60f;
  const float btn_w = (avail_w - gap) * 0.40f;
  ImGui::SetNextItemWidth(combo_w);
  ImGui::Combo("##SymptomCombo", ...);
  ImGui::SameLine(0, gap);
  ImGui::Button("🛠  Intentar Reparar", ImVec2(btn_w, 32.0f));
  ```
- **Acciones y Opciones**:
  - Casillas `--fullscreen` y `--crt`: Envoltura o colocación limpia sin desborde.
  - Botón "⚡ Optimizar y Recompilar Todo": Colocado debajo del texto explicativo o con cálculo de ancho para evitar solapamientos.
  - Avisos de estado con botón de cierre `✕` en fila dedicada.
- **Header y DropZone**:
  - Subtítulo actualizado: `"NES (6502) + SNES (65816) + GB/GBC (SM83) + GBA (ARM7TDMI) → Linux + Windows nativos (SDL3)"`.
  - Indicadores de motor: Añadir GB Engine y GBA Engine al status pill.
  - DropZone: Añadir extensiones `.gb`, `.gbc`, `.gba` y agregar accesos rápidos de prueba representativos.
- **Controles**:
  - Rama explícita para `RomPlatform::GameBoyAdvance`:
    - D-Pad: Flechas / WASD
    - Botón A: X / J
    - Botón B: Z / K
    - Gatillo L: Q
    - Gatillo R: E
    - Select / Start: RShift o Tab / Enter

### 3.2 Autoaprendizaje y Subprocesos (`studio/src/pipeline.cpp`)
- **Tolerancia CRLF**: Ajustar regex `kLearned` a `\\[\\[entry_points\\]\\]\\r?\\naddr = ...`.
- **Filtro de Stderr en Windows**: Solo registrar `last_game_error_` si el código de salida del proceso es distinto de 0 o si `captured_err` contiene errores reales no relacionados con líneas `FALLBACK`.
- **Deduplicación GBA**: Comparar tupla `{bank, addr, thumb}` para permitir el mismo destino en distinto ISA si correspondiera.
- **Límite de ROM GBA**: Verificar que `ev.addr - 0x08000000U < current_rom.file_size` (o tamaño en bytes de la ROM).
- **Auto-reparación GBA**: Al reportar `FreezeOnBoot` o `CrashOnLaunch` en GBA, purgar rutinas aprendidas sospechosas y realizar recompilación limpia.
- **Búsqueda inteligente de Clang**: Función `find_c_compiler()` que verifique PATH y `C:\Program Files\LLVM\bin\clang.exe`, ofreciendo instrucciones claras si no está instalado.

### 3.3 Runtime GBA (`gba/runtime/src/bus.cpp`)
- Añadir `std::fflush(stderr)` en `Bus::log_fallback()`.
- Crear `GbaStatePayload`:
  - `CpuState cpu`
  - `uint8_t io_regs[1024]`
  - `Timer timers[4]`
  - `DmaChannel dma[4]`
  - `uint8_t ime`, `uint16_t ie`, `uint16_t if_`
  - `uint16_t ppu_vcount`, etc.
  - Memoria RAM (EWRAM, IWRAM, SRAM, EEPROM, Palettes, VRAM, OAM).
- `save_state`: Escribir `SaveStateHeader` con `version = 2` y volcar `GbaStatePayload`.
- `load_state`: Si `version == 2`, leer payload completo y sincronizar hardware; si `version == 1`, leer payload legacy sin fallar.

### 3.4 Runtime Game Boy (`gb/runtime/src/bus.cpp` & `runtime.cpp`)
- En `Bus`: Añadir `logged_fallbacks_` (`std::set<std::pair<uint16_t, uint8_t>>`) y método `log_fallback(uint16_t addr, uint8_t bank, uint16_t from_pc)`.
- En `gb_runtime_dispatch`: Antes de entrar al intérprete paso a paso por no encontrar función compilada, llamar a `bus->log_fallback(cpu->pc, bus->current_rom_bank(), from_pc)`.
- Formato emitido: `FALLBACK addr=$%04X bank=$%02X from=$%04X\n` con `std::fflush(stderr)`.

---

## 4. Criterios de Aceptación
1. Compilación limpia y sin advertencias en Clang/MSVC (`-Wall -Wextra`).
2. CTest suite (tests unitarios existentes + nuevos tests de SaveState GBA v2 y Studio UI) pasando al 100%.
3. En la interfaz de Studio, ningún botón, combo o texto se monta o desborda en ventanas de 1280x720 o superiores.
4. Autoaprendizaje verificado: las rutinas descubiertas durante la ejecución en GBA y GB se registran en caliente, se agregan al TOML sin corrupción de CRLF y se compilan nativamente.
5. Save states en GBA (F5/F7) restauran perfectamente el estado de pantalla, temporizadores e interrupciones sin congelar el juego.
