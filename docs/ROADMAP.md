# DragonRecomp — Roadmap Multi-Consola

Hoja de ruta oficial de expansión de **DragonRecomp** sobre la base actual ya consolidada (**NES 6502**, **SNES 65816**, **GB/GBC SM83**, **GBA ARM7TDMI** y **DragonRecomp Studio**).

---

## Flujo de Trabajo Estándar por Consola

Cada consola nueva se desarrolla siguiendo estrictamente este ciclo para mantener `main` estable en todo momento:

1. **Rama dedicada** (`feat/sms`, `feat/pce`, `feat/genesis`, `feat/nds`, `feat/psp`, `feat/ps1`, `feat/n64`).
2. **Investigación técnica profunda** (CPU, mapa de memoria, mappers, video, audio, interrupciones, timings y documentación de referencia).
3. **Creación de SPEC** (`docs/<CONSOLA>_SPEC.md`).
4. **Implementación** (runtime C++20, intérprete de referencia, recompilador estático C99, pruebas e integración en Studio).
5. **Pruebas de código** (CTest unitarios y diferenciales sin requerir ROMs, compilación limpia con `-Wall -Wextra`).
6. **Pruebas con ROMs reales** (en `roms/`, gitignored y nunca modificados: arranque, fallbacks, auto-aprendizaje y compilación nativa).
7. **Pruebas manuales** (jugabilidad, video, audio, save states y empaquetado portable).
8. **Validación final y subida/merge** a `main`.

---

## Matriz de Factibilidad y Reutilización

| Fase | Consola / Objetivo | Rama | Factibilidad | Dificultad | Pieza clave reutilizada / creada | Esfuerzo Est. |
| :--- | :--- | :--- | :---: | :---: | :--- | :---: |
| **1** | **Sega Master System** *(+ Game Gear)* | `feat/sms` | **95% Muy Alta** | Baja | Reutiliza `gb/` (SM83) → **Crea Core Z80** | 2–3 semanas |
| **1** | **PC Engine / TurboGrafx-16** | `feat/pce` | **95% Muy Alta** | Baja | Reutiliza `nes/` (6502) → **Crea Core HuC6280** | 2–3 semanas |
| **1** | **R&D: Arquitectura N64 Genérico** | `docs/n64-rd` | **100% (Estudio)** | Media | Diseño de overlays DMA, RSP HLE y fallback MIPS64 | 1–2 semanas |
| **2** | **Sega Genesis / Mega Drive** | `feat/genesis` | **90% Muy Alta** | Media | Reutiliza **Core Z80** (F1) → **Crea Core 68000** | 4–6 semanas |
| **2** | **Nintendo DS** | `feat/nds` | **80% Alta** | Alta | Reutiliza **ARM7TDMI** (`gba/`) → **Crea Backend GPU 3D** | 8–12 semanas |
| **2** | **PSP (PlayStation Portable)** | `feat/psp` | **80% Alta** | Alta | Reutiliza **GPU 3D** (NDS) → **Crea Lifter MIPS + HLE NIDs** | 10–14 semanas |
| **3** | **PlayStation 1 (PSX)** | `feat/ps1` | **90% Muy Alta** | Media | Reutiliza **Lifter MIPS** (PSP) + **GPU 3D** + OpenBIOS (MIT) | 5–8 semanas |
| **3** | **Nintendo 64 (Recomp Genérico)** | `feat/n64` | **70% Media-Alta** | Muy Alta | Reutiliza **Lifter MIPS** (→ 64-bit) + **GPU 3D** + R&D F1 | 10–14 semanas |

---

## FASE 1 — Victorias Rápidas, Cimientos SEGA y R&D N64

### 1.1 `smsrecomp` — Sega Master System (+ Game Gear) (`feat/sms`)
- **CPU:** Zilog Z80 (3.58 MHz) — evoluciona la experiencia de `gb/` (SM83) y construye el core Z80 reutilizable para Sega Genesis.
- **Video:** VDP TMS9918A extendido (Mode 4, 256×192 / 256×224, scroll X/Y por registro, interrupción de línea, 64 sprites; Game Gear 160×144 con paleta RGB 12-bit).
- **Audio:** SN76489 PSG (3 ondas cuadradas + 1 ruido) + soporte opcional YM2413 FM.
- **Mappers:** Sega Mapper (`$FFFC–$FFFF`), Codemasters (`$0000/$4000/$8000`) y Korean.

### 1.2 `pcerecomp` — PC Engine / TurboGrafx-16 (`feat/pce`)
- **CPU:** Hudson HuC6280 (7.16 MHz) — 65C02 extendido con MMU de 8 páginas de 8 KB (`MPR0–MPR7`), reutilizando gran parte de `nes/`.
- **Video:** HuC6270 (VDC) + HuC6260 (VCE), 1 plano de fondo + 64 sprites, paleta 9-bit.
- **Audio:** PSG wavetable de 6 canales (32 muestras de 5 bits).

### 1.3 R&D: N64 Genérico (`docs/N64_GENERIC_RECOMP_SPEC.md`)
- Especificación arquitectónica para lograr recompilación genérica de `.z64` sin exigir archivos ELF de decompilación manual:
  - Escaneo de prólogos MIPS + análisis CFG desde el entrypoint.
  - Manejo híbrido de overlays DMA en RDRAM con intérprete MIPS64 fallback + auto-aprendizaje.
  - HLE de microcódigos RSP estándar (`Fast3D`, `F3DEX`, `F3DEX2`, `S2DEX`) + fallback RSP.

---

## FASE 2 — Consolas Estrella (16-bit + Portátiles 3D)

### 2.1 `mdrecomp` — Sega Genesis / Mega Drive (`feat/genesis`)
- Nuevo lifter **Motorola 68000** en C++20/C99 + **Core Z80** de Fase 1 + VDP 2D (Planos A/B/Window, 80 sprites, H-Int/V-Int, DMA) + YM2612 FM + SN76489 PSG.

### 2.2 `ndsrecomp` — Nintendo DS (`feat/nds`)
- Reutiliza **ARM7TDMI** de `gba/` y extiende el lifter ARMv4T → **ARMv5TE (ARM946E-S)** + sincronización IPC FIFO + doble pantalla/táctil + NitroFS.
- Introduce el **Backend GPU 3D compartido (`common/gpu3d` sobre SDL3_GPU / Vulkan)** para el motor 3D GX.

### 2.3 `psprecomp` — PlayStation Portable (`feat/psp`)
- Construye el **Lifter MIPS compartido (`common/mips` — Allegrex + VFPU)**, cargador ELF/PRX/ISO, capa **HLE de NIDs/Syscalls** (`sceKernel`, `sceGu`, `sceAudio`, `sceCtrl`, `sceIo`) y traductor de Display Lists del GE conectado al Backend GPU 3D compartido.

---

## FASE 3 — Cosecha MIPS 3D Clásica

### 3.1 `psxrecomp` — PlayStation 1 (`feat/ps1`)
- Reutiliza el **Lifter MIPS** de PSP (R3000A es un subconjunto) + coprocesador GTE + **Backend GPU 3D** + **OpenBIOS (MIT)** + SPU ADPCM + lector BIN/CUE/CHD.

### 3.2 `n64recomp` — Nintendo 64 Genérico (`feat/n64`)
- Extiende el **Lifter MIPS a 64 bits (R4300i)** + intérprete MIPS64 fallback + auto-aprendizaje de overlays DMA + HLE RSP/RDP sobre el **Backend GPU 3D**, aplicando el diseño del R&D de Fase 1.
