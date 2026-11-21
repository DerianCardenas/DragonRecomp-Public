# Sega Genesis / Mega Drive Specification (`GENESIS_SPEC.md`)

**Platform:** Sega Genesis / Mega Drive (`mdrecomp`, `md/`)  
**Phase:** Roadmap Phase 2.1 (`feat/genesis`)  
**Main CPU:** Motorola 68000 (Big-Endian 16/32-bit, 7.670454 MHz NTSC / 7.600489 MHz PAL)  
**Sound CPU:** Zilog Z80A (3.579545 MHz NTSC / 3.546894 MHz PAL)  
**Video:** Sega 315-5313 VDP (Mode 5, 64 KiB VRAM, 64-color CRAM 9-bit RGB, 40-word VSRAM)  
**Audio:** Yamaha YM2612 (OPN2, 6-ch 4-op FM + Ch6 8-bit DAC) + TI SN76489 PSG (reused from `sms/`)  

**Primary References:**
- Motorola M68000 Family Programmer's Reference Manual (M68000PM/AD)
- Plutiedev Mega Drive Hardware Documentation: https://plutiedev.com/
- Charles MacDonald's Sega Genesis VDP Documentation: https://segaretro.org/Sega_Mega_Drive/VDP_registers
- YM2612 Hardware Notes (SpritesMind / Nemesis): https://www.smspower.org/maxim/Documents/YM2612
- DragonRecomp Core Specs: [SPEC.md](SPEC.md), [ROADMAP.md](ROADMAP.md), [SMS_SPEC.md](SMS_SPEC.md)

---

## 1. Memory Map (Motorola 68000 — 24-bit Address Bus)

| Address Range | Size | Description |
| :--- | :---: | :--- |
| `$000000–$3FFFFF` | 4 MiB | Cartridge ROM (flat up to 4 MiB; SSF2 8×512 KiB banking for >4 MiB; optional SRAM at `$200000–$3FFFFF`) |
| `$A00000–$A0FFFF` | 64 KiB | Z80 Address Space (accessible by 68K when Z80 `BUSREQ` is asserted and Z80 is not in `RESET`) |
| `$A00000–$A01FFF` | 8 KiB | Z80 Sound RAM (mirrored at `$A02000–$A03FFF`) |
| `$A04000–$A04003` | 4 B | YM2612 FM Synthesizer ports (Part I addr/data, Part II addr/data; mirrored through `$A05FFF`) |
| `$A10000–$A1001F` | 32 B | Hardware Version (`$A10001`), Controller 1/2/Ext Data (`$A10003`/`$05`/`$07`) & Control (`$A10009`/`$0B`/`$0D`) |
| `$A11100` | 2 B | Z80 Bus Request (`BUSREQ`: write `$0100` to request, `$0000` to release; read bit 8 = 0 when granted) |
| `$A11200` | 2 B | Z80 Reset (`RESET`: write `$0000` to assert reset, `$0100` to release reset) |
| `$A130F0–$A130FF` | 16 B | Cartridge SRAM enable (`$A130F1` bit 0) & SSF2 512 KiB ROM bank registers (`$A130F3`..`$A130FF`) |
| `$A14000–$A14003` | 4 B | TMSS Security Register (`"SEGA"` write accepted silently) |
| `$C00000–$C0001F` | 32 B | VDP Data (`$C00000`/`$02`), Control (`$C00004`/`$06`), HV Counter (`$C00008`), PSG (`$C00011`/`$13`/`$15`/`$17`) |
| `$E00000–$FFFFFF` | 64 KiB | Main Work RAM (64 KiB mirrored across `$E00000–$FFFFFF`; canonical `$FF0000–$FFFFFF`) |

### Z80 Sound CPU Address Map (16-bit)
- `$0000–$1FFF`: 8 KiB Z80 Sound RAM (mirrored `$2000–$3FFF`).
- `$4000–$4003`: YM2612 Part I/II Address & Data ports (mirrored `$4000–$5FFF`).
- `$6000–$60FF`: 9-bit serial 68K bank register (bit 0 shifted into MSB of `bank_reg`; maps 32 KiB window `(bank_reg << 15)`).
- `$7F11`: SN76489 PSG write port.
- `$8000–$FFFF`: 32 KiB banked window into 68K address space (`(bank_reg << 15) | (addr & 0x7FFF)`).

---

## 2. Motorola 68000 CPU & Static Recompiler (`mdrecomp`)

### 2.1 CPU State (`MdCpuState`, C99-compatible)
- `uint32_t d[8]`: Data registers `D0–D7`.
- `uint32_t a[8]`: Address registers `A0–A7` (`a[7]` is active stack pointer).
- `uint32_t usp`, `ssp`: User and Supervisor stack pointers swapped automatically on `SR.S` (`$2000`) changes.
- `uint32_t pc`: 24-bit effective program counter (always even).
- `uint16_t sr`: Status Register (`T S - - I2 I1 I0 - - - X N Z V C`).
- `uint64_t cycles`, `next_event`: Master 68K cycle counter & runtime poll threshold.
- `uint8_t stopped`: Active `STOP` state (woken by interrupt of higher priority than `SR` mask).

### 2.2 Genesis Hardware Quirks
- **`TAS` instruction (`0x4AC0–0x4AFF`)**: On register `Dn`, sets bit 7 of `Dn`. On memory `<ea>`, reads byte and updates `N`/`Z` flags (`V=0, C=0`), but **does NOT write bit 7 back to memory** (matching real Genesis Model 1/2 bus arbitration required by *Gargoyles*).
- **Byte-order**: All 68K 16-bit/32-bit ROM/RAM accesses are **Big-Endian**.
- **Exceptions**: Vector table at `$000000–$0000FF`. Priority interrupts: Level 6 = V-Int (`$0078`), Level 4 = H-Int (`$0070`), Level 2 = Ext-Int (`$0068`). Software exceptions: `TRAP #0–#15` (`$0080–$00BC`), `Line-A` (`$0028`), `Line-F` (`$002C`), `Zero Divide` (`$0014`), `CHK` (`$0018`), `TRAPV` (`$001C`), `Illegal` (`$0010`).

### 2.3 Static Recompiler (`mdrecomp`)
- **Entry points**: Initial `PC` (`$000004`), `V-Int` (`$000078`), `H-Int` (`$000070`), `Ext-Int` (`$000068`), non-trivial `TRAP #0–#15`, `Line-A`, `Line-F` vectors, plus TOML `extra_entries` / `jump_tables` (auto-learned by Studio).
- **Jump table discovery**: Detects PC-relative jump table idioms (`MOVE.W (d8,PC,Dn),Dn` + `JMP (d8,PC,Dn)` and `JMP (d8,PC,Dn)` directly).
- **Hybrid execution**: Indirect `JMP`/`JSR` and Work RAM (`$FF0000–$FFFFFF`) code dispatch through `md_runtime_dispatch()` with transparent fallback to the cycle-accurate M68000 interpreter and fallback logging.
- **Generated C99 symbols**: `func_XXXXXX` (24-bit hex address), split across `recomp_funcs_0.c`, `recomp_funcs_1.c`, ... for parallel compilation.

---

## 3. VDP Mode 5 (`md/runtime/src/vdp.cpp`)

- **Memory**: 64 KiB VRAM, 64×16-bit CRAM (`0000 BBB0 GGG0 RRR0` → 9-bit RGB), 40×16-bit VSRAM.
- **Resolutions**: H40 (`320×224`, 40×28 tiles), H32 (`256×224`, 32×28 tiles), V30 PAL (`320×240` / `256×240`).
- **Timing (NTSC)**: 262 scanlines/frame, 488 68K cycles/scanline (~127,856 cycles/frame @ 59.92 Hz). Active lines `0..223`, VBlank `224..261`.
- **Layers & Priority (back → front)**:
  1. Backdrop (`Reg $07` palette/color)
  2. Plane B (low priority)
  3. Plane A / Window (low priority)
  4. Sprites (low priority)
  5. Plane B (high priority)
  6. Plane A / Window (high priority)
  7. Sprites (high priority)
- **Shadow / Highlight (`Reg $0C` bit 3)**: Normal/shadow/highlight brightness modulation including sprite palette 3 indices `$E` (highlight) and `$F` (shadow).
- **DMA**:
  - **68K → VDP** (VRAM/CRAM/VSRAM) triggered on 2nd Control Port word when `Reg $01` bit 4 (DMA enable) and command bit 5 are set and `Reg $17` bit 7 = 0.
  - **VRAM Fill** (`Reg $17` bits 7–6 = `10`): triggered on next Data Port write; writes low byte then fills `length` bytes with high byte.
  - **VRAM Copy** (`Reg $17` bits 7–6 = `11`): copies `length` bytes within VRAM.

---

## 4. Audio Subsystem

- **YM2612 (OPN2)**: 6 FM channels × 4 operators, 8 algorithms, phase generator (F-Number, Block, Detune, Multiple), envelope generator (AR/D1R/D2R/RR/SL/KS/SSG-EG), LFO (AM/PM), Channel 3 special mode, Timers A & B, Channel 6 8-bit unsigned DAC (`Reg $2B` bit 7, data in `$2A`), stereo L/R panning (`Reg $B4–$B6`). Resampled to 48 kHz stereo PCM.
- **SN76489 PSG**: Reuses `dragon::sms::Psg` (3 square channels + 1 noise channel, 3.579545 MHz) mixed into the 48 kHz stereo output stream.

---

## 5. Cartridge Formats & Mappers

- **Flat binary**: `.md`, `.bin`, `.gen` (big-endian, `"SEGA"` at `$0100`).
- **Super Magic Drive (`.smd`)**: 512-byte header (`header[8] == 0xAA && header[9] == 0xBB`) followed by 16 KiB interleaved blocks (first 8 KiB = odd bytes, second 8 KiB = even bytes); automatically de-interleaved on load.
- **SRAM**: Parsed from header `$01B0–$01BF` (`"RA"`), persisted as `.sav`, gated by `$A130F1` when ROM > 2 MiB.
- **SSF2 Mapper**: 8 × 512 KiB switchable ROM banks (`$A130F3`..`$A130FF`) for ROMs > 4 MiB (e.g., *Super Street Fighter II*).

---

## 6. Milestones

| ID | Milestone | Acceptance Criteria |
| :--- | :--- | :--- |
| **MD1** | M68000 interpreter + Z80 sound CPU + Bus + Cartridge (.md/.bin/.gen/.smd, SRAM, SSF2) + 3/6-btn input | `md_cpu_unit_tests` pass (ALU, flags, EA modes, exceptions, `TAS` quirk, Z80 bank/busreq) |
| **MD2** | VDP Mode 5 + YM2612 (OPN2) + SN76489 PSG + SDL3 frontend + Save States (`DRST`) | `md_vdp_unit_tests` & `md_ym2612_unit_tests` pass; commercial ROMs boot & render cleanly |
| **MD3** | `mdrecomp` static recompiler (M68K CFG + jump tables + C99 emitter) | `md_recomp_unit_tests` pass; commercial Genesis games boot & run in native `--recomp` mode |
| **MD4** | DragonRecomp Studio integration (detector, auto-pipeline, packager, library, controls) | Drag & drop `.md`/`.bin`/`.gen`/`.smd` in Studio; 100% CTest pass, 0 warnings |
