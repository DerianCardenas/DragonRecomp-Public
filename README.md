# 🐉 DragonRecomp

**Convierte tus juegos clásicos de NES, SNES, Game Boy / Game Boy Color, Game Boy Advance, Sega Master System / Game Gear, PC Engine / TurboGrafx-16, Sega Genesis / Mega Drive, Nintendo DS, PlayStation Portable (PSP) y PlayStation 1 (PS1) en juegos nativos para PC.**

[![Descargar Studio para Windows](https://img.shields.io/badge/Descargar%20Studio%20v1.0.0-Windows%20x64%20(Portable)-00b4d8?style=for-the-badge&logo=windows)](https://github.com/DerianCardenas/DragonRecomp-Public/releases/latest)
[![Descargar Studio para Linux](https://img.shields.io/badge/Descargar%20Studio%20v1.0.0-Linux%20x64%20(Portable)-f77f00?style=for-the-badge&logo=linux)](https://github.com/DerianCardenas/DragonRecomp-Public/releases/latest)

DragonRecomp es una herramienta que toma tus juegos favoritos de **NES (Nintendo Entertainment System)**, **Super Nintendo (SNES)**, **Game Boy / Game Boy Color (DMG / CGB)**, **Game Boy Advance (GBA)**, **Sega Master System / Game Gear (SMS / GG)**, **PC Engine / TurboGrafx-16 (PCE / TG-16)**, **Sega Genesis / Mega Drive (MD)**, **Nintendo DS (NDS)**, **PlayStation Portable (PSP)** y **PlayStation 1 (PS1)** y los recompila en programas nativos para **Windows** (`.exe`) y **Linux**, con rendimiento perfecto a 60 FPS, respuesta instantánea en los controles y sin la pesadez de los emuladores tradicionales.

---

> [!IMPORTANT]
> ### ⚠️ Requisito indispensable: Necesitas tu propia ROM original
>
> **DragonRecomp NO incluye, NO aloja ni distribuye ninguna ROM ni archivos de juegos con derechos de autor.**
>
> Para utilizar esta herramienta y generar ejecutables de tus juegos, **es obligatorio que cuentes con una copia digital legal y de respaldo obtenida de tu propio cartucho físico original** de NES, Super Nintendo, Game Boy / Game Boy Color, Game Boy Advance, Sega Master System / Game Gear, PC Engine / TurboGrafx-16, Sega Genesis / Mega Drive o Nintendo DS:
>
> - **Formatos admitidos**: Archivos `.nes` (para NES), `.sfc` o `.smc` (para Super Nintendo), `.gb` o `.gbc` (para Game Boy y Game Boy Color), `.gba` (para Game Boy Advance), `.sms` o `.gg` (para Sega Master System y Game Gear), `.pce` o `.sgx` (para PC Engine / TurboGrafx-16), `.md`, `.gen`, `.smd` o `.bin` (para Sega Genesis / Mega Drive), `.nds` (para Nintendo DS), `.iso`, `.cso`, `.pbp`, `.elf` o `.prx` (para PlayStation Portable) y `.cue`, `.bin`, `.iso` o `.psexe` (para PlayStation 1).
> - DragonRecomp actúa únicamente como un compilador y reproductor de hardware fiel; no provee material protegido por copyright.
> - **Esto también aplica a los paquetes exportados**: el ejecutable recompilado solo contiene la lógica del juego traducida a código nativo — los gráficos y el audio los sigue leyendo del ROM original en tiempo de ejecución. Ningún paquete generado por DragonRecomp incluye el ROM. Si compartes la carpeta exportada con alguien, **esa persona necesita poner su propia copia legal** del ROM — exactamente igual que proyectos como [Zelda64Recomp](https://github.com/Mr-Wiseguy/Zelda64Recomp) o nxnative. Ver la sección [📦 ROM original en paquetes exportados](#-rom-original-en-paquetes-exportados) más abajo.

---

## 💡 ¿En qué se diferencia de un emulador común?

* **Un emulador tradicional**: Finge ser una consola completa dentro de tu computadora mientras juegas, traduciendo cada instrucción una por una en tiempo real. Esto a menudo consume más recursos, genera retraso en los controles (*input lag*) y puede sufrir pequeñas pausas o tirones.
* **DragonRecomp (Recompilador Estático)**: Lee el juego una sola vez y lo traduce directamente a código nativo de computadora (C y C++). El resultado es un juego de PC moderno y real:
  - 🚀 **Rendimiento fluido**: 60 fotogramas por segundo (FPS) estables como roca.
  - ⚡ **Latencia ultrabaja**: Respuesta inmediata a cada pulsación de botón.
  - 🔋 **Uso mínimo de procesador**: No calienta tu computadora ni drena tu batería.
  - 🎮 **Plug & Play**: Compatible con teclado y mandos de consola (Xbox, PlayStation, Nintendo Switch y genéricos).

---

## 🚀 Guía Rápida: Cómo usar DragonRecomp Studio

DragonRecomp incluye **DragonRecomp Studio**, una interfaz gráfica moderna, intuitiva y en español diseñada para que cualquier persona pueda convertir sus juegos en pocos clics.

```
+--------------------------------------------------------------------------+
|  🐉 DragonRecomp Studio                                   [📁 Abrir ROM] |
+--------------------------------------------------------------------------+
|                                                                          |
|     [ 📁 SELECCIONAR ARCHIVO ROM (.nes, .sfc, .gb, .gba, .sms, .pce, .md).. ] |
|                                                                          |
|           — o arrastra y suelta tu archivo ROM aquí —                     |
|                                                                          |
+--------------------------------------------------------------------------+
|  Destino:  ( ) Solo Linux     ( ) Solo Windows (.exe)     (*) Ambos      |
|                                                                          |
|  [ ▶ PROBAR JUEGO ]                 [ 📦 EXPORTAR PAQUETE PORTABLE ]     |
+--------------------------------------------------------------------------+
```

### Paso 1: Abre DragonRecomp Studio
Usa el lanzador incluido en la raíz del proyecto (compila la primera vez si hace falta):

* **Windows**: haz doble clic en **`run-studio.bat`**.
* **Linux**: ejecuta **`./run-studio.sh`** (o haz doble clic si tu entorno lo permite).

También puedes iniciar el binario directamente en Linux:
```sh
./build/studio/dragon-studio
```

> En Linux, para añadir DragonRecomp Studio al menú de aplicaciones con icono,
> ejecuta `./scripts/install-launcher.sh` (y `./scripts/uninstall-launcher.sh` para quitarlo).

### Paso 2: Selecciona tu juego
Tienes dos formas muy sencillas de cargar tu juego:
* Haz clic en el botón **`📁 SELECCIONAR ARCHIVO ROM`** y elige tu archivo (`.nes`, `.sfc`, `.smc`, `.gb`, `.gbc`, `.gba`, `.sms`, `.gg`, `.pce`, `.sgx`, `.md`, `.gen`, `.smd`, `.bin`) desde el explorador de carpetas.
* O simplemente **arrastra el archivo de tu juego** desde tu carpeta y suéltalo dentro de la ventana del programa.

### Paso 3: Análisis y Recompilación automática
El programa detectará al instante si se trata de un juego de NES, SNES, Game Boy / Game Boy Color, Game Boy Advance, Sega Master System / Game Gear, PC Engine / TurboGrafx-16 o Sega Genesis / Mega Drive, identificará el título, la memoria del cartucho, comprobará su compatibilidad y preparará la recompilación en segundos sin que tengas que configurar parámetros complicados.

### Paso 4: Jugar o Exportar
* **▶ Probar juego**: Pulsa este botón para abrir y disfrutar tu juego inmediatamente en tu pantalla.
* **📦 Exportar paquete portable**: Elige tu plataforma favorita (**Windows**, **Linux** o **Ambos**) y haz clic en exportar.
  * Para **Windows**: Te generará una carpeta lista con su ejecutable `.exe`, librerías necesarias y un lanzador `run.bat` que funciona sin instalaciones adicionales.
  * Para **Linux**: Te creará un paquete listo con el binario nativo y el script de inicio `run.sh`.

### 📦 ROM original en paquetes exportados

El recompilador solo traduce la **lógica de la CPU** a código nativo; los
gráficos y el audio del juego siguen viviendo en el ROM original, y el
ejecutable los lee de ahí al arrancar — igual que cualquier otro proyecto de
recompilación estática (Zelda64Recomp, N64Recomp, nxnative, ...). Por eso
**ningún paquete exportado por DragonRecomp incluye el ROM**: incluirlo sería
distribuir el contenido con derechos de autor de Nintendo a quien reciba la
carpeta, aunque la lógica del juego ya esté recompilada.

Cada paquete exportado trae una carpeta `roms/` vacía con un `roms/LEEME.txt`
explicando esto. Para jugar:

1. **Tú** (quien exportó) no necesitas hacer nada extra: el ejecutable
   primero intenta la ruta de tu ROM en esta máquina, y si ya no existe (p.
   ej. moviste el proyecto), busca automáticamente en `roms/` por la huella
   SHA-1 del juego.
2. **Quien reciba la carpeta** debe colocar **su propia copia legal** del
   ROM dentro de `roms/` (cualquier nombre de archivo — se detecta por
   SHA-1, no por nombre) o pasarla explícitamente:
   ```sh
   ./run.sh --rom /ruta/a/tu/copia.nes        # Linux
   run.bat --rom C:\ruta\a\tu\copia.nes        # Windows
   ```
3. Si el ROM no coincide con el SHA-1 esperado (otra revisión, otro idioma,
   un juego distinto), el ejecutable lo dice claramente al arrancar en vez
   de fallar en silencio o con un error críptico.

---

## 🎮 Controles

Puedes jugar tanto con tu teclado como conectando cualquier mando por USB o Bluetooth (los controles se reconocen automáticamente gracias a SDL3):

| Acción en el juego | Tecla alternativa 1 | Tecla alternativa 2 | Mando / Gamepad |
| :--- | :---: | :---: | :---: |
| **Cruz direccional (Moverse)** | Flechas de dirección | Teclas `W`, `A`, `S`, `D` | D-Pad o Palanca izquierda |
| **Botón A / 1 (SMS) / I (PCE)** | `X` *(NES/SNES/PCE)* / `Z` *(GB/GBA/SMS/GG/MD)* | `J` | Botón Sur (A en Xbox / Cruz en PS) |
| **Botón B / 2 (SMS) / II (PCE)** | `Z` *(NES/SNES/PCE)* / `X` *(GB/GBA/SMS/GG/MD)* | `K` | Botón Este (B en Xbox / Círculo en PS) |
| **Botón C** *(Genesis/MD)* | `C` | `L` | Gatillo / Botón Este |
| **Botón X** *(SNES / Genesis 6-btn)* | `V` *(SNES)* / `Q` *(MD)* | `I` *(SNES)* / `U` *(MD)* | Botón Norte (Y en Xbox / Triángulo en PS) |
| **Botón Y** *(SNES / Genesis 6-btn)* | `C` *(SNES)* / `E` *(MD)* | `L` *(SNES)* / `I` *(MD)* | Botón Oeste (X en Xbox / Cuadrado en PS) |
| **Botón Z** *(Genesis 6-btn)* | `R` | `O` | Gatillo `RB` / `R1` |
| **Gatillo L** *(SNES / GBA)* | `Q` | `U` *(GBA)* | Gatillo o botón `LB` / `L1` |
| **Gatillo R** *(SNES / GBA)* | `E` | `R` *(SNES)* / `I` *(GBA)* | Gatillo o botón `RB` / `R1` |
| **Start / Run (PCE) / Pause (SMS)** | `Enter` | `Espacio` *(SMS/GG)* | Botón Start / Menú / Options |
| **Select / Mode (MD)** | `Shift Derecho` | `Tab` / `Backspace` | Botón Select / Back / Share |
| **Salir del juego** | `Escape` | — | Guía / Home |

### ⌨️ Atajos y Guardado de Estados (Hotkeys)

| Atajo | Función |
| :--- | :--- |
| **`F5`** | **Guardado Rápido (*Quick Save*)**: Guarda el estado de la partida en la ranura activa. |
| **`F7`** | **Carga Rápida (*Quick Load*)**: Restaura el estado de la partida desde la ranura activa. |
| **`0` a `9`** | **Seleccionar Ranura (*Slots*)**: Cambia entre las 10 ranuras de guardado disponibles (0-9). |
| **`F8`** | **Relación de Aspecto**: Alterna entre proporción clásica (4:3 en NES/SNES/SMS/PCE/Genesis, 10:9 en GB/GG, 3:2 en GBA) y pantalla completa. |
| **`F9`** | **Filtro CRT**: Alterna en tiempo real las líneas de barrido retro (*scanlines*). |
| **`F11`** o **`Alt + Enter`** | **Pantalla Completa**: Alterna entre modo ventana y pantalla completa. |
| **Archivos `.sav`** | **Persistencia en batería**: Los juegos que guardan partida (Zelda, Pokémon, etc.) persisten automáticamente su archivo `.sav`. |

---

## 💻 Requisitos del Sistema

* **Sistema Operativo**:
  * **Windows**: Windows 10 o Windows 11 (64 bits).
  * **Linux**: Cualquier distribución moderna de 64 bits (Ubuntu 22.04+, Fedora, Arch, Debian, etc.).
* **Gráficos**: Tarjeta gráfica o chip integrado con soporte para OpenGL / SDL3.
* **Dispositivo de entrada**: Teclado o cualquier mando compatible con PC (Xbox, DualShock/DualSense, Switch Pro Controller, 8BitDo, etc.).

---

## 🛠️ Para Desarrolladores y Compilación desde Fuente

Si deseas clonar el repositorio y compilar el proyecto por ti mismo:

### Requisitos de desarrollo
* Compilador de C++ con soporte para **C++20** (Clang 15+ o GCC 12+ en Linux; MSVC o MinGW-w64 en Windows).
* **CMake** ≥ 3.25.
* **Ninja** (recomendado para compilación rápida).
* Biblioteca **SDL3** (incluyendo cabeceras de desarrollo).

### Instrucciones de construcción

```sh
# 1. Clonar el repositorio
git clone https://github.com/DerianCardenas/DragonRecomp.git
cd DragonRecomp

# 2. Configurar el proyecto
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 3. Compilar todas las herramientas y la interfaz gráfica
cmake --build build

# 4. (Opcional) Ejecutar las pruebas automatizadas
ctest --test-dir build --output-on-failure
```

### Documentación técnica
* [docs/SPEC.md](docs/SPEC.md) — Arquitectura del recompilador y diseño del runtime.
* [docs/NES_SPEC.md](docs/NES_SPEC.md) — Especificación técnica del recompilador NES (MOS 6502, PPU 2C02, APU).
* [docs/SNES_SPEC.md](docs/SNES_SPEC.md) — Especificación técnica de SNES (W65C816, PPU 5C77/5C78, SPC700).
* [docs/GB_SPEC.md](docs/GB_SPEC.md) — Especificación técnica de Game Boy / Game Boy Color (Sharp SM83, PPU, APU, MBC).
* [docs/GBA_SPEC.md](docs/GBA_SPEC.md) — Especificación técnica de Game Boy Advance (ARM7TDMI ARM/Thumb, PPU Modos 0–5, DirectSound APU, DMA, BIOS HLE).
* [docs/SMS_SPEC.md](docs/SMS_SPEC.md) — Especificación técnica de Sega Master System y Game Gear (Zilog Z80, VDP Modo 4, SN76489 PSG, mappers Sega/Codemasters/Korean).
* [docs/PCE_SPEC.md](docs/PCE_SPEC.md) — Especificación técnica de PC Engine y TurboGrafx-16 (Hudson HuC6280, HuC6270 VDC, HuC6260 VCE, PSG de 6 canales).
* [docs/GENESIS_SPEC.md](docs/GENESIS_SPEC.md) — Especificación técnica de Sega Genesis y Mega Drive (Motorola 68000, Z80, VDP Modo 5, YM2612 FM + SN76489 PSG).
* [docs/NDS_SPEC.md](docs/NDS_SPEC.md) — Especificación técnica de Nintendo DS (ARM946E-S + ARM7TDMI, Dual 2D A/B, 3D GX + `common/gpu3d`, SPU 16 canales, Táctil TSC2046).
* [docs/AUTO_PIPELINE_SPEC.md](docs/AUTO_PIPELINE_SPEC.md) — Detección automática, pipeline de aprendizaje y diagnóstico.
* [docs/ROADMAP.md](docs/ROADMAP.md) — Plan de expansión multi-consola y flujo de ingeniería por plataforma.
* [docs/MILESTONES.md](docs/MILESTONES.md) — Hoja de ruta y estado de desarrollo del proyecto.

---

## 📜 Licencia

Este proyecto está distribuido bajo la **Licencia MIT**. Consulta el archivo `LICENSE` o los términos de licencia abierta para más detalles.
Nintendo, NES, Super Nintendo, Game Boy y Game Boy Advance son marcas registradas de Nintendo Co., Ltd.; Sega, Master System, Game Gear, Genesis y Mega Drive son marcas registradas de SEGA Corporation; PC Engine y TurboGrafx-16 son marcas registradas de NEC Corporation / Konami Digital Entertainment (anteriormente Hudson Soft). Este proyecto es un desarrollo independiente de código abierto con fines de preservación e investigación y no está afiliado ni respaldado por Nintendo, SEGA ni NEC/Konami.
