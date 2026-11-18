# Tasks 004: Tareas de Implementación

> **Spec Asociada**: [Spec 004: Biblioteca Basada en Portables e Importación de Recompilados](spec.md)  
> **Estado General**: COMPLETADO (31/31 pruebas de CTest superadas al 100%)

---

## Fase 1: Modelo de Datos y Persistencia de Portables (`GameLibrary`)

- [x] **1.1 Extensión de estructura `GameEntry` (`studio/include/studio/library.h`)**
  - [x] Añadir `std::filesystem::path package_dir`.
  - [x] Añadir `std::filesystem::path executable_linux`.
  - [x] Añadir `std::filesystem::path executable_windows`.
  - [x] Método auxiliar `bool is_portable() const noexcept`.

- [x] **1.2 Actualización de `GameLibrary` (`studio/src/library.cpp`)**
  - [x] Implementar `add_portable(const RomInfo& info, const std::filesystem::path& package_dir, const std::filesystem::path& exec_linux, const std::filesystem::path& exec_windows, GameMode mode)`.
  - [x] Implementar `add_manual_portable(...)`, `inspect_portable_folder(...)` y `resolve_host_executable(...)`.
  - [x] Serializar `package_dir`, `executable_linux` y `executable_windows` en `GameLibrary::save()`.
  - [x] Deserializar dichos campos en `GameLibrary::load()` con fallback seguro si están ausentes.

- [x] **1.3 Pruebas Unitarias de Biblioteca (`studio/tests/library_tests.cpp`)**
  - [x] Test para serialización y lectura de `package_dir` y ejecutables.
  - [x] Test para `add_portable`, `add_manual_portable`, `inspect_portable_folder` (con y sin ROM) y `resolve_host_executable()`.

---

## Fase 2: Registro Exclusivo en Exportación (`app.cpp` & `packager.cpp`)

- [x] **2.1 Desacoplar `load_rom()` de la biblioteca (`studio/src/app.cpp`)**
  - [x] Eliminar `library_.add_rom(info)` de `load_rom()`.
  - [x] Asegurar que cargar una ROM mantenga el flujo de análisis en la vista de Recompilación sin ensuciar la biblioteca.

- [x] **2.2 Registro tras empaquetado exitoso (`studio/src/app.cpp`)**
  - [x] En el botón "⚡ Exportar Ahora", tras recibir `last_package_result_.success == true`:
    - [x] Identificar ejecutables Linux (`linux_executable_path`) y Windows (`windows_executable_path`).
    - [x] Registrar en `library_` con `add_portable(...)`.
    - [x] Invocar `library_.save()`.
    - [x] Disparar la descarga asíncrona de carátula en segundo plano con `cover_mgr_`.

---

## Fase 3: Ejecución Directa de Portables desde la Biblioteca (`library_view.cpp`)

- [x] **3.1 Lanzador de Portables (`studio/src/app.cpp` & `studio/include/studio/app.h`)**
  - [x] Implementar `StudioApp::launch_portable_game(const GameEntry& entry)`:
    - [x] Si SO es Linux: ejecutar `executable_linux` en su carpeta con `LD_LIBRARY_PATH` hacia `lib/`.
    - [x] Si SO es Windows: ejecutar `executable_windows` (`.exe`).
    - [x] Manejo de errores amigable si el ejecutable del SO anfitrión no existe.
    - [x] Registrar telemetría con `library_.touch_played()`.

- [x] **3.2 Conectar Tarjetas de la Biblioteca (`studio/src/library_view.cpp`)**
  - [x] Modificar el clic sobre la tarjeta / botón "JUGAR" para invocar `launch_portable_game(*g)` en vez de `load_rom()`.
  - [x] Añadir opciones contextuales `[...]`: `"▶ Ejecutar juego portable"`, `"📁 Abrir carpeta del portable"` y `"⚙ Abrir ROM en Recompilador"`.

---

## Fase 4: Importador de Recompilados y Modal de Entrada Manual

- [x] **4.1 Inspección Automática de Carpeta (`studio/src/app.cpp` & `studio/src/library.cpp`)**
  - [x] Implementar `StudioApp::import_portable_folder(const std::filesystem::path& folder_path)`:
    - [x] Escaneo de binarios nativos ELF, `.exe`, `run.sh` y `run.bat`.
    - [x] Escaneo de la carpeta `roms/` en busca de archivos `.nes`, `.sfc`, `.smc`, `.gb`, `.gbc`, `.gba`.
    - [x] Si hay ROM válida: extraer metadatos mediante `detect_rom()` y registrar en `library_`.
    - [x] Emitir notificación: `"✔ Juego recompilado importado: <Título>"`.

- [x] **4.2 Modal de Fallback Manual por Falta de ROM (`studio/src/app.cpp`)**
  - [x] Detectar cuando `roms/` no contiene ROMs válidas.
  - [x] Abrir el modal con la alerta:
    `"No se encontro ningún rom en la carpeta correspondiente, llena los datos del juego manualmente"`.
  - [x] Formulario ImGui con:
    - [x] Campo de texto editable para **Título del juego**.
    - [x] Combo desplegable para seleccionar **Plataforma** (NES, SNES, GB, GBC, GBA).
    - [x] Campos de ejecutable Linux y Windows.
    - [x] Selector de archivo opcional para vincular la **ROM original externa** y copiarla a `package_dir / "roms"`.
  - [x] Botón "✔ Guardar e Importar" que valida los datos y guarda la entrada en la biblioteca.

- [x] **4.3 Botón en UI de Biblioteca (`studio/src/library_view.cpp`)**
  - [x] Colocar el botón `[ 📥 Importar Recompilado... ]` en la barra superior de la vista de Biblioteca y en la tarjeta de bienvenida / slot de añadir.
  - [x] Conectar con el diálogo de selección de carpeta del sistema (`SDL_ShowOpenFolderDialog`) y soporte de Drag & Drop de carpetas.

---

## Fase 5: Verificación y Pruebas Finales

- [x] **5.1 Pruebas de Flujo Completo E2E**
  - [x] Cargar ROM sin exportar: no agrega entradas a la biblioteca.
  - [x] Exportar juego: biblioteca añade la tarjeta portable.
  - [x] Clic en tarjeta: lanza el ejecutable portable nativo sin recompilar.
  - [x] Importar carpeta con ROM: importa automáticamente.
  - [x] Importar carpeta sin ROM: muestra alerta y modal interactivo para completar datos.
- [x] **5.2 Suite Automatizada**
  - [x] Ejecutar `ctest` y verificar que las 31 pruebas pasen al 100%.
