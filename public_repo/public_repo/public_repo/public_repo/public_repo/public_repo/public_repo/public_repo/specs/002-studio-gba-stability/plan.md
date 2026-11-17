# Plan: Spec 002 — Studio UI & GBA/GB Stability Package

## Fases de Implementación

### Fase 1: Runtime GBA & GB (Núcleo de bajo nivel)
1. **GBA Save States v2**:
   - Definir `GbaStatePayload` en `gba/runtime/include/gba/bus.h` o cabecera interna.
   - Implementar serialización completa en `gba/runtime/src/bus.cpp` (`save_state`).
   - Implementar deserialización en `load_state` con detección de versión 1 y versión 2.
   - Añadir tests unitarios en `gba/runtime/tests/gba_runtime_tests.cpp` para validar guardado y recuperación íntegra de I/O registers, timers y DMAs.
2. **GBA Fallback Flush**:
   - Añadir `std::fflush(stderr)` tras cada invocación de `Bus::log_fallback()`.
3. **GB Fallback Logging**:
   - Implementar `Bus::log_fallback` en `gb/runtime/src/bus.cpp` con `std::fflush(stderr)`.
   - Invocar `log_fallback` en `gb/runtime/src/runtime.cpp` dentro de `gb_runtime_dispatch` cuando `lookup_function` retorne nulo.
   - Añadir test unitario en `gb/runtime/tests/gb_runtime_tests.cpp`.

### Fase 2: Pipeline de Auto-aprendizaje & Subprocesos (`studio/src/pipeline.cpp`)
1. **Tolerancia CRLF y Regex**:
   - Modificar las expresiones regulares de saneamiento y detección para aceptar `\r?\n`.
2. **Filtro de Errores Windows**:
   - Corregir `monitor_game_process` para que no asigne `last_game_error_` cuando la salida sean fallbacks normales o el proceso termine exitosamente.
3. **Validación de Límites y Deduplicación**:
   - Validar rango contra el tamaño real de la ROM en GBA.
   - Incluir bandera `thumb` en el set de deduplicación de puntos de entrada.
4. **Auto-reparación Inteligente**:
   - En `diagnose_and_repair`, purgar funciones aprendidas y limpiar caché en síntomas de congelamiento/crash de GBA.
5. **Localización de Clang**:
   - Implementar búsqueda inteligente del ejecutable de Clang en Windows.

### Fase 3: Interfaz Gráfica (`studio/src/app.cpp`)
1. **Layout Responsivo y Anti-Desborde**:
   - Aplicar anchos proporcionales (60% / 40%) en la tarjeta de diagnóstico.
   - Reorganizar casillas de pantalla completa y filtro CRT para evitar colisiones.
   - Colocar el botón "⚡ Optimizar y Recompilar Todo" de forma segura.
   - Separar avisos de estado y botones de cierre.
2. **Soporte Completo de Plataformas**:
   - Actualizar Header y status pills con Game Boy y GBA.
   - Actualizar DropZone con extensiones y juegos de muestra.
   - Añadir pestaña y controles específicos para GBA (L/R, A/B).

### Fase 4: Verificación y Documentación
1. Ejecutar suites completas de CTest.
2. Validar que no existan regresiones de compilación ni advertencias.
3. Actualizar `docs/MILESTONES.md` y generar walkthrough.
