# Tareas: Spec 002 — Studio UI & GBA/GB Stability Package

- [x] **Tarea 1: GBA Save States v2 y Registro I/O**
  - [x] 1.1 Definir estructura `HardwareStatePayload` y `Ppu::StatePayload` con `io_regs_`, timers, DMAs, interrupciones y PPU.
  - [x] 1.2 Implementar `Bus::save_state` emitiendo cabecera `DRST` con `version = 2`.
  - [x] 1.3 Implementar `Bus::load_state` con soporte para version 2 y fallback para version 1.
  - [x] 1.4 Agregar test unitario para verificar persistencia de registers I/O en save state.

- [x] **Tarea 2: Auto-aprendizaje en Runtimes (GBA y Game Boy)**
  - [x] 2.1 Agregar `std::fflush(stderr)` a `Bus::log_fallback` en `gba/runtime/src/bus.cpp`.
  - [x] 2.2 Implementar `Bus::log_fallback` con `std::fflush(stderr)` en `gb/runtime/src/bus.cpp`.
  - [x] 2.3 Invocar `log_fallback` en `gb/runtime/src/runtime.cpp` dentro de `gb_runtime_dispatch`.
  - [x] 2.4 Agregar test unitario de fallback logging en Game Boy.

- [x] **Tarea 3: Corrección de Pipeline Studio (`pipeline.cpp`)**
  - [x] 3.1 Ajustar regex de saneamiento para soportar saltos de línea Windows (`\r?\n`).
  - [x] 3.2 Corregir `monitor_game_process` para filtrar fallbacks y no reportar falsos errores en Windows.
  - [x] 3.3 Validar direcciones GBA contra el tamaño real de la ROM.
  - [x] 3.4 Agregar `thumb` al set de deduplicación de puntos de entrada en GBA.
  - [x] 3.5 Implementar purga de funciones aprendidas para `FreezeOnBoot` y `CrashOnLaunch` en GBA.
  - [x] 3.6 Implementar búsqueda inteligente de Clang en Windows con instrucciones amigables.

- [x] **Tarea 4: Rediseño de Layout y Soporte de Plataformas en Studio (`app.cpp`)**
  - [x] 4.1 Implementar anchos proporcionales (60% / 40%) en la sección de Diagnóstico.
  - [x] 4.2 Reorganizar casillas de verificación y botón de optimización para evitar solapamientos.
  - [x] 4.3 Actualizar Header con mención a GB/GBC y GBA y cuatro pills de estado.
  - [x] 4.4 Actualizar DropZone con `.gb`, `.gbc`, `.gba` y accesos rápidos de prueba.
  - [x] 4.5 Agregar mapeo y visualización de controles específica para Game Boy Advance.

- [x] **Tarea 5: Verificación y Validación de Código**
  - [x] 5.1 Verificar compatibilidad estricta de sintaxis C++20, headers y tipos sin asignación dinámica en hot paths.
  - [x] 5.2 Formateo conforme a `.clang-format` y convenciones de `AGENTS.md`.
