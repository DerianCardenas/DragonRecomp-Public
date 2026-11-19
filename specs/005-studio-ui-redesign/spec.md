# Spec 005: Nueva Interfaz de DragonRecomp Studio (Mockup Obsidian) y Sistema de Preferencias

> **Estado**: IMPLEMENTADA (31/31 tests en verde)  
> **Fecha**: 2026-11-18  
> **Referencia Visual**: `DragonRecomp Studio — Mockup UI.html`  
> **Alcance**: `studio/` (`theme.h/.cpp`, `settings.h/.cpp`, `app.h/.cpp`, `library_view.cpp`, `pipeline.h/.cpp`) y soporte de `--autosave` en runtimes (`nes`, `snes`, `gb`, `gba`).

---

## 1. Resumen Ejecutivo

Esta especificación define la transición completa de **DragonRecomp Studio** hacia la nueva interfaz definida en `DragonRecomp Studio — Mockup UI.html`, junto con la implementación real de todas las opciones del panel de **Preferencias**, aplicando los ajustes obligatorios indicados por el usuario:

1. **Inspector de Biblioteca sin botón "Recompilar"**: En el panel lateral derecho (`Inspector`) de la vista de Biblioteca, solo aparecen las acciones **`▶ Jugar ahora`**, **`Abrir carpeta`** y **`Eliminar`**. El botón `"Recompilar"` **NO debe verse**.
2. **Eliminación segura (solo de la biblioteca)**: El botón **`Eliminar`** en el Inspector quita la entrada únicamente del registro de la biblioteca (`library.toml`), **sin borrar ni modificar jamás los archivos o carpetas del disco**.
3. **Eliminación de la sección "ROMs de prueba"**: En la vista de Recompilador se elimina por completo la sección inferior de carga rápida `"ROMs de prueba"` (`Super Mario (NES)`, `Super Mario (SNES)`, `Tetris (GB)`, `FireRed (GBA)`).
4. **Rutas en Preferencias**:
   - **Directorio de exportación**: Se conserva y actúa como la **carpeta por defecto para exportar juegos portables** en todo Studio.
   - **Directorio de biblioteca**: **NO debe verse** en la interfaz.
5. **Funcionalidad completa de Preferencias**: Todas las demás configuraciones del mockup (`Núcleos máximos`, `Nivel de optimización -O0/-O1/-O2/-O3`, `Directorio de exportación`, `Modo oscuro`, `Color de acento`, `Autoguardado de estado`, `Filtro CRT por defecto`, `Guardar cambios` y `Restablecer todo`) se implementan con persistencia en `settings.toml` y efecto real en el compilador, la interfaz y los runtimes.

---

## 2. Arquitectura Visual y Tokens de Diseño

### 2.1 Paleta de Colores y Temas Dinámicos

| Token | Modo Oscuro (`dark_mode = true`) | Modo Claro (`dark_mode = false`) | Uso |
| :--- | :---: | :---: | :--- |
| `--bg` | `#0C0E14` | `#F3F4F8` | Fondo general de la aplicación y caja de logs |
| `--surface` | `#161922` | `#FFFFFF` | Sidebar, Topbar, Inspector, Tarjetas |
| `--surface2` | `#1E2230` | `#E8ECF4` | Controles secundarios, inputs, píldoras, hover |
| `--border` | `#252A3A` | `#D0D6E4` | Bordes de 1px en paneles, tarjetas y divisores |
| `--text` | `#E8EAF0` | `#161922` | Texto principal |
| `--text-muted` | `#7C8299` | `#5A6278` | Etiquetas secundarias y metadatos |
| `--text-dim` | `#4A5068` | `#8C94AA` | Timestamps, bordes sutiles, iconos inactivos |
| `--cyan` | `#1EC8D4` | `#0VA6B2` | Etiquetas de sección (`INSPECTOR`), plataforma, botón seleccionar |
| `--success` | `#2ECC71` | `#27AE60` | Indicador de estado listo (`ready-dot`), badges nativos |
| `--danger` | `#E74C3C` | `#D63031` | Botón `Eliminar` de la biblioteca y alertas de error |

**Colores de Acento Seleccionables (`--primary`)**:
1. **Naranja (Por defecto)**: `#F07C1E` (`--primary-dim`: `rgba(240, 124, 30, 0.12)`)
2. **Cyan**: `#1EC8D4` (`--primary-dim`: `rgba(30, 200, 212, 0.12)`)
3. **Púrpura**: `#9B59B6` (`--primary-dim`: `rgba(155, 89, 182, 0.12)`)
4. **Verde**: `#2ECC71` (`--primary-dim`: `rgba(46, 204, 113, 0.12)`)

---

### 2.2 Estructura Global del Shell

```
+------+-----------------------------------------------------------------------------------+
|      |  DragonRecomp Studio                         (● 4 núcleos · -O2)  [+ Importar...] |  <- Topbar (52px)
|  DR  +-------------------------------------------------------------------+---------------+
|      |                                                                   |               |
| [📚] |                                                                   |               |
|      |                                                                   |   INSPECTOR   |
| [⚡] |                        ÁREA PRINCIPAL                             |      O        |
|      |             (Biblioteca / Recompilador / Preferencias)            |  SIDEBAR LOG  |
| [⚙]  |                                                                   |  (280px/260px)|
|      |                                                                   |               |
|      |                                                                   |               |
| (i)  |                                                                   |               |
+------+-------------------------------------------------------------------+---------------+
  56px
Sidebar
```

1. **Sidebar Izquierda (`56px` de ancho)**:
   - Badge superior **`DR`** (`32x32px`, `border-radius: 8px`, fondo `--primary`, texto blanco bold).
   - 3 botones de navegación (`40x40px`, `border-radius: 8px`, fondo activo `--primary-dim` y trazo `--primary`):
     1. **Biblioteca** (icono de libros/estantería)
     2. **Recompilador** (icono de rayo)
     3. **Preferencias** (icono de engranaje)
   - Icono inferior de información con tooltip `v1.0.0`.
2. **Topbar (`52px` de alto)**:
   - Izquierda: Título **`DragonRecomp Studio`** con `Recomp` coloreado en `--primary`.
   - Derecha:
     - **Status Pill**: Punto verde (`#2ECC71`) + texto monoespaciado dinámico `<N> núcleos · <opt_level>` (ej. `2 núcleos · -O1` o `4 núcleos · -O2` según las Preferencias activas).
     - Botón primario (`btn-primary`):
       - En la vista **Biblioteca**: **`+ Importar Recompilado`** (abre el selector de carpeta portable) y botón secundario **`+ Importar ROM`** (cambia al Recompilador).
       - En las demás vistas: **`+ Importar ROM`** (abre selector de ROM y pasa a la vista Recompilador).

---

## 3. Especificación por Vista

### 3.1 Vista 1: Biblioteca (`StudioView::Library`)

Dividida en dos paneles horizontales:

1. **Panel Izquierdo (`biblioteca-left`)**:
   - **Pestañas de Consola (`console-tabs`)**:
     - `Nintendo NES <count>`
     - `Super Nintendo <count>`
     - `Game Boy / Color <count>`
     - `Game Boy Advance <count>`
     - La pestaña activa se pinta con fondo `--primary` y texto blanco; las inactivas con `--surface` y borde `--border`.
   - **Cuadrícula de Juegos (`games-grid`)**:
     - Tarjetas en proporción `3:4` (`140px–180px` de ancho adaptable, `border-radius: 12px`).
     - Si la tarjeta está seleccionada (`selected_library_sha1_ == g.sha1`), muestra borde `--primary` (`2px`) y fondo `--primary-dim`, y su título inferior se colorea en `--primary`.
     - Contenido de la tarjeta:
       - Si tiene carátula descargada/asignada en `CoverManager`, dibuja la carátula con esquinas redondeadas + punto verde de estado listo (`ready-dot`, arriba a la derecha).
       - Si no tiene carátula, dibuja el diseño limpio del mockup:
         - Arriba-izquierda: etiqueta de plataforma (`NES`, `SNES`, `GB`, `GBC`, `GBA`) en `--cyan` monoespaciado.
         - Arriba-derecha: punto verde (`ready-dot`, `6x6px`, `--success`).
         - Centro: abreviatura de hasta 4 caracteres en mayúsculas (`SMB3`, `CTRA`, `ZLD1`, etc.) en fuente grande bold (`22px`).
         - Abajo-izquierda: tiempo jugado (`4h 12m` o `0m`) en monoespaciado `--text-muted`.
     - Debajo de la tarjeta: Nombre del juego truncado con elipsis (`game-card-name`).
     - **Interacción**:
       - Un clic sobre la tarjeta selecciona el juego y actualiza el panel **Inspector**.
       - Doble clic sobre la tarjeta lanza directamente el juego portable (`launch_portable_game`).
       - Clic derecho mantiene el menú contextual para gestión de carátula (`Descargar carátula`, `Asignar carátula manual`, `Restablecer carátula`).

2. **Panel Derecho: Inspector (`280px` de ancho)**:
   - Si hay un juego seleccionado en la pestaña actual (por defecto se autoselecciona el primero de la pestaña activa):
     - Encabezado:
       - Etiqueta `INSPECTOR` (`10px`, mayúsculas, `--cyan`).
       - Título completo del juego seleccionado (`18px` bold).
     - Filas de metadatos (`inspector-row` con línea divisoria inferior):
       - `Plataforma`: `NES (6502)` / `SNES (65816)` / `GB (SM83)` / `GBC (SM83)` / `GBA (ARM7TDMI)` en color `--cyan`.
       - `Tiempo jugado`: tiempo formateado (`4h 12m`).
       - `Funciones C99`: cantidad de funciones o modo (`Nativo` / `Intérprete`).
       - `Compilación`: `<N> núcleos · <opt_level>` en monoespaciado.
       - `Estado`: `Listo` en `--success`.
     - **Botones de Acción (`inspector-actions`)**:
       1. **`▶ Jugar ahora`** (botón principal `--primary`, ancho completo, `40px` alto): Ejecuta el binario portable del sistema operativo anfitrión (`launch_portable_game`).
       2. **`Abrir carpeta`** (botón secundario `--surface2` con borde `--border`): Abre `package_dir` en el explorador de archivos del sistema.
       3. **`Eliminar`** (botón secundario con texto `--danger` `#E74C3C` y borde `rgba(231, 76, 60, 0.3)`): **Elimina el registro únicamente de `library.toml` (`library_.remove(sha1)` + `library_.save()`) sin tocar ningún archivo en disco**.
       - *(Regla explícita: El botón `"Recompilar"` NO se incluye en el Inspector)*.

---

### 3.2 Vista 2: Recompilador (`StudioView::Recompiler`)

Dividida en dos paneles horizontales:

1. **Panel Izquierdo (`recomp-left`)**:
   - **Zona de Arrastre (`drop-zone`)**:
     - Recuadro con borde punteado (`2px dashed --border`, cambia a `--cyan` con fondo `--cyan-dim` al pasar el cursor).
     - Icono de subida, título `"Arrastra tu ROM aquí"`.
     - Chips de formatos soportados: `.nes`, `.sfc`, `.smc`, `.gb`, `.gbc`, `.gba`.
     - Botón cyan **`Seleccionar archivo`** (`open_rom_file_dialog()`).
     - Subtítulo `"Detección automática de plataforma"`.
   - **Tarjeta de Resultado (`recomp-result`)** (visible cuando hay una ROM cargada en el pipeline):
     - **Cabecera (`result-header`)**:
       - Izquierda: Título de la ROM + badge cyan de arquitectura (`SNES (65816)`) + badge verde de estado (`100% Nativo` o `Modo Intérprete` / `Compilando...`).
       - Derecha: Etiqueta `SHA-1` y primeros 12 caracteres del hash en monoespaciado (`6b47bb75d165…`).
     - **Cuerpo (`result-body`)**:
       - Izquierda (`result-meta`):
         - Grid de 3 bloques:
           1. `FUNCIONES C99` (valor numérico grande en monoespaciado)
           2. `INSTRUCCIONES` (valor numérico grande en monoespaciado)
           3. `COMPILACIÓN` (tiempo en segundos, ej. `4.89s`)
         - Barra `Progreso de optimización` (`compile-bar` con gradiente de `--primary` a `--cyan` reflejando `pipeline_.progress()`).
         - Checkboxes en línea: `[ ] Pantalla completa` y `[ ] Filtro CRT`.
       - Derecha (`result-actions`, ancho `175px`):
         - **`▶ Jugar (60 FPS)`** (`btn-play`): Lanza el binario recién compilado (`pipeline_.launch_game`).
         - **`Exportar portable`** (`btn-secondary`): Abre el modal de exportación preconfigurado con el **Directorio de exportación** por defecto de Preferencias.
         - **`Guardar en biblioteca`** (`btn-secondary`): Exporta directamente el paquete portable para el sistema anfitrión dentro del **Directorio de exportación** por defecto y lo registra en `library.toml`.
   - *(Regla explícita: NO existe la sección `"ROMs de prueba"`).*

2. **Panel Derecho: Log y Recuperación (`recomp-side`, `260px` de ancho)**:
   - **Log de compilación (`log-box`)**:
     - Etiqueta superior `LOG DE COMPILACIÓN` (`--cyan`).
     - Consola monoespaciada oscura (`--bg`, borde `--border`) que muestra el historial cronológico de eventos del pipeline (`MM:SS` + mensaje coloreado según severidad: `log-info` cyan, `log-msg` gris, `log-ok` verde, `log-warn` primario/ámbar).
   - **Acciones de recuperación**:
     - Etiqueta `ACCIONES DE RECUPERACIÓN` (`--cyan`).
     - Combo desplegable con los síntomas conocidos.
     - Botón **`Intentar acción`** (`btn-primary`, ancho completo) + mensaje compacto con el resultado de la acción.

---

### 3.3 Vista 3: Preferencias (`StudioView::Preferences`)

Contenedor centrado/alineado a la izquierda (`max-width: 640px`, padding `24px`) con 4 bloques de configuración y persistencia en `settings.toml`:

#### 1. Sección `COMPILACIÓN`
- **Núcleos máximos** (`Límite de hilos para compilación paralela`):
  - Slider interactivo de `1` a `16` con indicador numérico monoespaciado en color `--primary`.
  - Efecto: Ajusta `pipeline_.set_max_compile_threads(threads)` y actualiza la píldora de estado en la Topbar (`<N> núcleos · <opt>`).
- **Nivel de optimización** (`Nivel de optimización de GCC/Clang al compilar C99`):
  - Combo desplegable con opciones: `-O0`, `-O1`, `-O2`, `-O3`.
  - Efecto: Ajusta el flag de optimización pasado a Clang/GCC en `Pipeline::compile_c_to_binary` y `Pipeline::compile_windows_binary`, y actualiza la píldora de estado en la Topbar.

#### 2. Sección `RUTAS`
- **Directorio de exportación** (`Dónde se guardan los portables exportados`):
  - Caja monoespaciada mostrando la ruta actual + botón **`Cambiar`** que abre el selector de carpetas del sistema (`SDL_ShowOpenFolderDialog`).
  - Efecto: Define la carpeta por defecto donde se exportan todos los juegos portables (sincronizado con `export_dir_buf_`).
- *(Regla explícita: La fila `"Directorio de biblioteca"` NO debe mostrarse).*

#### 3. Sección `APARIENCIA`
- **Modo oscuro** (`Alterna entre tema oscuro y claro`):
  - Toggle switch estilizado (`36x20px`).
  - Efecto: Alterna en tiempo real los colores de `ThemeColors` y `ImGui::GetStyle()` entre la paleta oscura Obsidian (`#0C0E14`) y la paleta clara (`#F3F4F8`).
- **Color de acento** (`Color primario de la interfaz`):
  - 4 círculos seleccionables (`24x24px`) con borde de selección activo:
    - Naranja (`#F07C1E`)
    - Cyan (`#1EC8D4`)
    - Púrpura (`#9B59B6`)
    - Verde (`#2ECC71`)
  - Efecto: Cambia en tiempo real el color primario (`--primary` y `--primary-dim`) de toda la interfaz (logo `DR`, texto `Recomp`, pestañas activas, botones principales, sliders y toggles).

#### 4. Sección `JUEGO`
- **Autoguardado de estado** (`Guarda automáticamente al cerrar el juego`):
  - Toggle switch (`on` por defecto).
  - Efecto:
    - Cuando está activo, al lanzar un juego desde Studio (tanto desde la Biblioteca como desde el Recompilador), Studio pasa el argumento `--autosave` (y define `DRAGON_AUTOSAVE_STATE=1` en el entorno del proceso hijo).
    - En los 4 runtimes (`nes`, `snes`, `gb`, `gba`), al cerrarse limpiamente la ventana del juego (`SDL_EVENT_QUIT` o tecla `Escape`), si `--autosave` o `DRAGON_AUTOSAVE_STATE=1` está activo, el frontend guarda automáticamente el estado actual en el archivo de autoguardado (`<rom_path>.auto.drst` / slot 0).
- **Filtro CRT por defecto** (`Aplica scanlines retro al iniciar`):
  - Toggle switch (`off` por defecto).
  - Efecto:
    - Inicializa y sincroniza la casilla `Filtro CRT` en el Recompilador.
    - Al lanzar cualquier juego desde la Biblioteca (`launch_portable_game`), aplica `--crt` si esta preferencia o la configuración del juego está activa.

#### 5. Botones Inferiores
- **`Guardar cambios`** (`btn-primary`): Persiste la configuración actual en `settings.toml`, aplica los valores al `Pipeline` y muestra confirmación visual.
- **`Restablecer todo`** (`btn-ghost`): Restaura todos los controles a sus valores predeterminados de fábrica y actualiza `settings.toml`.

---

## 4. Persistencia de Preferencias (`settings.toml`)

Archivo ubicado en `%APPDATA%/DragonRecomp/settings.toml` (Windows) o `$XDG_CONFIG_HOME/dragonrecomp/settings.toml` / `~/.config/dragonrecomp/settings.toml` (Linux):

```toml
[compilation]
max_threads = 4
opt_level = "-O2"

[paths]
export_directory = "/home/derian/Documentos/Proyectos/Derian/DragonRecomp/ExportedGames"

[appearance]
dark_mode = true
accent_color = "orange" # "orange" | "cyan" | "purple" | "green"

[game]
auto_save_state = true
default_crt_filter = false
```

---

## 5. Criterios de Aceptación

1. **Fidelidad con el Mockup HTML**: La aplicación muestra la Sidebar izquierda de `56px` con el logo `DR` y los 3 iconos de navegación, la Topbar de `52px` con la píldora `<N> núcleos · <opt>`, y las 3 vistas (`Biblioteca`, `Recompilador`, `Preferencias`) con la paleta Obsidian.
2. **Inspector de Biblioteca**:
   - Seleccionar un juego en la cuadrícula muestra sus detalles en el panel `Inspector` a la derecha.
   - **No existe** el botón `"Recompilar"` en el Inspector.
   - El botón `"Eliminar"` quita el juego de la biblioteca (`library.toml`) sin borrar ningún archivo del disco.
3. **Recompilador Limpio**:
   - **No existe** la sección `"ROMs de prueba"`.
   - El panel derecho muestra el `Log de compilación` con marcas de tiempo y la sección `Acciones de recuperación`.
   - El botón `"Guardar en biblioteca"` exporta el juego al `Directorio de exportación` por defecto y lo añade a la biblioteca.
4. **Preferencias Funcionales**:
   - **No existe** el campo `"Directorio de biblioteca"`.
   - Cambiar `Núcleos máximos` y `Nivel de optimización` actualiza la píldora de la Topbar y los flags reales de compilación en `Pipeline`.
   - Cambiar `Directorio de exportación` actualiza la carpeta por defecto usada al exportar portables.
   - Cambiar `Modo oscuro` y `Color de acento` actualiza los colores de la interfaz en tiempo real.
   - `Autoguardado de estado` guarda automáticamente el save state al salir de un juego en los runtimes.
   - `Filtro CRT por defecto` activa scanlines al iniciar juegos.
   - `Guardar cambios` y `Restablecer todo` persisten y restauran correctamente `settings.toml`.
