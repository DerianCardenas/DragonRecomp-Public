# Plan 005: Implementación de la Nueva Interfaz (Mockup Obsidian) y Preferencias

> **Spec Asociada**: [Spec 005: Nueva Interfaz de DragonRecomp Studio y Sistema de Preferencias](spec.md)

---

## 1. Fases de Implementación

### Fase 1: Sistema de Preferencias (`StudioSettings`) y Niveles de Optimización en `Pipeline`
- **Archivos**:
  - `studio/include/studio/settings.h` (nuevo)
  - `studio/src/settings.cpp` (nuevo)
  - `studio/include/studio/pipeline.h`
  - `studio/src/pipeline.cpp`
  - `studio/CMakeLists.txt`
- **Detalles**:
  1. Crear `StudioSettings` con carga/guardado atómico en `settings.toml` (`max_threads`, `opt_level`, `export_directory`, `dark_mode`, `accent_color`, `auto_save_state`, `default_crt_filter`) y método `reset_to_defaults()`.
  2. Ampliar `CompileOptLevel` en `Pipeline` para soportar los 4 niveles del mockup: `-O0` (`O0`), `-O1` (`O1`), `-O2` (`O2`), `-O3` (`O3`).
  3. Añadir un buffer de log cronológico (`pipeline_.compile_log()`) en `Pipeline` que registre cada paso con marca de tiempo `MM:SS` y nivel (`Info`, `Message`, `Success`, `Warning`) para alimentar el panel `Log de compilación` del mockup.

### Fase 2: Soporte de `--autosave` en Runtimes (`nes`, `snes`, `gb`, `gba`)
- **Archivos**:
  - `nes/runtime/include/nes/frontend.h`, `nes/runtime/src/frontend.cpp`
  - `snes/runtime/include/snes/frontend.h`, `snes/runtime/src/frontend.cpp`
  - `gb/runtime/include/gb/frontend.h`, `gb/runtime/src/frontend.cpp`
  - `gba/runtime/include/gba/frontend.h`, `gba/runtime/src/frontend.cpp`
- **Detalles**:
  1. Soportar el flag de línea de comandos `--autosave` y la variable de entorno `DRAGON_AUTOSAVE_STATE=1` en `parse_runner_args` / `run_*_app`.
  2. Al salir limpiamente del bucle principal del juego (`running = false` por cierre de ventana o tecla `Escape`), si `autosave` está habilitado, guardar automáticamente el estado de la partida en el slot 0 (`save_state_to_file`).

### Fase 3: Motor de Temas Dinámico (Modo Oscuro/Claro + 4 Colores de Acento)
- **Archivos**:
  - `studio/include/studio/theme.h`
  - `studio/src/theme.cpp`
- **Detalles**:
  1. Actualizar `ThemeColors` para que sus colores sean dinámicos en lugar de `constexpr` fijos, inicializados con los valores exactos del mockup (`#0C0E14`, `#161922`, `#1E2230`, `#252A3A`, `#F07C1E`, `#1EC8D4`, `#2ECC71`, `#E74C3C`).
  2. Implementar `apply_studio_theme(bool dark_mode, AccentColor accent, float dpi_scale)` que recalcula los tokens de `ThemeColors` y actualiza `ImGui::GetStyle().Colors` en vivo.

### Fase 4: Nuevo Shell UI (Sidebar `56px` + Topbar `52px` + 3 Vistas)
- **Archivos**:
  - `studio/include/studio/app.h`
  - `studio/src/app.cpp`
  - `studio/src/library_view.cpp`
- **Detalles**:
  1. **Shell Principal**:
     - `render_sidebar()`: Barra vertical izquierda de `56px` con logo `DR`, iconos vectoriales dibujados con `ImDrawList` para `Biblioteca`, `Recompilador` y `Preferencias`, e indicador inferior `v1.0.0`.
     - `render_topbar()`: Barra superior de `52px` con título `DragonRecomp Studio`, píldora de estado `● <N> núcleos · -O<X>` y botones de importación.
  2. **Vista Biblioteca + Inspector (`library_view.cpp`)**:
     - Pestañas de consola con contador, cuadrícula de tarjetas `3:4` con punto verde `ready-dot`, abreviatura o carátula, tiempo jugado y selección activa.
     - Panel derecho **Inspector (`280px`)** con metadatos del juego seleccionado y los 3 botones aprobados: **`▶ Jugar ahora`**, **`Abrir carpeta`** y **`Eliminar`** (sin botón `"Recompilar"`, y `"Eliminar"` solo quita de `library.toml`).
  3. **Vista Recompilador + Sidebar de Log (`app.cpp`)**:
     - Drop zone con borde punteado, chips `.nes` `.sfc` `.smc` `.gb` `.gbc` `.gba` y botón cyan `Seleccionar archivo` (sin sección `"ROMs de prueba"`).
     - Tarjeta de resultado post-compilación con métricas, barra de progreso gradiente, toggles y botones `▶ Jugar (60 FPS)`, `Exportar portable` y `Guardar en biblioteca`.
     - Panel derecho (`260px`) con `Log de compilación` en vivo y `Acciones de recuperación`.
  4. **Vista Preferencias (`app.cpp`)**:
     - Secciones `COMPILACIÓN`, `RUTAS` (solo `Directorio de exportación`, sin `Directorio de biblioteca`), `APARIENCIA` (`Modo oscuro` y 4 `Colores de acento`) y `JUEGO` (`Autoguardado de estado` y `Filtro CRT por defecto`), más botones `Guardar cambios` y `Restablecer todo`.

### Fase 5: Pruebas Unitarias y Verificación
- **Archivos**:
  - `studio/tests/library_tests.cpp`
  - `studio/tests/studio_unit_tests.cpp`
- **Detalles**:
  1. Añadir tests unitarios para `StudioSettings` (carga, guardado, reset, niveles de optimización `-O0..-O3`).
  2. Verificar que las 31 suites de `ctest` compilen sin warnings y pasen al 100%.
