# DragonRecomp Studio — Prototipos de UI (Tauri vs Qt 6/QML vs Slint)

Estos tres prototipos independientes (sin conectar aún al motor C++ de `studio_lib`) permiten comparar visualmente y a nivel de código las 3 mejores tecnologías de interfaz para el rediseño de **DragonRecomp Studio**.

Todos siguen la misma guía de diseño:
- **Tema oscuro sólido (CERO gradientes)**: profundidad por elevación de superficies planas (`#0B0E14` → `#111620` → `#171D2B`) y bordes nítidos de 1-2 px.
- **Diseño Gamer amigable**: acento principal cian eléctrico (`#00E5E8`) y acentos sólidos por consola (NES `#EF4444`, SNES `#A855F7`, GB/GBC `#22C55E`, GBA `#3B82F6`).
- **Panel de Configuración listo**: incluye el selector de núcleos de compilación (por defecto **2 núcleos**) y perfil de compilación (`Rápido -O1` / `Balanceado -O2`).

## Vista rápida instantánea (sin instalar nada)

Abre [comparador_visual.html](comparador_visual.html) directamente en tu navegador (doble clic) para comparar los 3 diseños interactivamente.

## Estructura de los 3 prototipos

1. **Tauri v2 (`tauri/`)** — *HTML/CSS/JS + Rust WebView nativo (~12 MB)*
   - Diseño tipo launcher gamer moderno (barra superior, buscador, estantería por consolas y panel inspector lateral derecho).
   - Código fuente: `tauri/src/index.html`, `tauri/src/styles.css`, `tauri/src/app.js`, `tauri/src-tauri/`.
   - Prueba rápida: abre `tauri/src/index.html` en cualquier navegador, o ejecuta `cargo tauri dev` dentro de `tauri/`.

2. **Qt 6 / QML (`qml/`)** — *C++20 + QML nativo acelerado por GPU (~45 MB con DLLs)*
   - Diseño escritorio nativo con barra lateral izquierda de consolas y banner sólido de "Continuar jugando".
   - Código fuente: `qml/qml/Theme.qml`, `qml/qml/GameCard.qml`, `qml/qml/Main.qml`, `qml/main.cpp`, `qml/CMakeLists.txt`.
   - Compilación: `cmake -S qml -B build-qml && cmake --build build-qml` (requiere Qt 6.5+).

3. **Slint UI (`slint/`)** — *C++20 + Slint declarativo nativo (~7 MB, 0% WebView)*
   - Diseño consola/handheld ultra-nítido con barra inferior estilo gamepad (`[A] Jugar`, `[X] Importar`, `[F11] Fullscreen`).
   - Código fuente: `slint/ui/theme.slint`, `slint/ui/components/game_card.slint`, `slint/ui/app.slint`, `slint/main.cpp`, `slint/CMakeLists.txt`.
   - Compilación: `cmake -S slint -B build-slint && cmake --build build-slint` (descarga Slint automáticamente vía FetchContent).
