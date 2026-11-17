# Game Boy Advance Specification (ARM7TDMI + PPU + DirectSound APU + DMA + BIOS HLE)

See [SPEC.md](SPEC.md) for the general pipeline and conventions.  
Primary references:
- GBATEK (Martin Korth / nocash): <https://problemkaputt.de/gbatek.htm>
- Tonc GBA Programming Manual (Jasper Vijn): <https://www.coranac.com/tonc/text/toc.htm>
- ARM7TDMI Technical Reference Manual (ARM DDI 0029E)

---

## 1. Overview & Hardware Specifications

The Game Boy Advance subsystem (`gba/`) implements a static recompiler (`gbarecomp`) and hardware-accurate runtime (`gba_runtime`) for the 32-bit Game Boy Advance (`AGB-001` / `AGS-001`).

| Component | Specification |
| :--- | :--- |
| **CPU Core** | 32-bit ARM7TDMI (ARMv4T architecture) @ 16.777216 MHz ($2^{24}$ Hz) |
| **Instruction Sets** | **ARM** (32-bit word-aligned) and **Thumb** (16-bit halfword-aligned) with dynamic interworking (`BX`) |
| **Display** | 240 × 160 pixels, 15-bit RGB555 (32,768 colors), 59.7275 Hz refresh (280,896 cycles/frame) |
| **BIOS ROM** | 16 KiB (`$00000000 - $00003FFF`), protected with HLE `SWI` services and IRQ dispatcher |
| **Work RAM** | 256 KiB EWRAM (`$02000000`, 16-bit bus) + 32 KiB Fast IWRAM (`$03000000`, 32-bit bus) |
| **Video Memory** | 96 KiB VRAM (`$06000000`), 1 KiB Palette RAM (`$05000000`), 1 KiB OAM (`$07000000`) |
| **Cartridge ROM** | Up to 32 MiB mapped across 3 waitstate mirrors (`$08000000`, `$0A000000`, `$0C000000`) |
| **Cartridge Save** | SRAM (32/64 KiB), Flash (64/128 KiB with bank switching), EEPROM (512 B / 8 KiB via DMA3) |
| **Audio** | 2× 8-bit DirectSound DMA PCM FIFOs (`FIFO_A`, `FIFO_B`) + 4 legacy PSG channels |

### 1.1 Cartridge Header (`$08000000 - $080000BF`)

Every Game Boy Advance ROM begins with a 192-byte header:
- `$000 - $003`: 32-bit ARM branch instruction (`B start`) to the entry point.
- `$004 - $09F`: Compressed Nintendo Logo (156 bytes).
- `$0A0 - $0AB`: Game Title (12 uppercase ASCII characters).
- `$0AC - $0AF`: Game Code (4 uppercase ASCII characters, e.g., `AXVE`).
- `$0B0 - $0B1`: Maker Code (2 ASCII characters, e.g., `01` for Nintendo).
- `$0B2`: Fixed value `$96` (mandatory signature).
- `$0BD`: Header Complement Check (`chk = -(sum($A0..$BC) + 0x19) & 0xFF`).

---

## 2. CPU: ARM7TDMI (ARMv4T)

### 2.1 Registers & Execution Modes

The ARM7TDMI provides 16 visible 32-bit general-purpose registers (`R0`–`R15`) and the Current Program Status Register (`CPSR`):
- `R0`–`R12`: General purpose registers (`R0`–`R7` accessible as low registers in Thumb mode; `R8`–`R12` high registers).
- `R13` (`SP`): Stack Pointer (banked across User/System, IRQ, Supervisor, Abort, Undefined, FIQ modes).
- `R14` (`LR`): Link Register (holds subroutine/exception return addresses; banked per processor mode).
- `R15` (`PC`): Program Counter.
  - **Pipeline Prefetch Quirk:** Due to the 3-stage fetch-decode-execute pipeline, reading `R15` during instruction execution returns `inst_addr + 8` in ARM state and `inst_addr + 4` in Thumb state.
- `CPSR` (Current Program Status Register):
  - Bit 31 (`N`): Negative / Sign flag.
  - Bit 30 (`Z`): Zero flag.
  - Bit 29 (`C`): Carry / Unsigned borrow-not flag.
  - Bit 28 (`V`): Signed Overflow flag.
  - Bit 7 (`I`): IRQ Disable (`1` = disabled, `0` = enabled).
  - Bit 6 (`F`): FIQ Disable.
  - Bit 5 (`T`): Thumb State (`0` = 32-bit ARM, `1` = 16-bit Thumb).
  - Bits 4–0 (`M4..M0`): Processor Mode (`0x10` User, `0x12` IRQ, `0x13` Supervisor, `0x1F` System).

Post-BIOS boot state initialized by `gba_cpu_reset`:
- `R13_usr / R13_sys = 0x03007F00`
- `R13_irq = 0x03007FA0`
- `R13_svc = 0x03007FE0`
- `R15 (PC) = 0x08000000`
- `CPSR = 0x0000001F` (System mode, ARM state)

### 2.2 Multi-Game Calling & Branching Conventions

Different GBA compilers (`agbcc`, devkitARM GCC, ARM SDT/ADS, Metrowerks CodeWarrior, and custom assembly engines) generate vastly different calling, interworking, and branching patterns. Both `gbarecomp` and `gba_runtime` explicitly handle all of the following cases:

1. **Direct Calls (`BL target`)**:
   - In ARM mode: 24-bit signed word offset (`LR = inst_addr + 4`).
   - In Thumb mode: dual 16-bit instruction sequence (`0xF000..0xF7FF` high offset + `0xF800..0xFFFF` low offset), setting `LR = (inst_addr + 4) | 1U`.
2. **ARM ↔ Thumb Interworking (`BX Rm` & Compiler Veneers)**:
   - Because ARMv4T lacks an immediate `BLX label` instruction, compilers emit interworking veneers:
     - PC-relative literal load + exchange: `LDR Rm, [PC, #imm]; BX Rm` (where bit 0 of the literal pool word selects Thumb=1 or ARM=0). The disassembler statically resolves PC-relative literal pool loads (`LDR Rd, [PC, #imm]` and `ADD Rd, PC, #imm`) to automatically discover cross-mode targets and their exact ARM/Thumb state.
     - Inline mode switch veneer: Thumb `BX PC; NOP` switching to ARM at `(inst_addr + 4) & ~2U`.
     - Register call stubs (`_call_via_r0` .. `_call_via_r12` in `libgcc`/`agbcc`): The caller executes `BL _call_via_rX` (putting the caller's return address in `LR`), and `_call_via_rX` immediately executes `BX rX`. The callee then returns directly to the original caller via `BX LR`, bypassing `_call_via_rX`.
3. **Diverse Subroutine Return Idioms**:
   - `BX LR` (standard interworking return in both ARM and Thumb).
   - `MOV PC, LR` (ARM and Thumb high-register move `0x46F7`).
   - `POP {..., PC}` (Thumb `0xBDxx`). Note: on ARMv4T (ARM7TDMI), `POP {PC}` does **not** switch processor mode (bit 0 is cleared without changing `CPSR.T`), so compilers often emit `POP {R3}; BX R3` for Thumb-to-ARM returns.
   - `LDMFD SP!, {..., PC}` / `LDMFD SP!, {..., PC}^` (ARM block pop into `R15`, optionally restoring `CPSR = SPSR`).
   - `LDR PC, [SP], #4` (ARM single-word post-indexed pop into `R15`).
   - `SUBS PC, LR, #4` (ARM exception return restoring `CPSR = SPSR`).
   - Conditional returns in ARM (`BX<cc> LR`, `MOV<cc> PC, LR`, `LDM<cc>FD SP!, {..., PC}`).
4. **Switch / Jump Tables & GCC Thumb Table Helpers**:
   - **ARM Jump Tables**: `ADD PC, PC, Rn, LSL #2` or `LDR PC, [PC, Rn, LSL #2]` (often guarded by `CMP Rn, #max; BLS/BHI`).
   - **Thumb Jump Tables**: PC-relative table address loads followed by `LDRH`/`LDR` and `ADD PC, Rm` / `MOV PC, Rm`.
   - **GCC Thumb Case Helpers (`__gnu_thumb1_case_uqi`, `sqi`, `uhi`, `shi`, `si`)**: The caller executes `BL __gnu_thumb1_case_*` followed immediately by an inline array of 8-bit/16-bit/32-bit branch offsets inside the caller's code stream. The helper reads the offset at `LR`, computes `target = LR + offset`, and jumps back into the middle of the caller function!
   - **Intra-Function Local Dispatch (`dispatch_pc`) & Disjoint CFG Partitioning**: Every recompiled C99 function emits a label `L_08XXXXXX:` at each basic block leader (`func.block_addrs`) and includes a local `dispatch_pc:` switch table over its block leaders. Basic blocks are partitioned into strictly disjoint functions (promoting cross-function branches to standalone function seeds) and literal pool / `0xFF` padding boundaries halt linear scanning, keeping peak memory usage under 50 MB even on 16–32 MiB ROMs. Whenever a call or computed jump lands on a block leader belonging to the current function in the same ARM/Thumb state (such as returning from `__gnu_thumb1_case_*` or executing `ADD PC, Rm`), execution jumps directly to `goto L_08XXXXXX;` in native C99 without unwinding the C stack or falling back to the interpreter.
5. **Code Executing from IWRAM (`$03000000`) / EWRAM (`$02000000`)**:
   - Many GBA games copy time-critical ARM routines (audio mixers like `SoundMainRAM`, decompression, affine math, Flash programming) into IWRAM at runtime and invoke them via `BX Rm`.
   - When `gba_runtime_dispatch` targets RAM (`< 0x08000000`), it seamlessly executes the runtime's cycle-accurate ARM/Thumb interpreter until execution returns to the caller (`SP > entry_sp` or `PC == expected_ret`) or branches back to a compiled ROM function, without polluting the ROM fallback log.

---

## 3. Memory Map & Bus Alignment Rules

```
$00000000 - $00003FFF:  16 KiB System BIOS ROM (executable only from inside BIOS)
$02000000 - $0203FFFF: 256 KiB On-Board Work RAM (EWRAM, mirrored to $02FFFFFF)
$03000000 - $03007FFF:  32 KiB On-Chip Work RAM (IWRAM, mirrored to $03FFFFFF, incl. $03FFFF00)
$04000000 - $040003FE:   1 KiB Memory-Mapped I/O Registers (PPU, APU, DMA, Timers, Keypad, IRQ)
$05000000 - $050003FF:   1 KiB BG/OBJ Palette RAM (256 BG + 256 OBJ 15-bit RGB555 entries)
$06000000 - $06017FFF:  96 KiB Video RAM (64 KiB BG + 32 KiB OBJ)
$07000000 - $070003FF:   1 KiB Object Attribute Memory (128 OBJs × 8 bytes + 32 Affine matrices)
$08000000 - $09FFFFFF:  32 MiB Game Pak ROM / FlashROM (Wait State 0)
$0A000000 - $0BFFFFFF:  32 MiB Game Pak ROM Mirror (Wait State 1)
$0C000000 - $0DFFFFFF:  32 MiB Game Pak ROM Mirror (Wait State 2) + EEPROM ($0DFFFF00)
$0E000000 - $0E00FFFF:  64 KiB Game Pak SRAM / Flash (64 KiB or 128 KiB banked)
```

### 3.1 ARM7TDMI Unaligned Access & Bus Width Quirks

- **Unaligned `LDR` (32-bit read)**: Reading from `addr` where `(addr & 3) != 0` fetches the aligned word at `addr & ~3` and **rotates right** by `(addr & 3) * 8` bits (`ROR(word, (addr & 3) * 8)`).
- **Unaligned `LDRH` (16-bit unsigned read)**: Reading from `addr` where `(addr & 1) != 0` fetches the aligned halfword at `addr & ~1` and rotates right by 8 bits.
- **Unaligned `LDRSH` (16-bit signed read)**: Reading from an odd address `addr` sign-extends the single byte at `addr` (`(int32_t)(int8_t)read8(addr)`).
- **8-bit Writes (`STRB`) to Video Memory**: Writing a byte `v` to Palette RAM (`$05000000`) or BG VRAM (`$06000000 - $0600FFFF` / `$06013FFF` in bitmap modes) writes the duplicated halfword `(v << 8) | v` to `addr & ~1`. 8-bit writes to OAM (`$07000000`) are ignored.

---

## 4. PPU (240 × 160 Display Engine)

- **Timing:** 240×160 visible pixels; 228 total scanlines per frame (0–159 VDraw, 160–227 VBlank).
  - Each scanline takes **1,232 cycles** (960 cycles HDraw + 272 cycles HBlank).
  - Full frame: **280,896 cycles** (~59.7275 Hz).
- **Background Modes (`DISPCNT` bits 0–2):**
  - **Mode 0:** BG0, BG1, BG2, BG3 regular text/tile layers (4bpp 16-color or 8bpp 256-color tiles, screen sizes 256×256 to 512×512).
  - **Mode 1:** BG0, BG1 regular text/tile layers + BG2 affine (rotation/scaling).
  - **Mode 2:** BG2, BG3 affine layers.
  - **Mode 3:** 240×160 single-frame 15-bit RGB555 direct color bitmap (`$06000000`).
  - **Mode 4:** 240×160 double-buffered 8-bit paletted bitmap (`$06000000` / `$0600A000`, selected by `DISPCNT` bit 4).
  - **Mode 5:** 160×128 double-buffered 15-bit RGB555 bitmap (`$06000000` / `$0600A000`).
- **Sprites (OAM):** Up to 128 sprites (8×8 to 64×64), supporting normal and affine transformations, 1D (`DISPCNT` bit 6 = 1) and 2D (`DISPCNT` bit 6 = 0) tile mapping, H/V flip, priorities 0–3, and alpha blending.

---

## 5. DMA, Timers, APU & BIOS HLE

- **DMA (`DMA0`–`DMA3`, `$040000B0 - $040000DF`)**:
  - Supports Immediate (`0`), VBlank (`1`), HBlank (`2`), and Special/Audio FIFO (`3`) start timings, 16-bit and 32-bit transfer widths, repeat mode, and completion IRQs.
- **Timers (`TM0`–`TM3`, `$04000100 - $0400010F`)**:
  - 16-bit counters with prescalers (`1`, `64`, `256`, `1024`), cascade mode (increment when preceding timer overflows), overflow IRQ, and DirectSound FIFO A/B sample consumption on `TM0`/`TM1` overflow.
- **APU (`$04000060 - $040000A7`)**:
  - 2 DirectSound 8-bit signed PCM FIFOs (`FIFO_A` at `$040000A0`, `FIFO_B` at `$040000A4`) + 4 PSG channels mixed at 48,000 Hz stereo.
- **BIOS HLE & Interrupt Handling**:
  - Implements `SWI 0x00` (`SoftReset`) through `SWI 0x1F` (`MidiKey2Freq`).
  - Hardware interrupts vector through BIOS `$00000018` → `$00000128`, saving `R0-R3, R12, LR` onto `SP_irq`, setting `LR = 0x00000138`, and jumping in ARM mode to the user handler pointer at `[$03007FFC]` (mirrored at `[$03FFFFFC]`). Returning to `$00000138` restores the saved registers and returns to the interrupted context.

---

## 6. Milestones & Verification Criteria

| ID | Milestone | Key Acceptance Criterion |
| :---: | :--- | :--- |
| **A1** | ARM7TDMI Interpreter (ARM + Thumb) + Bus + Cartridge + BIOS HLE | Passes comprehensive ARM/Thumb instruction, interworking, SWI, DMA, Timer & Save tests (`gba_cpu_unit_tests`). |
| **A2** | GBA PPU (Modes 0–5, Affine, OAM Sprites, Blending) + SDL3 Frontend | Renders Tile Modes 0–2, Bitmap Modes 3–5, and OAM Sprites accurately (`gba_ppu_unit_tests`). |
| **A3** | `gbarecomp` Static Recompiler (ARM/Thumb CFG + Multi-Case C99 Emitter) | Recompiles ARM/Thumb interworking, veneers, switch tables, IWRAM calls & IRQ handlers with 0 divergences (`gba_recomp_unit_tests`). |
| **A4** | DirectSound DMA FIFO + PSG APU + Save Persistence (SRAM/Flash/EEPROM) | Timer-driven DMA FIFO audio streaming and battery `.sav` / `DRST` save states verified (`gba_apu_unit_tests`). |
| **A5** | DragonRecomp Studio Integration for GBA | Drag-and-drop `.gba` detection, auto-pipeline, fallback learning, and Linux/Windows portable packaging (`studio_unit_tests`). |

### 6.1 Known limitations & Automatic Fallbacks

- **Pokemon Emerald (SHA-1 `fe1558a3dcb0360ab558969e09b690888b846dd9`)** freezes at the
  title screen under native recompilation: its VBlank IRQ handler calls into the m4a
  sound-engine mixer (`func_082E6828_thumb`, chaining through veneer `func_082E6C22_thumb`
  into `082E71E8`/`082E8434`), and the recompiled code for that call chain never returns —
  it spins for ~172 iterations per handler invocation without making progress, so `IF` is
  never acknowledged and the game's `VBlankIntrWait` spin (`$080008C6`) never releases.
  Diagnosed by headless frame-by-frame comparison against `--interp` (both agree byte-for-
  byte up to frame 201; the recompiled path's EWRAM hash freezes there while the interpreter
  continues to the title). Root cause is in the recompiled arithmetic/flags of that specific
  call chain, not in IRQ dispatch, audio pacing, or the Studio build pipeline. **Mitigation**:
  `Pipeline::generate_initial_toml` (`studio/src/pipeline.cpp`) auto-detects this ROM by
  SHA-1/name and sets `interpreter_only = true`, same pattern as SMW on SNES (§S3). The
  cycle-accurate interpreter runs the game correctly and at full speed. A real codegen fix
  (tracing the m4a mixer call chain against ARM7TDMI semantics) is still open.

- **Pokemon Mystery Dungeon: Red Rescue Team (SHA-1 `5d46ce66ce8f01b7a6fc7e78119e73d32ab7e909`)**
  exhibits continuous audio/event loops that trigger stack nesting limits under naive C99
  emission. `Pipeline::generate_initial_toml` auto-detects this ROM and sets `interpreter_only = true`,
  ensuring stable 60 FPS gameplay, full audio and cutscenes.

- **Cart GPIO Port & RTC Seiko S-3511A**:
  Supported at `$080000C4..$080000C8` (data, direction, control). Serial 3-wire communication
  protocol (SCK, CS, SIO) provides real-time clock data (BCD year, month, day, day-of-week, hour,
  minute, second) synchronized to host time for Pokémon Ruby, Sapphire, Emerald and Boktai.
