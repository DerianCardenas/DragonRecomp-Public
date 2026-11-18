# Plan 004: Plan de Implementación de Biblioteca Basada en Portables

> **Objetivo**: Implementar el registro exclusivo de portables en la biblioteca, ejecución directa según el sistema operativo anfitrión y el asistente de importación con diagnóstico/alerta de ROMs ausentes.

---

## 1. Fases de Ejecución

### Fase 1: Extensión del Modelo de Datos y Persistencia (`GameLibrary`)
- **Archivos**: `studio/include/studio/library.h`, `studio/src/library.cpp`, `studio/tests/library_tests.cpp`.
- **Tareas**:
  1. Ampliar `GameEntry` con `package_dir`, `executable_linux`, `executable_windows`.
  2. Implementar `GameLibrary::add_portable(...)` y actualizar `save()` / `load()` para serializar los nuevos campos en `library.toml`.
  3. Desacoplar `add_rom()` para que solo sea invocado explícitamente cuando exista un paquete portable válido o importación manual.
  4. Agregar pruebas unitarias en `studio/tests/library_tests.cpp`.

### Fase 2: Registro en Exportación Portable (`Packager` & `StudioApp`)
- **Archivos**: `studio/src/app.cpp`, `studio/src/packager.cpp`.
- **Tareas**:
  1. Remover la llamada prematura a `library_.add_rom(info)` dentro de `StudioApp::load_rom()`.
  2. En el callback de éxito de `Packager::create_portable_package()`, registrar la entrada portable en `library_` con la ruta de la carpeta generada y los ejecutables detectados (`executable_linux`, `executable_windows`).
  3. Persistir inmediatamente con `library_.save()` y disparar la descarga de carátula en segundo plano con `CoverManager`.

### Fase 3: Ejecución de Portables desde la Biblioteca (`library_view.cpp` & `app.cpp`)
- **Archivos**: `studio/src/library_view.cpp`, `studio/src/app.cpp`, `studio/include/studio/app.h`.
- **Tareas**:
  1. Modificar la acción al hacer clic en "JUGAR" o sobre la tarjeta de un juego:
     - En Linux: ejecutar `package_dir / executable_linux` (o `package_dir / "run.sh"`).
     - En Windows: ejecutar `package_dir / executable_windows` (o `package_dir / "run.bat"`).
     - Si se exportaron ambas plataformas, priorizar automáticamente el binario compatible con el SO host.
  2. Actualizar el tiempo de juego (`touch_played`) al iniciar la sesión.
  3. Añadir en el menú contextual `[...]` de la tarjeta la opción secundaria: `"🛠 Abrir en Recompilador"`, permitiendo reabrir la ROM en el pipeline original si el usuario lo desea.

### Fase 4: Flujo de "Importar Recompilado" y Modal Manual
- **Archivos**: `studio/src/app.cpp`, `studio/src/library_view.cpp`, `studio/include/studio/app.h`.
- **Tareas**:
  1. Agregar el botón `[ 📥 Importar Recompilado ]` en la barra superior de la Biblioteca.
  2. Implementar la inspección automática de carpetas (`inspect_portable_package`):
     - Detección de binarios ejecutables Linux/Windows.
     - Búsqueda de ROMs compatibles en `package_dir / "roms"`.
  3. Si se encuentra la ROM en `roms/`, importar automáticamente y emitir notificación de éxito.
  4. Si no se encuentra ninguna ROM en `roms/`, desplegar el modal interactivo con la alerta:
     `"No se encontro ningún rom en la carpeta correspondiente, llena los datos del juego manualmente"`
  5. Permitir completar título, consola, seleccionar archivo ROM (con opción de copiarlo a `roms/`) y guardar en la biblioteca.

---

## 2. Análisis de Riesgos y Mitigaciones

| Riesgo Técnico | Impacto | Mitigación |
| :--- | :--- | :--- |
| Entradas huérfanas en `library.toml` que no tienen `package_dir` | Medio | Si un juego en la biblioteca carece de `package_dir`, al intentar jugar se verifica si existe en la caché de Studio (`build/studio_cache/<sha1>`) o se ofrece abrir en el recompilador para re-exportar. |
| Incompatibilidad de binario de Linux en distribuciones dispares | Bajo | El empaquetador ya incluye `libSDL3.so.0` en `lib/` y RUNPATH `$ORIGIN:$ORIGIN/lib`, garantizando portabilidad en cualquier distribución x86_64. |
| Permisos de ejecución de scripts importados (`run.sh`) | Bajo | Al importar una carpeta en Linux, el importador aplica automáticamente `chmod +x` sobre los ejecutables y scripts detectados. |
| ROM con extensión no estándar en la carpeta `roms/` | Bajo | La inspección busca firmas de cabecera (`detect_rom`) además de verificar extensiones conocidas (`.nes`, `.sfc`, `.smc`, `.gb`, `.gbc`, `.gba`). |
