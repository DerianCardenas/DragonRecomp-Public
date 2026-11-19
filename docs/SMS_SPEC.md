# Sega Master System & Game Gear Specification (Zilog Z80 + VDP Mode 4 + SN76489 PSG)

See [SPEC.md](SPEC.md) for the general DragonRecomp pipeline and conventions.  
Primary authoritative references:
- SMS Power! Development Documentation: <https://www.smspower.org/Development/Documents>
- Charles MacDonald's Sega Master System VDP documentation (`msvdp.txt`): <https://www.smspower.org/uploads/Development/msvdp-20021112.txt>
- SMS Power! SN76489 PSG specification: <https://www.smspower.org/Development/SN76489>
- SMS Power! Mappers & Memory Map: <https://www.smspower.org/Development/Mappers>
- SMS Power! Game Gear Hardware Differences: <https://www.smspower.org/Development/GameGear>

---

## 1. Overview & Hardware Modes

The Sega 8-bit subsystem (`sms/`) unifies support for both the **Sega Master System (SMS)** and the **Sega Game Gear (GG)** in a single static recompiler (`smsrecomp`) and native C++20 runtime (`dragon-sms-runtime`). The Z80 CPU core is also designed to be cleanly reusable as the sound coprocessor for Sega Genesis / Mega Drive in Phase 2.

| Feature | Sega Master System (SMS) | Sega Game Gear (GG) |
| :--- | :--- | :--- |
| **CPU Core** | Zilog Z80A @ 3.579545 MHz (NTSC) / 3.546893 MHz (PAL) | Zilog Z80A @ 3.579545 MHz (NTSC) |
| **Address Space** | 64 KiB memory + 256 I/O ports | 64 KiB memory + 256 I/O ports (ports `$00–$06` extended) |
| **Work RAM** | 8 KiB (`$C000–$DFFF`, mirrored at `$E000–$FFFF`) | 8 KiB (`$C000–$DFFF`, mirrored at `$E000–$FFFF`) |
| **VRAM** | 16 KiB (via VDP ports `$BE`/`$BF`) | 16 KiB (via VDP ports `$BE`/`$BF`) |
| **CRAM (Palette)** | 32 bytes (32 colors, 6-bit `--BBGGRR`, 64 total colors) | 64 bytes (32 colors, 12-bit `----BBBB GGGGRRRR`, 4,096 total colors) |
| **Visible Screen** | 256×192 (or 256×224 in Mode 4 224-line) | 160×144 centered window (`x ∈ [48, 207]`, `y ∈ [24, 167]`) |
| **Audio** | SN76489 PSG (mono, 3 tone + 1 noise) | SN76489 PSG (stereo panning via port `$06`) |
| **Pause / Start** | Pause button triggers Z80 **NMI** (`$0066`) | Start button polled via port `$00` bit 7 (active low, no NMI) |

### 1.1 ROM Header & Platform Detection

1. **Optional 512-byte copier header:** If `file_size % 16384 == 512`, strip the leading 512 bytes before loading ROM banks.
2. **Sega `"TMR SEGA"` header:** Scanned at offsets `$7FF0` (32 KiB+ ROMs), `$3FF0` (16 KiB), and `$1FF0` (8 KiB):
   - `+$00..$07`: `"TMR SEGA"` ASCII magic.
   - `+$0A..$0B`: 16-bit little-endian checksum.
   - `+$0C..$0E`: Product code (BCD) + version nibble.
   - `+$0F` upper nibble (Region / System):
     - `3` = SMS Japan, `4` = SMS Export
     - `5` = GG Japan, `6` = GG Export, `7` = GG International
3. **File extension fallback:** `.gg` selects Game Gear mode; `.sms` selects Master System mode.
4. **Mapper auto-detection:**
   - ROM ≤ 48 KiB: Flat / Sega compatible (no paging needed).
   - Default (> 48 KiB): **Sega Mapper** (`$FFFC–$FFFF`).
   - **Codemasters Mapper** detected via Codemasters checksum / header heuristic or TOML override (`mapper = "codemasters"`).
   - **Korean Mapper** selectable via TOML override (`mapper = "korean"`).

---

## 2. CPU: Zilog Z80A

### 2.1 Register Architecture

The Z80 extends the 8080/SM83 register model with shadow registers, two 16-bit index registers (`IX`, `IY`), and interrupt/refresh registers (`I`, `R`):

- **Main set:** `A, F, B, C, D, E, H, L` (pairs `AF, BC, DE, HL`).
- **Shadow (alternate) set:** `A', F', B', C', D', E', H', L'` (swapped via `EX AF, AF'` and `EXX`).
- **Index & pointer registers:** `IX`, `IY`, `SP`, `PC`.
- **Special registers:** `I` (interrupt vector high byte), `R` (memory refresh counter, lower 7 bits increment per M1 opcode fetch).
- **Flags (`F`):**
  - Bit 7: `S` (Sign)
  - Bit 6: `Z` (Zero)
  - Bit 5: `Y` (Undocumented copy of bit 5)
  - Bit 4: `H` (Half-Carry)
  - Bit 3: `X` (Undocumented copy of bit 3)
  - Bit 2: `P/V` (Parity for logical/rotate ops; signed 2's-complement Overflow for arithmetic ops; `BC != 0` for block ops; `IFF2` for `LD A,I` / `LD A,R`)
  - Bit 1: `N` (Subtract flag for `DAA`)
  - Bit 0: `C` (Carry)

```c
/* Shared CpuState layout in sms/runtime/include/sms/cpu.h */
typedef struct SmsCpuState {
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;

    /* Shadow register bank */
    uint8_t a2, f2;
    uint8_t b2, c2;
    uint8_t d2, e2;
    uint8_t h2, l2;

    uint16_t ix, iy;
    uint16_t sp, pc;
    uint8_t i, r;

    /* Interrupt & execution control */
    uint8_t iff1;      /* Maskable interrupt enable */
    uint8_t iff2;      /* Saved IFF1 during NMI */
    uint8_t im;        /* Interrupt mode: 0, 1, or 2 */
    uint8_t ei_delay;  /* EI takes effect after the following instruction */
    uint8_t halted;    /* HALT state waiting for IRQ/NMI */
    uint8_t nmi_pending;

    uint64_t cycles;   /* Elapsed Z80 T-cycles */
} SmsCpuState;
```

### 2.2 Instruction Tables & Prefixes

All documented instructions plus commonly used undocumented instructions (such as `IXH`/`IXL`/`IYH`/`IYL` half-index operations and `SLL`, frequently emitted by SMS games and SDCC):
- **Unprefixed (`0x00–0xFF`)**: 8/16-bit loads, ALU, jumps, calls, returns, `RST`, `IN A,(n)`, `OUT (n),A`, `EX`/`EXX`, `DJNZ`, `DAA`, `HALT`, `DI`, `EI`.
- **`CB` prefix (`0x00–0xFF`)**: `RLC/RRC/RL/RR/SLA/SRA/SLL/SRL`, `BIT/RES/SET` on registers and `(HL)`.
- **`ED` prefix**: Block transfers (`LDI/LDIR/LDD/LDDR`), block compares (`CPI/CPIR/CPD/CPDR`), block I/O (`INI/INIR/IND/INDR`, `OUTI/OTIR/OUTD/OTDR`), 16-bit `ADC/SBC HL, rr`, `NEG`, `RETN/RETI`, `IM 0/1/2`, `LD I,A`, `LD R,A`, `LD A,I`, `LD A,R`, `RRD`, `RLD`, `IN r,(C)`, `OUT (C),r`, `LD (nn),rr`, `LD rr,(nn)`.
- **`DD` (`IX`) & `FD` (`IY`) prefixes**: Replace `HL` / `(HL)` with `IX` / `(IX+d)` or `IY` / `(IY+d)`, plus `IXH/IXL/IYH/IYL`.
- **`DDCB` / `FDCB` prefixes**: Indexed bit/shift operations with displacement byte **before** opcode byte (`DD CB <d> <op>`).

Single opcode length/cycle table in `sms/runtime/include/sms/opcodes.h`, shared across disassembler, recompiler, interpreter, and unit tests.

### 2.3 Interrupts & Reset State

- **Reset:** `PC = 0x0000`, `SP = 0xDFF0` (standard post-BIOS state so games work cleanly without requiring a copyrighted SMS BIOS), `IFF1 = IFF2 = 0`, `IM = 1`, `I = 0`, `R = 0`.
- **`EI` Delay:** `EI` sets `ei_delay = 1`; `iff1 = iff2 = 1` becomes active only **after** the immediately following instruction executes (`EI; RET` / `EI; RETI` must complete before servicing a pending IRQ).
- **Maskable IRQ (VDP):** Level-triggered! As long as the VDP IRQ line is asserted (Frame INT pending with `IE0=1`, or Line INT pending with `IE1=1`) and `iff1 == 1` and `ei_delay == 0`:
  - Exit `halted`, set `iff1 = iff2 = 0`, push `PC`, add 13 T-cycles (IM 1) and jump to `$0038` (IM 1) or vector read from `(I << 8) | 0xFF` (IM 2).
- **NMI (SMS Pause button):** Edge-triggered! Exit `halted`, `iff2 = iff1`, `iff1 = 0`, push `PC`, add 11 T-cycles, jump to `$0066`.

---

## 3. Memory Map, Mappers & I/O Ports

### 3.1 Memory Map (`$0000–$FFFF`)

```
$0000 - $03FF : Fixed first 1 KiB of ROM (Sega Mapper only; vectors stay mapped)
$0400 - $3FFF : ROM Slot 0 (16 KiB bank via $FFFD, default bank 0)
$4000 - $7FFF : ROM Slot 1 (16 KiB bank via $FFFE, default bank 1)
$8000 - $BFFF : ROM Slot 2 (16 KiB bank via $FFFF, default bank 2) OR 16 KiB Cart SRAM
$C000 - $DFFF : 8 KiB System Work RAM
$E000 - $FFFF : Mirror of Work RAM ($C000-$DFFF); $FFFC-$FFFF also latch Sega Mapper
```

### 3.2 Supported Mappers

1. **Sega Mapper (Standard):**
   - `$FFFC` (RAM/Control):
     - Bit 3: `1` = Map Cartridge SRAM into Slot 2 (`$8000–$BFFF`), `0` = Map ROM.
     - Bit 2: Selects SRAM bank 0 or 1 (total 32 KiB battery-backed SRAM, persisted to `.sav`).
     - Bits 1..0: ROM bank shift (0/1/2/3 → bank page shift, usually 0).
   - `$FFFD`: Slot 0 ROM bank (`bank % num_banks`, `$0000–$03FF` remains fixed to ROM offset 0).
   - `$FFFE`: Slot 1 ROM bank (`bank % num_banks`).
   - `$FFFF`: Slot 2 ROM bank (`bank % num_banks`).
   - Writes to `$FFFC–$FFFF` also write to the underlying Work RAM mirror (`$DFFC–$DFFF`).
2. **Codemasters Mapper:**
   - Writes to `$0000` select Slot 0 bank (no fixed 1 KiB!).
   - Writes to `$4000` select Slot 1 bank (bit 7 enables 8 KiB onboard RAM at `$A000–$BFFF` for Ernie Els Golf).
   - Writes to `$8000` select Slot 2 bank.
3. **Korean Mapper:**
   - Writes to `$A000` select Slot 2 bank (`$8000–$BFFF`); Slots 0 and 1 stay fixed at banks 0 and 1.

### 3.3 I/O Port Decoding

On SMS, the Z80 8-bit port address is decoded by bits **7, 6, and 0**:

| Port Range | Parity (A0) | Read | Write |
| :--- | :--- | :--- | :--- |
| **`$00–$06`** *(GG only)* | Exact | `$00`: Start (bit 7, active low) & Region (bit 6)<br>`$01–$05`: Serial/Link regs | `$01–$05`: Serial/Link regs<br>`$06`: PSG Stereo Panning (`L[7:4]`, `R[3:0]`) |
| **`$00–$3F`** | Even (`$3E`) | Open bus (`$FF`) | Memory Control (latched) |
| **`$00–$3F`** | Odd (`$3F`) | Open bus (`$FF`) | I/O Port Control (TH/TR pins for Export region check) |
| **`$40–$7F`** | Even (`$7E`) | VDP **V-Counter** | **SN76489 PSG** data write |
| **`$40–$7F`** | Odd (`$7F`) | VDP **H-Counter** | **SN76489 PSG** data write |
| **`$80–$BF`** | Even (`$BE`) | **VDP Data** (buffered read + addr++) | **VDP Data** (VRAM/CRAM write + addr++) |
| **`$80–$BF`** | Odd (`$BF`) | **VDP Status** (clears INT flags & latch) | **VDP Control** (2-byte addr/code or register write) |
| **`$C0–$FF`** | Even (`$DC`) | **Joypad Port A** (P1 dirs/B1/B2 + P2 Up/Down, active low) | Ignored |
| **`$C0–$FF`** | Odd (`$DD`) | **Joypad Port B** (P2 Left/Right/B1/B2 + Reset + TH pins, active low) | Ignored |

---

## 4. Video: VDP (Mode 4)

### 4.1 Timing
- **NTSC:** 262 scanlines/frame, 228 Z80 T-cycles/scanline (59,736 T-cycles/frame ≈ 59.92 Hz).
- **Active display:** Lines `0..191` (standard 192-line mode) or `0..223` (224-line SMS2 mode).
- **V-Counter (NTSC 192-line):** Counts `0x00..0xDA` (lines 0–218), then jumps to `0xD5..0xFF` (lines 219–261).

### 4.2 Control Port (`$BF`) & Data Port (`$BE`)
- **Command latch (`second_byte`):**
  - First write to `$BF`: latches low byte (`addr_low`) and immediately updates low 8 bits of `vdp_addr`.
  - Second write to `$BF`: updates high 6 bits of `vdp_addr` (`val & 0x3F`) and 2-bit command `code = (val >> 6) & 0x03`:
    - `code == 0`: VRAM Read — immediately pre-fetches `read_buffer = vram[vdp_addr]` and increments `vdp_addr`.
    - `code == 1`: VRAM Write.
    - `code == 2`: VDP Register Write — `regs[val & 0x0F] = addr_low`.
    - `code == 3`: CRAM Write.
  - Reading `$BF` (Status) or reading/writing `$BE` (Data) resets `second_byte = false`.
- **CRAM Writes:**
  - **SMS:** `cram[vdp_addr & 0x1F] = val`; 6-bit RGB (`--BBGGRR`) expanded to 8-bit (`c * 85`).
  - **Game Gear:** Even address (`(vdp_addr & 1) == 0`) stores `val` into `cram_latch` without modifying CRAM; odd address commits `cram[(vdp_addr & 0x3E)] = cram_latch` (`GGGGRRRR`) and `cram[(vdp_addr & 0x3E) + 1] = val & 0x0F` (`----BBBB`), expanded to 8-bit (`c * 17`).

### 4.3 Key VDP Registers
- **Reg 0 (`$00`):** Bit 7 = Lock vertical scroll on right 8 columns (`col >= 24`), Bit 6 = Lock horizontal scroll on top 2 rows (`y < 16`), Bit 5 = Mask column 0 (`x < 8`) with backdrop color, Bit 4 = **Line Interrupt Enable (`IE1`)**, Bit 3 = Shift sprites left by 8 px (`EC`), Bit 2 = Mode 4 select (`M4`), Bit 1 = `M2`.
- **Reg 1 (`$01`):** Bit 6 = Display Enable (`BLK`), Bit 5 = **Frame Interrupt Enable (`IE0`)**, Bit 4 = `M1` (224-line mode when `M4=1, M2=1, M1=1`), Bit 1 = 8×16 sprites, Bit 0 = Zoomed sprites (2×).
- **Reg 2 (`$02`):** Name Table base address (`(reg2 & 0x0E) << 10` in 192-line mode).
- **Reg 5 (`$05`):** Sprite Attribute Table (SAT) base address (`(reg5 & 0x7E) << 7`).
- **Reg 6 (`$06`):** Sprite Pattern Generator base (`(reg6 & 0x04) << 11` → `$0000` or `$2000`).
- **Reg 7 (`$07`):** Backdrop / overscan color index (`16 + (reg7 & 0x0F)`).
- **Reg 8 (`$08`):** Background X scroll (subtracted from pixel X: tiles scroll **right** as Reg 8 increases).
- **Reg 9 (`$09`):** Background Y scroll (latched once per frame at start of active display; wraps at 224 in 192-line mode, 256 in 224-line mode).
- **Reg 10 (`$0A`):** Line Interrupt Counter reload value.

### 4.4 Interrupt Lifecycle
- **Frame Interrupt:** Set at start of VBlank (scanline 192 in 192-line mode, 224 in 224-line mode) → sets status bit 7 (`0x80`).
- **Line Interrupt:** Internal `line_counter` decrements on scanlines `0..active_height` (inclusive). On underflow (`0xFF`), reloads from `Reg 10` and sets `line_irq_pending = true`. On all other scanlines (`> active_height`), reloads continuously from `Reg 10`.
- **IRQ Line:** Asserted to Z80 iff `((status & 0x80) && (reg1 & 0x20)) || (line_irq_pending && (reg0 & 0x10))`. Cleared by reading VDP Status (`$BF`).

### 4.5 Rendering Pipeline (Scanline-Accurate)
- Output buffer: `256 × 224` RGBA32 (`uint32_t`), with viewport rect (`256×192`, `256×224`, or GG `160×144` cropped from `(48, 24)`).
- Background tiles: 4-bit planar (4 bitplanes interleaved per row of 8 pixels = 32 bytes/tile). Each nametable entry is 16-bit little-endian: `bits 0..8` = tile index (0–511), `bit 9` = H-flip, `bit 10` = V-flip, `bit 11` = palette select (0 = CRAM `0..15`, 1 = CRAM `16..31`), `bit 12` = priority over sprites (only for non-zero BG pixels).
- Sprites: Up to 64 in SAT (`Y` at `SAT+i`; `Y == 0xD0` stops scan in 192-line mode; displayed at `Y + 1`). Up to 8 sprites per scanline; 9th sprite sets status bit 6 (Overflow). Overlapping opaque sprite pixels set status bit 5 (Collision).

---

## 5. Audio: SN76489 PSG

- Clock input: `CPU_CLK / 16` (`223,721.56 Hz` NTSC).
- **3 Tone Channels (`0, 1, 2`):** 10-bit countdown period (`0` or `1` outputs constant high DC), toggling output polarity `+1 / -1` on reload.
- **1 Noise Channel (`3`):** 16-bit LFSR initialized to `0x8000`:
  - Feedback bit = parity of `(lfsr & 0x0009)` in white noise mode (`NF = 1`), or `lfsr & 1` in periodic mode (`NF = 0`).
  - Shift rate: `0` → 16, `1` → 32, `2` → 64, `3` → Tone 2 period.
- **Volume Table:** 4-bit attenuation per channel (`0` = max, `1..14` = `-2 dB` steps, `15` = silent `0.0`).
- **Game Gear Stereo (`$06`):** Independent Left/Right channel enable mask; SMS defaults to `0xFF` (centered mono).
- Resampled to 48 kHz 16-bit stereo PCM for SDL3 audio stream.

---

## 6. Static Recompiler (`smsrecomp`) & Runtime Architecture

### 6.1 Directory Layout

```
sms/
  CMakeLists.txt
  runtime/
    include/sms/
      cpu.h            # SmsCpuState, flags, helpers
      opcodes.h        # Shared Z80 opcode lengths & cycle tables
      vdp.h            # Mode 4 VDP + Game Gear 12-bit CRAM
      psg.h            # SN76489 PSG (mono + GG stereo)
      bus.h            # Memory map, Sega/Codemasters/Korean mappers, I/O ports
      interpreter.h    # Full cycle-accurate Z80 interpreter (also fallback)
      runtime.h        # SmsRuntime, frame loop, dispatch table, save states (DRST)
      frontend.h       # SDL3 window, input, audio, CRT scanlines, aspect ratio
    src/
      opcodes.cpp, vdp.cpp, psg.cpp, bus.cpp, interpreter.cpp, runtime.cpp,
      frontend.cpp, sms_runner_main.cpp
    tests/
      cpu_unit_tests.cpp
      vdp_unit_tests.cpp
      psg_unit_tests.cpp
  recompiler/
    include/smsrecomp/
      disassembler.h   # Z80 decoder & bank-aware CFG builder
      emitter.h        # Pure C99 code emitter (func_BB_XXXX)
    src/
      disassembler.cpp, emitter.cpp, main.cpp
    tests/
      recomp_unit_tests.cpp
```

### 6.2 Generated Symbol & Dispatch Conventions

- Generated functions: `void func_BB_XXXX(SmsCpuState* cpu, SmsBus* bus);` where `BB` is the 16 KiB ROM bank (`00` for fixed `$0000–$03FF` and bank 0).
- Indirect jumps (`JP (HL)`, `JP (IX)`, `JP (IY)`), computed calls, RAM-executed code (`$C000–$FFFF`), and undiscovered targets route through `sms_dispatch(cpu, bus)` which looks up `(current_bank(pc), pc)` in `kRecompDispatchTable` and falls back cleanly to `sms_interpreter_step()` while logging `FALLBACK bank=BB addr=XXXX` for DragonRecomp Studio's auto-learner.
- Polling `sms_runtime_poll(cpu, bus)` checks pending VDP/NMI interrupts and steps VDP/PSG in lockstep with `cpu->cycles`.

### 6.3 Save States & Battery SRAM
- Battery SRAM (`32 KiB` max): auto-saved to `<rom>.sav` on exit when dirty.
- Save States (`DRST` header + `SMS1` tag): F5/F7 quick save/load + `--save-state <path>` autosave integration for DragonRecomp Studio Spec 005.

---

## 7. Verification Milestones (Acceptance Criteria)

1. **Unit tests (no ROM required):**
   - `sms_cpu_unit_tests`: Z80 ALU flags (`S/Z/H/P/V/N/C`), `DAA`, `EX/EXX`, `IX/IY` indexed ops, `CB`/`ED`/`DDCB`/`FDCB` prefixes, `LDIR/CPIR`, `EI` delay, IM 1 IRQ & NMI.
   - `sms_vdp_unit_tests`: Mode 4 nametable rendering, H/V scroll locks, column-0 mask, 8×8 & 8×16 sprites, overflow/collision flags, line interrupt counter underflow, SMS 6-bit & Game Gear 12-bit CRAM latch.
   - `sms_psg_unit_tests`: SN76489 tone frequencies, 16-bit `0x0009` white/periodic LFSR noise, GG stereo panning.
   - `sms_recomp_unit_tests`: Z80 CFG disassembly, bank-aware C99 emission, differential test comparing recompiled C99 execution against interpreter on synthetic Z80 ROMs.
2. **Real ROM tests (user-supplied in `roms/`):**
   - Boot, rendering, audio, input, and native C99 recompilation of real `.sms` and `.gg` games with DragonRecomp Studio integration.
