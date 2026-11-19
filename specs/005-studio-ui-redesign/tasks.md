# Tasks 005: Lista de Tareas — Nueva Interfaz y Preferencias de Studio

> **Spec Asociada**: [Spec 005: Nueva Interfaz de DragonRecomp Studio y Sistema de Preferencias](spec.md)  
> **Estado**: COMPLETADA (31/31 tests en verde)

---

## Fase 1: Preferencias Persistentes (`StudioSettings`) y `Pipeline`

- [x] **1.1 Crear `StudioSettings` (`studio/include/studio/settings.h`, `studio/src/settings.cpp`)**
  - [x] Definir `enum class AccentColor { Orange, Cyan, Purple, Green }`.
  - [x] Definir `struct StudioSettings` (`max_compile_threads`, `opt_level`, `export_directory`, `dark_mode`, `accent_color`, `auto_save_state`, `default_crt_filter`).
  - [x] Implementar `defaults()`, `load()` y `save()` sobre `settings.toml`.
- [x] **1.2 Extender `Pipeline` (`studio/include/studio/pipeline.h`, `studio/src/pipeline.cpp`)**
  - [x] Soportar los 4 niveles de optimización (`-O0`, `-O1`, `-O2`, `-O3`) en `CompileOptLevel`.
  - [x] Añadir registro de líneas de log con timestamp `MM:SS` y nivel (`Info`, `Message`, `Success`, `Warning`) para el panel `Log de compilación`.
  - [x] Soportar parámetro `bool auto_save_state` en `Pipeline::launch_game` pasando `--save-state <dir>/autosave.state`.

---

## Fase 2: Autoguardado de Estado en Runtimes (`nes`, `snes`, `gb`, `gba`)

- [x] **2.1 Integración de autoguardado con `--save-state`**
  - [x] Verificado que los 4 runtimes (`nes`, `snes`, `gb`, `gba`) ya soportan `--save-state <path>` guardando automáticamente al salir limpiamente; tanto `Pipeline::launch_game` como `StudioApp::launch_portable_game` pasan `--save-state <work_dir>/autosave.state` cuando `auto_save_state` está activo.

---

## Fase 3: Motor de Temas Dinámico (`theme.h`, `theme.cpp`)

- [x] **3.1 Paleta Obsidian, Modo Claro y 4 Colores de Acento**
  - [x] Convertir `ThemeColors` a tokens dinámicos actualizables en tiempo de ejecución.
  - [x] Implementar modo oscuro (`#0C0E14`, `#161922`, `#1E2230`, `#252A3A`) y modo claro (`#F3F4F8`, `#FFFFFF`, `#E9ECF2`).
  - [x] Implementar cambio dinámico de `Primary` y `PrimaryDim` entre Naranja (`#F07C1E`), Cyan (`#1EC8D4`), Púrpura (`#9B59B6`) y Verde (`#2ECC71`).

---

## Fase 4: Implementación de la Nueva UI (Mockup HTML + Ajustes del Usuario)

- [x] **4.1 Shell Global (`studio/src/studio_views.cpp`, `studio/src/app.cpp`)**
  - [x] Implementar `render_sidebar()` (`56px` ancho) con logo `DR`, navegación (`Biblioteca`, `Recompilador`, `Preferencias`) e icono `v1.0.0`.
  - [x] Implementar `render_topbar()` (`52px` alto) con título `DragonRecomp Studio`, píldora `● <N> núcleos · <opt>` y botón principal.
- [x] **4.2 Vista Biblioteca e Inspector (`studio/src/library_view.cpp`)**
  - [x] Pestañas de consola (`Nintendo NES`, `Super Nintendo`, `Game Boy / Color`, `Game Boy Advance`) con contadores y estilo activo `--primary`.
  - [x] Grid de tarjetas `3:4` con `ready-dot`, carátula o abreviatura, tiempo jugado y estado seleccionado.
  - [x] Panel lateral derecho **`Inspector` (`280px`)**:
    - [x] Mostrar `Plataforma`, `Tiempo jugado`, `Modo`, `Paquete` y `Estado`.
    - [x] Botón **`▶ Jugar ahora`** (lanza el ejecutable portable del SO anfitrión).
    - [x] **Omitido botón `"Recompilar"`** (no visible ni en Inspector ni en menú contextual).
    - [x] Botón **`Abrir carpeta`** (abre `package_dir`).
    - [x] Botón **`Eliminar`** (elimina solo de `library.toml`, sin borrar archivos del disco).
- [x] **4.3 Vista Recompilador (`studio/src/studio_views.cpp`)**
  - [x] Drop zone con borde punteado, chips de formatos (`.nes`, `.sfc`, `.smc`, `.gb`, `.gbc`, `.gba`) y botón cyan `Seleccionar archivo`.
  - [x] **Eliminada por completo la sección `"ROMs de prueba"`**.
  - [x] Tarjeta `RecompResultCard` con métricas, barra de progreso gradiente, checkboxes (`Pantalla completa`, `Filtro CRT`) y botones `▶ Jugar (60 FPS)`, `Exportar portable` y `Guardar en biblioteca`.
  - [x] Sidebar derecha (`276px`) con `Log de compilación` y `Acciones de recuperación`.
- [x] **4.4 Vista Preferencias (`studio/src/studio_views.cpp`)**
  - [x] Sección `COMPILACIÓN`: Slider `Núcleos máximos` (`1..16`) y combo `Nivel de optimización` (`-O0`, `-O1`, `-O2`, `-O3`).
  - [x] Sección `RUTAS`: `Directorio de exportación` con botón `Cambiar` (usado como carpeta por defecto para exportar). **Omitido `"Directorio de biblioteca"`**.
  - [x] Sección `APARIENCIA`: Toggle `Modo oscuro` y selector circular de 4 `Colores de acento` con vista previa en vivo.
  - [x] Sección `JUEGO`: Toggles `Autoguardado de estado` y `Filtro CRT por defecto`.
  - [x] Botones `Guardar cambios` y `Restablecer todo`.

---

## Fase 5: Pruebas, Responsividad Multi-Resolución y Validación

- [x] **5.1 Tests Unitarios (`studio/tests/studio_unit_tests.cpp`)**
  - [x] Añadido `test_studio_settings()` y pruebas de `CompileOptLevel` (`-O0..-O3`) y `Pipeline::compile_log()`.
- [x] **5.2 Adaptación Multi-Resolución / HiDPI (`studio_views.cpp`, `library_view.cpp`, `app.cpp`)**
  - [x] Tarjetas de Preferencias (`COMPILACION`, `RUTAS`, `APARIENCIA`, `JUEGO`), métricas del Recompilador y `PathDisplayBox` convertidas a `ImGuiChildFlags_AutoResizeY` (cero recortes verticales en escalas 1.25x/1.5x).
  - [x] Alineación derecha dinámica mediante `GetContentRegionAvail()` / `CalcTextSize()`, anchos proporcionales de paneles laterales (`LibraryInspector`, `RecompSide`), salto de línea automático en pestañas de consolas y alturas dinámicas en `TopBar` y pies de carátula.
- [x] **5.3 Compilación y `ctest`**
  - [x] Compilación limpia sin warnings y 31/31 tests en verde en Linux y Windows.
