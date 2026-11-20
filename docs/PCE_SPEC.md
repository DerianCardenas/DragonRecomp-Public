# PC Engine / TurboGrafx-16 Architecture & Recompiler Specification (`PCE_SPEC.md`)

**Platform:** NEC PC Engine / TurboGrafx-16 (HuCard)  
**CPU:** Hudson Soft HuC6280 (7.15909 MHz / 1.78977 MHz, 65C02 superset + MMU)  
**Video:** HuC6270 VDC (64 KiB VRAM, 1 BG plane, 64 sprites) + HuC6260 VCE (512-color 9-bit GRB palette)  
**Audio:** HuC6280 6-channel Wavetable PSG (+ 2 noise channels, LFO, DDA)  
**Primary References:**
- Charles MacDonald, *PC Engine / TurboGrafx-16 Hardware Notes (`pcetech.txt`)*: https://www.chrismcovell.com/texts.html
- Archaic Pixels / PCE Dev Wiki: https://www.chibiakumas.com/6502/pcengine.php
- Mednafen / Mesen PCE Hardware Documentation: https://github.com/SourMesen/Mesen2

---

## 1. Memory Map & MMU (`MPR0`–`MPR7`)

The HuC6280 has a **16-bit logical address space** (64 KiB) mapped onto a **21-bit physical address space** (2 MiB, 256 × 8 KiB banks) via eight 8-bit Memory Page Registers (`MPR0`–`MPR7`):

| Logical Range | Page Register | Typical Physical Bank | Description |
| :--- | :---: | :---: | :--- |
| `$0000–$1FFF` | `MPR0` | `$FF` | Hardware I/O page (VDC, VCE, PSG, Timer, Joypad, IRQ) |
| `$2000–$3FFF` | `MPR1` | `$F8` | 8 KiB Work RAM (**Zero Page = `$2000–$20FF`**, **Stack = `$2100–$21FF`**) |
| `$4000–$5FFF` | `MPR2` | `$01..$7F` | HuCard ROM page |
| `$6000–$7FFF` | `MPR3` | `$01..$7F` | HuCard ROM page |
| `$8000–$9FFF` | `MPR4` | `$01..$7F` | HuCard ROM page |
| `$A000–$BFFF` | `MPR5` | `$01..$7F` | HuCard ROM page |
| `$C000–$DFFF` | `MPR6` | `$01..$7F` | HuCard ROM page |
| `$E000–$FFFF` | `MPR7` | `$00` | HuCard Boot Bank 0 (Interrupt & Reset Vectors `$FFF6–$FFFE`) |

### Physical 2 MiB Space (`phys_bank = mpr[addr >> 13]`, `off = addr & 0x1FFF`)
- **`$00–$7F`** (1 MiB): HuCard ROM.
  - Standard sizes (`128 KiB`, `256 KiB`, `512 KiB`, `768 KiB`, `1024 KiB`) mirror cleanly across `$00–$7F`.
  - **384 KiB HuCards** (e.g. *Bonk's Adventure*, *R-Type*): lower 256 KiB mapped in `$00–$3F` (mirrored), upper 128 KiB mapped in `$40–$7F` (mirrored).
  - **Street Fighter II' Mapper (2.5 MiB):** writes to `$1FF0–$1FF3` (within ROM space) select which 512 KiB bank (`0..3`) is mapped into physical banks `$40–$7F` while `$00–$3F` stays fixed to the first 512 KiB.
- **`$F7`**: 2 KiB Backup RAM (BRAM, battery-backed `.sav`).
- **`$F8–$FB`**: 8 KiB Work RAM (mirrored across `$F8–$FB`).
- **`$FF`**: Hardware I/O Page:
  - `$0000–$03FF`: HuC6270 VDC (`addr & 3`: `0` Status R / AR W, `2` Data Lo, `3` Data Hi)
  - `$0400–$07FF`: HuC6260 VCE (`addr & 7`: `0` Control W, `2/3` CTA W, `4/5` Data R/W)
  - `$0800–$0BFF`: PSG (`$0800–$0809` W)
  - `$0C00–$0FFF`: Timer (`$0C00` counter/reload, `$0C01` enable bit 0)
  - `$1000–$13FF`: Joypad I/O (`$1000` W: bit 0=`SEL`, bit 1=`CLR`; R: bits 0–3 active-low data, bit 6 region `1`=JP/`0`=US, bit 7=`1` no CD)
  - `$1400–$17FF`: Interrupt Controller (`$1402` mask R/W, `$1403` status R / TIQ ack W)

### ROM Image Detection
- **512-byte copier header:** stripped automatically when `file_size % 8192 == 512`.
- **US TurboGrafx-16 bit-reversed dumps:** auto-detected by inspecting reset vector (`$1FFE–$1FFF`) and entry opcodes; if bit-reversal produces a valid `$E000–$FFFF` reset vector and valid HuC6280 boot preamble (`SEI`/`CLD`/`CSH`/`LDX`/`LDA`), every ROM byte is un-reversed on load.

---

## 2. HuC6280 CPU Semantics

- **Registers:** `A`, `X`, `Y`, `S` (stack at `$2100 | S`), `P` (`N V T B D I Z C`), `PC`, `MPR[8]`, `speed_high` (`1` = 7.16 MHz, `0` = 1.79 MHz).
- **Zero Page / Stack offset:** Zero Page instructions access logical `$2000 | zp` (NOT `$0000 | zp`); stack instructions access logical `$2100 | S` (NOT `$0100 | S`).
- **Decimal Mode:** `ADC` and `SBC` execute full BCD arithmetic when `D = 1`; hardware interrupts clear `D = 0` and `T = 0`.
- **`SET` (`$F4`) & `T` Flag (`0x20`):** Sets `T = 1` for the immediately following instruction only. When `T = 1`, `ORA`, `AND`, `EOR`, and `ADC` read/modify/write Zero Page `$2000 | X` instead of `A`. Every non-`SET` instruction clears `T = 0`.
- **HuC6280 Extensions:**
  - `ST0` (`$03`), `ST1` (`$13`), `ST2` (`$23`): write immediate directly to VDC ports `$0000`, `$0002`, `$0003` (bypassing `MPR0`).
  - `TMA` (`$43`), `TAM` (`$53`): transfer between `A` and `MPR0..7` selected by bitmask.
  - `TII` (`$73`), `TDD` (`$C3`), `TIN` (`$D3`), `TIA` (`$E3`), `TAI` (`$F3`): 7-byte block memory transfers (`len == 0` transfers `65536` bytes; cycles = `17 + 6 * len`).
  - `TST` (`$83`, `$93`, `$A3`, `$B3`), `RMB0..7` (`$07..$77`), `SMB0..7` (`$87..$F7`), `BBR0..7` (`$0F..$7F`), `BBS0..7` (`$8F..$FF`).
  - `BSR` (`$44`), `SXY` (`$02`), `SAX` (`$22`), `SAY` (`$42`), `CLA` (`$62`), `CLX` (`$82`), `CLY` (`$C2`), `CSL` (`$54`), `CSH` (`$D4`).
  - Full **WDC 65C02** set: `BRA`, `PHX/PLX/PHY/PLY`, `STZ`, `TRB/TSB`, `INC A/DEC A`, `BIT` modes, `(zp)`, `JMP (abs,X)`.
- **Interrupt Vectors:** `$FFF6` IRQ2/BRK, `$FFF8` IRQ1 (VDC), `$FFFA` TIQ (Timer), `$FFFC` NMI, `$FFFE` RESET.
- **Timer:** 7-bit reloadable down-counter clocked every 1024 CPU high-speed cycles (~6.99 kHz); underflow triggers TIQ (acknowledged by writing `$1403`).

---

## 3. HuC6270 VDC & HuC6260 VCE

- **VRAM:** 32,768 × 16-bit words (64 KiB). Auto-increment (`+1`, `+32`, `+64`, `+128`) from `CR` bits 11–12.
- **Registers `$00–$13`:**
  - `$00` MAWR, `$01` MARR (pre-fetches `read_buffer` on hi-byte write), `$02` VRR/VWR
  - `$05` CR (IRQs + BG/Sprite enable + increment)
  - `$06` RCR (raster compare IRQ at `active_line + 64 == (rcr & 0x3FF)`)
  - `$07` BXR (10-bit BG X scroll), `$08` BYR (9-bit BG Y scroll, latched on write and incremented each active scanline)
  - `$09` MWR (BAT width 32/64/128 tiles × height 32/64 tiles)
  - `$0A` HSR, `$0B` HDR (`width = ((hdr & 0x7F) + 1) * 8`, clamped to 256..512), `$0C` VPR, `$0D` VDR (`height = (vdr & 0x1FF) + 1`, typically 224–242), `$0E` VCR
  - `$0F` DCR (DMA control + auto VRAM→SATB at VBlank)
  - `$10` SOUR, `$11` DESR, `$12` LENR (VRAM→VRAM DMA of `LENR + 1` words on hi-byte write)
  - `$13` DVSSR (schedules 256-word VRAM→SATB DMA at VBlank)
- **Background Rendering:** 8×8 planar tiles (16 words = 32 bytes: 8 words planes 0/1 + 8 words planes 2/3). BAT entry = bits 0–11 tile index (`word_addr = tile * 16`), bits 12–15 BG palette `0..15`.
- **Sprite Rendering:** 64 entries × 4 words in internal SATB:
  - Word 0: Y (`screen_y = (w0 & 0x3FF) - 64`)
  - Word 1: X (`screen_x = (w1 & 0x3FF) - 32`)
  - Word 2: Pattern address (`(w2 & 0x3FE) << 5` words in VRAM, 64 words per 16×16 cell: 16 words per bitplane 0..3)
  - Word 3: bits 0–3 palette (`0..15`), bit 7 SPBG (`1`=front, `0`=behind BG), bit 8 CGX (`0`=16px, `1`=32px), bit 11 X-flip, bits 12–13 CGY (`00`=16px, `01`=32px, `11`=64px), bit 15 Y-flip.
  - Max 16 × 16-px sprite cells per scanline (sets Overflow flag), Sprite 0 non-transparent overlap sets Collision flag.
- **VCE Palette:** 512 × 9-bit entries (`GGGRRRBBB`: G=bits 8–6, R=bits 5–3, B=bits 2–0), expanded to `SDL_PIXELFORMAT_RGBA32` (`0xFF000000 | (B << 16) | (G << 8) | R`).
- **Frame Timing:** 262 scanlines/frame, 455 CPU cycles/scanline (~119,210 cycles/frame at 7.15909 MHz, ~60.0 Hz).

---

## 4. HuC6280 PSG Audio

- 6 channels clocked at 3.579545 MHz, downsampled to 48,000 Hz stereo `int16_t`:
  - `$0800`: Channel select (`0..5`)
  - `$0801`: Global stereo balance (L: bits 7–4, R: bits 3–0)
  - `$0802/$0803`: 12-bit channel frequency divider
  - `$0804`: Channel control (bit 7=enable, bit 6=DDA direct mode, bits 4–0=volume `0..31`; write with bits 7–6=`01` resets waveform index)
  - `$0805`: Channel stereo balance (L: bits 7–4, R: bits 3–0)
  - `$0806`: 5-bit waveform sample write (writes to 32-entry table or directly to DDA output)
  - `$0807`: Noise control on ch 4–5 (bit 7=enable, bits 4–0=frequency)
  - `$0808/$0809`: LFO frequency & control (ch 1 modulates ch 0 frequency)

---

## 5. Static Recompiler (`pcerecomp`)

- **Function identity:** `(phys_bank, logical_addr)` → C99 symbol `func_BB_XXXX(PceCpuState* cpu)`.
- **CFG discovery:**
  - Seeds from hardware vectors in bank `$00` (`$FFFE` RESET, `$FFF8` IRQ1, `$FFFA` TIQ, `$FFF6` IRQ2, `$FFFC` NMI) + TOML `[[entry_points]]` + `[[jump_tables]]`.
  - Tracks constant `LDA #imm; TAM #mask` page mappings within functions to discover cross-bank `JSR`/`JMP` targets automatically.
- **C99 Emission (`-std=c99 -Wall -Wextra -Werror`):**
  - Native C99 for hot 65C02/HuC6280 instructions, block transfers (`TII`/`TDD`/`TIN`/`TIA`/`TAI`), VDC port writes (`ST0`/`ST1`/`ST2`), MMU ops (`TAM`/`TMA`), bit ops (`RMB`/`SMB`/`BBR`/`BBS`/`TST`).
  - Cycle-accurate runtime hooks: `pce_bus_read`, `pce_bus_write`, `pce_st0`, `pce_st1`, `pce_st2`, `pce_tam`, `pce_tma`, `pce_runtime_poll`, `pce_runtime_dispatch`, `pce_runtime_step`.
  - Frame callback inside `Bus::sync_hardware` cleanly drives headless and SDL3 presentation without blocking inside recompiled main loops, plus non-local return & stack-pointer resume for jump tables (`JMP (abs,X)` / `RTS` tricks).
- **Controls:** D-Pad (`Arrows`/`WASD`), Button I (`Z`/`J`), Button II (`X`/`K`), Run (`Enter`), Select (`Right Shift`/`Tab`/`Backspace`), Save/Load States (`F5`/`F7`, slots `0–9`), Aspect 4:3 (`F8`), CRT (`F9`), Fullscreen (`F11`), Quit (`Esc`).
