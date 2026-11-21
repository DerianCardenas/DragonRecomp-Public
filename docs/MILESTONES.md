# Milestones

Status values: `todo` · `in-progress` · `done` · `blocked`.
Details and acceptance criteria: [NES_SPEC.md](NES_SPEC.md) §5,
[SNES_SPEC.md](SNES_SPEC.md) §6, [GB_SPEC.md](GB_SPEC.md) §10, [GBA_SPEC.md](GBA_SPEC.md) §6.

**Entornos NES, SNES y Game Boy: cerrados y verificados (2026-11-13).** Todos sus
milestones (N1–N5, S1–S4, G1–G5) están `done`; build limpio sin warnings
(clang, Debug) y las 29 pruebas de CTest pasan al 100%. NES (SMB, DK) y SNES
(SMB) recompilan y arrancan con boot tests nativos; SMW corre en modo intérprete.
Game Boy conserva sus suites unitarias (CPU/PPU/APU/recomp) verdes; aún no incluye
configs de juego ni boot tests end-to-end por ausencia de ROMs en el repo.

**2026-11-15 — infraestructura de verificación (spec 003).** La auditoría de
2026-11-14 encontró que "done" en la tabla de abajo describe la *capacidad
construida*, no un resultado verificado en un checkout limpio: no había CI, los
boot tests ROM-gated se auto-saltaban como `return 0` indistinguible de un pass,
y las aserciones de pantalla eran débiles (`distinct_colors >= 5`). `specs/003-ci-e2e-verificacion/`
corrige esto: CI hosted (`.github/workflows/ci.yml`, sin ROMs comerciales),
resolución de ROMs por `DRAGON_ROM_DIR`/SHA-1 con skip real (`SKIP_RETURN_CODE 77`),
golden-hash de framebuffer para DK, y un `report.toml` por juego (emitido por
cada recompilador en `generated/report.toml`) que documenta el **modo real**
en vez de un `done` plano:

| Juego | Plataforma | Modo real | Fuente |
|-------|-----------|-----------|--------|
| Super Mario Bros. | NES | nativo (0 fallbacks, boot test + golden-hash en `dk_boot_test.cpp` como patrón; SMB usa el mismo patrón sin golden-hash aún) | `nes/games/smb/smb.toml`, `report.toml` |
| Donkey Kong | NES | **nativo, verificado por golden-hash** de framebuffer en 2 checkpoints | `nes/games/dk/dk.toml`, `report.toml` |
| Super Mario Bros. SNES | SNES | nativo (0 fallbacks, boot test) | `snes/games/smb_snes/smb_snes.toml`, `report.toml` |
| The Legend of Zelda: A Link to the Past | SNES | **nativo** (soporte de cambio de contexto por corrutinas en IRQ/NMI para hilo 3D de Trifuerza y propagación M/X inter-procedimental) | `snes/runtime/src/bus.cpp`, `snes/recompiler/src/disassembler.cpp` |
| Street Fighter 5 (Hack) | SNES | **nativo** (open-bus en cartuchos 0-SRAM para anticopia `$70:0000`, despacho de `BRK`/`COP` y modos M/X inter-procedimentales) | `snes/runtime/src/bus.cpp`, `snes/recompiler/src/emitter.cpp` |
| Super Mario Bros. The Lost Levels (SMW Hack) | SNES | **nativo** (recorte de cabecera SMC doble de 1024 bytes y seguimiento de pila en `TXS`/`TCS`) | `snes/runtime/src/cartridge.cpp`, `studio/src/rom_detector.cpp` |
| Super Mario World | SNES | **intérprete** (`interpreter_only`, truco de pila en NMI; sin ROM en la biblioteca local, ver spec 003 §1.1) | `snes/games/smw/smw.toml` |
| SMB2(J) hack hM04 | NES | **intérprete** (`interpreter_only`, truco de pila/MMC3; ver [[recomp-interrupt-timing-limit]]) | memoria del proyecto |
| Pokémon Esmeralda | GBA | **intérprete** (`interpreter_only`, la cadena de llamadas del mixer m4a nunca retorna) | `studio/src/pipeline.cpp` `requires_interpreter_only()` |
| Pokémon Mundo Misterioso: Equipo de Rescate Rojo | GBA | **intérprete** (`interpreter_only`, bucles continuos de audio/eventos) | `studio/src/pipeline.cpp` `requires_interpreter_only()` |

Regla de oro (spec 003 §3.5): un juego solo cuenta como "recompilado" si
`report.toml` trae `interpreter_only = false` y 0 fallbacks observados. GB y
GBA **aún no tienen** `--verify` lock-step (solo NES y SNES lo tienen) y no
hay configs de GB/GBC/GBA committeados todavía — tareas pendientes explícitas
de `specs/003-ci-e2e-verificacion/tasks.md` (Tarea 5, Tarea 6.3/6.4).

| ID | Milestone | Key acceptance criterion | Status |
|----|-----------|--------------------------|--------|
| M0 | Project skeleton, specs, agent guides | `cmake -S . -B build -G Ninja && cmake --build build` succeeds | done |
| N1 | 6502 interpreter + bus + NROM + iNES loader | nestest matches `nestest.log` | done |
| N2 | NES PPU (dot-based) + SDL3 frontend + input | SMB & DK playable in interpreter mode; blargg vbl/nmi + sprite-hit tests pass | done |
| N3 | `nesrecomp` + SMB/DK configs | SMB 1-1→1-2 with zero fallbacks; `--verify` no divergence | done |
| N4 | APU + mappers 1/2/3/4 | blargg `apu_test` passes; one game per mapper recompiled | done |
| N5 | Mappers extendidos (40, 99, 189) | Soporte de Mapper 40 (SMB2J con IRQ de 4096 ciclos y PRG en $6000), Mapper 99 (Vs. System) y Mapper 189 (SF2) | done |
| S1 | 65816 interpreter + LoROM/HiROM bus + DMA | SNES CPU test ROMs pass | done |
| S2 | S-PPU + HDMA + SPC700/DSP + frontend | SNES PPU Modes 0-7, HDMA, SPC700/S-DSP audio, SDL3 frontend | done |
| S3 | `snesrecomp` (M/X variants) + SMB SNES config | SMB SNES boot & gameplay with zero fallbacks | done |
| S4 | Mode 7, hi-res, HiROM coverage | HiROM + Mode 7 affine matrix/multiply, `smb_snes_recomp` native | done |
| M5 | DragonRecomp Studio (GUI + Auto-Pipeline + Packager) | Drag & Drop ImGui GUI, ROM auto-detection, auto-learning, portable ELF packager | done |
| M6 | Soporte Windows y Multiplataforma | Compilación nativa en Windows (MSVC/MinGW), empaquetado `.exe` / `run.bat`, bundles CMake independientes y `SDL3.dll` | done |
| M7 | Persistencia SRAM (.sav), Save States & Visuales | Guardado automático `.sav`, Save States (`DRST`, F5/F7, slots 0-9), corrección 4:3/10:9, F11 fullscreen, F9 CRT scanlines | done |
| M8 | Robustez de UI, fuentes Inter/Montserrat y pipelines | Tipografía escalable en alta definición, prevención de colisiones en tablas de controles, corrección de escapes TOML en Windows y empaquetado automático de DLLs | done |
| M9 | Empaquetado Portable GUI, Optimización Multinúcleo y Estabilidad SNES | Bundling obligatorio de SDL3.dll sin terminal (-mwindows), compilación C paralela en Studio, ampliación de cards/botones y soporte integral de jump engines JSL / intérprete SMW | done |
| M10 | Refinamiento UX Studio, CI Hosted y RTC GBA | Rediseño y ampliación del modal de exportación (840x560), desacoplamiento de exportación diferida ("Exportar Ahora"), centrado dinámico, CI automatizado (Spec 003), emulación Cart GPIO + RTC Seiko S-3511A y fallbacks automáticos para estabilidad en GBA | done |
| G1 | SM83 interpreter + bus + MBC1/3/5 + GB/GBC loader | Blargg `cpu_instrs.gb` passes in interpreter mode | done |
| G2 | GB/GBC PPU (scanline) + Palettes (DMG/CGB) + Joypad + SDL3 | Tetris & Pokemon Red/Blue playable in interpreter mode | done |
| G3 | `gbrecomp` tool + C99 Emitter + CFG Builder | Tetris & Pokemon Red/Blue recompiled native with 0 fallbacks | done |
| G4 | APU (4 ch) + MBC3 RTC + CGB Double Speed + HDMA | Pokemon Gold/Silver/Crystal boot, audio & RTC functional | done |
| G5 | DragonRecomp Studio integration for GB/GBC | Drag & drop `.gb`/`.gbc`, auto-pipeline, auto-repair, packager | done |
| A1 | ARM7TDMI interpreter (ARM + Thumb) + bus + BIOS HLE + saves | `gba_cpu_unit_tests` (ARM/Thumb, interworking, SWI, DMA, Timers, SRAM/Flash/EEPROM) pass | done |
| A2 | GBA PPU (Modes 0–5, affine, OAM sprites, blending) + SDL3 | `gba_ppu_unit_tests` pass; 240×160 RGBA32 & 3:2 aspect ratio | done |
| A3 | `gbarecomp` static recompiler (ARM/Thumb CFG + multi-case C99 emitter) | `gba_recomp_unit_tests` (veneers, `_call_via_rX`, switch tables, IWRAM, IRQ) pass | done |
| A4 | GBA DirectSound DMA FIFO + PSG APU + Save States (`DRST`) | `gba_apu_unit_tests` pass; 48 kHz stereo audio & `.sav` / F5/F7 states | done |
| A5 | DragonRecomp Studio integration for GBA | Drag & drop `.gba`, auto-pipeline, fallback learning, Linux/Windows packager | done |
| M11 | Rediseño Obsidian UI, Biblioteca Portable y Preferencias (Specs 004 y 005) | Biblioteca basada en portables con importador de recompilados y ejecución directa por SO, rediseño Obsidian UI (tema claro/oscuro + 4 acentos), preferencias persistentes (`settings.toml`, 2 núcleos y `-O1` por defecto, autoguardado `--save-state`), adaptación responsiva multi-resolución/HiDPI y fix de PPU GB | done |
| MS1 | Sega Master System / Game Gear Z80 interpreter + bus + mappers (Sega/Codemasters/Korean) | `sms_cpu_unit_tests` pass; Z80 completo (base/CB/ED/DD/FD/DDCB/FDCB), detección dual TMR SEGA + Codemasters, SRAM y save states `DRST` | done |
| MS2 | SMS/GG VDP Mode 4 + SN76489 PSG + SDL3 frontend | `sms_vdp_unit_tests` y `sms_psg_unit_tests` pass; 256×192/224 SMS y 160×144 GG con audio PSG estéreo 48 kHz | done |
| MS3 | `smsrecomp` static recompiler (Z80 CFG + C99 emitter) | `sms_recomp_unit_tests` pass; Sonic, Alex Kidd, Wonder Boy III, Micro Machines y WWF Wrestlemania verificados pixel-perfect en modo nativo `--recompiled` (incl. `PUSH+JP(HL)`) | done |
| MS4 | DragonRecomp Studio integration for SMS/GG + tarjeta de controles | Drag & drop `.sms`/`.gg`, auto-pipeline, empaquetador portable, tarjeta de controles post-recompilación para todas las consolas | done |
| P1 | PC Engine / TurboGrafx-16 HuC6280 interpreter + MPR MMU + HuCard loader | `pce_cpu_unit_tests` pass; HuC6280 completo (256 opcodes, bloque TII/TDD/TIN/TIA/TAI, TAM/TMA, BBR/BBS, ST0/ST1/ST2), 32 KiB WRAM, Timer, auto-detección de bit-swap US TG-16 y auto-sanación de dumps GoodTools `[b1]`/`[bad]` | done |
| P2 | HuC6270 VDC + HuC6260 VCE + 6-ch Wavetable PSG + SDL3 frontend | `pce_vdc_unit_tests` y `pce_psg_unit_tests` pass; timing RCR de 2 fases, sprites/BG 9-bit RGB, audio PSG 6 canales + DDA con acumulador de ciclos impares y pacing nanosegundo SDL3 | done |
| P3 | `pcerecomp` static recompiler (HuC6280 CFG + C99 emitter) | `pce_recomp_unit_tests` pass; 16/16 juegos comerciales + Bomberman (J) verificados en modo nativo `--recompiled` (incl. `PHA;PHA;RTS`, bucles `LDA/CMP/BEQ`, SuperGrafx WRAM/VDC1) | done |
| P4 | DragonRecomp Studio integration for PCE/TG-16 | Drag & drop `.pce`/`.sgx`, auto-pipeline, tarjeta de controles PCE (I/II/RUN/SELECT), empaquetador portable Windows/Linux | done |
| MD1 | Sega Genesis / Mega Drive M68000 interpreter + Z80 sound CPU + bus + cartridge (.md/.bin/.gen/.smd, SRAM, SSF2) | `md_cpu_unit_tests` pass; M68000 16 grupos de opcodes, 12 modos EA, excepciones, quirk `TAS` de Genesis, Z80 BUSREQ/RESET/bank y controles 3/6 botones | done |
| MD2 | Genesis VDP Mode 5 + YM2612 (OPN2) + SN76489 PSG + SDL3 frontend | `md_vdp_unit_tests` y `md_ym2612_unit_tests` pass; 320×224/256×224/240, Planos A/B/Window, 80 sprites, DMA 68K/Fill/Copy, FM 6-ch + DAC + PSG estéreo 48 kHz | done |
| MD3 | `mdrecomp` static recompiler (M68000 CFG + C99 emitter) | `md_recomp_unit_tests` pass; Sonic 1, Sonic 2, Streets of Rage 2, Aladdin, Gunstar Heroes, Shinobi III, Castlevania: Bloodlines y Gargoyles verificados (Sonic 1 title screen pixel-perfect en modo nativo `--recomp`) | done |
| MD4 | DragonRecomp Studio integration for Genesis / Mega Drive | Drag & drop `.md`/`.bin`/`.gen`/`.smd`, auto-pipeline, tarjeta de controles 6 botones, biblioteca, empaquetador portable y `package-studio.sh` | done |
