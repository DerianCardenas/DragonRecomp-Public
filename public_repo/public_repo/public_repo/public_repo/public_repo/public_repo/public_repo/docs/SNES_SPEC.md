# SNES Specification (65816 / S-PPU / SPC700)

See [SPEC.md](SPEC.md) for the general pipeline and conventions.
References: <https://snes.nesdev.org/wiki/>, <https://problemkaputt.de/fullsnes.htm>,
<https://wiki.superfamicom.org/>.

## 1. CPU: Ricoh 5A22 (65816 core)

### 1.1 State

```c
typedef struct CpuState {
    uint16_t a, x, y, s, d;   /* C accumulator (B:A), index, stack, direct page */
    uint8_t  db, pb;          /* data bank, program bank */
    uint8_t  c, v, i, d_flag, m, xf, e;   /* unpacked flags; e = emulation */
    uint16_t n, z;            /* lazy N/Z; N bit = bit7 or bit15 per width */
    uint64_t master_cycles;
    uint64_t next_event;
    void    *bus;
} CpuState;
```

- 24-bit addresses: `bank:addr`. `DB` used for absolute data, `PB` for code,
  bank 0 for direct page and stack. Direct page `D` can be anywhere in bank 0;
  `D & 0xFF != 0` adds a cycle to DP accesses.
- **Decimal mode IS implemented** on the 65816 and some games use it
  (score counters): `ADC`/`SBC` must honor D in 8- and 16-bit widths.

### 1.2 M/X flags → code variants

The width of A/memory (M) and X/Y (X) changes opcode lengths for immediates,
so the disassembler must **track M/X statically**:
- Each function is decoded for a specific entry state `(m, x)`; `REP`/`SEP`
  with constant operands update the state; intra-procedural `PHP`/`PLP` pairs
  restore the pushed `(m, x)` state; inter-procedural `RTS`/`RTL` propagate
  the callee's exit `(m, x)` state back to `JSR`/`JSL` callers; unmatched `PLP`,
  `RTI`, and `XCE` make it unknown unless hinted.
- Native interrupt entry points (`NMI_Native`, `IRQ_Native`) are seeded across
  all 4 native `(m, x)` combinations (`m1x1`, `m0x1`, `m1x0`, `m0x0`).
- The same guest address may be compiled multiple times:
  `func_BB_XXXX_m1x1`, `func_BB_XXXX_m0x1`, …; only variants actually reached
  are emitted. Callers pick the variant from their own known state at the call
  site; if unknown, `runtime_dispatch` selects by current `cpu->m/xf`.
- Config `mx_hints` force entry states and resolve unknowns after `PLP`.
- Setting X=1 clears the high bytes of X and Y (emit explicitly).
- Emulation mode (`e = 1`): forces M=X=1, stack in page 1, 6502-style
  vectors. Games switch to native mode early (`CLC; XCE`); code before that
  is compiled in the `e1` variant. `XCE` with unknown carry → dispatch.

### 1.3 Instructions

All 256 opcodes are official (no unofficial set). Notables:
- Long addressing (`al`, `[dp]`, `[dp],Y`), stack-relative (`sr,S`,
  `(sr,S),Y`), block moves `MVN`/`MVP` (emit a loop with per-byte cycle
  accounting and bus calls; interruptible), `PEA`/`PEI`/`PER`, `JSL`/`RTL`,
  `JML [abs]`, `JMP (abs,X)` (bank = PB), `COP`, `WDM`, `STP`, `WAI`
  (`WAI` emits `runtime_wai(cpu); if (cpu->halted) return;`).
- `TCS/TSC/TCD/TDC/XBA/TXY/TYX`.
- Wrapping rules differ between emulation and native mode (DP, stack).

### 1.4 Timing

- Master clock 21.477 MHz (NTSC). Memory access speed per region:
  6 (fast ROM when `$420D` bit0=1 in banks $80+), 8 (slow ROM, WRAM),
  12 (`$4000-$41FF` joypad serial). Internal operations 6.
- Generated code accumulates master cycles per bus access type; region speed
  is resolved statically when the address is known, else via the bus.
- H/V counters derived from master cycles (1364 per scanline, 262 lines).

### 1.5 Interrupts & Synchronization

- NMI at V-blank start (`$4200` bit7), H/V IRQ (`$4200` bits 4-5, `$4207-$420A`),
  `$4210`/`$4211` acknowledge. Delivered at `runtime_poll` as in SPEC §4.4.
- `WAI` ($CB): never emit `cpu->waiting = 1; return;` because returning from
  recompiled C subroutines terminates the routine prematurely. Instead,
  `runtime_wai(cpu)` enters a scanline-advancing poll loop. Asserted NMI or
  Timer IRQ clears `cpu->waiting = 0`. If unmasked (`I = 0`), the interrupt
  handler is dispatched; then `runtime_wai` exits and execution continues
  linearly at `next_pc`.
- **Nested IRQs in NMI**: the 65816 CPU hardware allows IRQs during NMI if
  the NMI handler executes `CLI` (`I = 0`). The runtime dispatches timer IRQs
  even when `in_nmi_` is true (used by games like SMB All-Stars for split-screen
  status bar raster effects).
- **Coroutine Context Switching in NMI/IRQ**: `runtime_poll` preserves the
  `PB:PC` restored by `RTI` rather than overwriting it with the pre-interrupt
  `PB:PC`. When an interrupt handler switches stacks (`TCS`/`TXS`) to yield to a
  background coroutine (such as *The Legend of Zelda: A Link to the Past*
  switching at scanline 216 IRQ to its 3D Triforce thread), `runtime_poll`
  continues dispatching the active coroutine until a subsequent interrupt's
  `RTI` restores the interrupted main thread.

## 2. DMA / HDMA

- 8 channels, `$43x0-$43xA`. General-purpose DMA (`$420B`) halts the CPU:
  8 master cycles per byte + overhead; executed by the runtime synchronously
  inside the `bus_write` to `$420B` (cycles added to `master_cycles`).
- HDMA (`$420C`): per-scanline transfers at H=~1104; runtime performs them in
  scanline catch-up. Indirect mode and repeat flag supported.

## 3. S-PPU

- Scanline-based renderer (pixel-accurate within the line) is sufficient for
  most games; raster effects come from HDMA (per-line) and mid-line writes are
  rare (accept inaccuracy initially).
- BG modes 0–7 (Mode 1 is the priority for SMW; Mode 7 for later), sprites
  (OBJ, 128 entries, sizes per `$2101`), windows, color math (main/sub screen),
  mosaic, brightness/forced blank (`$2100`), offset-per-tile (modes 2/4/6).
- VRAM 64 KiB, CGRAM 512 B, OAM 544 B; access restrictions during rendering.
- Output 256×224 (239 overscan), hi-res/interlace later.
- **Pixel Packing & Frontend Format**: scanline buffer packs 32-bit pixels as
  `(r8 << 24) | (g8 << 16) | (b8 << 8) | 0xFF`. The SDL3 texture must use
  `SDL_PIXELFORMAT_RGBA8888` (not `RGBA32`, which on little-endian x86_64
  maps to `ABGR8888`, turning alpha into saturated red and swapping red/blue).

### 3.1 Controller Serial Ports ($4016 / $4017)

- SNES Standard Controller serial shift is **MSB-first** (bit 15 first: B, Y,
  Select, Start, Up, Down, Left, Right, A, X, L, R).
- Reads return `(joy_shift_ & 0x8000) != 0 ? 1 : 0`, followed by left-shift
  `joy_shift_ = (joy_shift_ << 1) | 1`. Active-high strobe on `$4016.0` latches
  controller state continuously.

## 4. Audio: SPC700 + S-DSP

- **Emulated, not recompiled**: the SPC700 runs its own code uploaded at boot
  (IPL handshake via `$2140-$2143`). An accurate SPC700 interpreter + S-DSP
  (8 voices BRR, ADSR/GAIN, echo, noise, pitch mod) at 1.024 MHz / 32 kHz.
- **Full SPC700 Instruction Set Coverage**: All 256 SPC700 opcodes ($00–$FF) are
  implemented in `snes/runtime/src/apu.cpp` including DP/memory-to-memory ALU ops,
  bit-manipulation (`SET1`/`CLR1`/`BBS`/`BBC`), single-bit Boolean instructions
  (`OR1`/`AND1`/`EOR1`/`MOV1`/`NOT1` with 13-bit memory-bit addressing), table
  calls (`TCALL 0..15`, `PCALL`), and 16-bit word operations (`ADDW`, `SUBW`,
  `CMPW`, `INCW`, `DECW`, `MOVW`). Game sound drivers (e.g., N-SPC) rely on these
  instructions without incurring desynchronizations or stalled voices.
- Synchronized with the main CPU via catch-up on port access. Communication
  loops (CPU polling `$2140`) are timing sensitive → precise cycle ratio.
- Output 32 kHz resampled to 48 kHz for SDL3.

## 5. Cartridge mapping

| Map | Header at | ROM mapping |
|-----|-----------|-------------|
| LoROM (mode $20/$30) | `$7FC0` | 32 KiB banks at `$8000-$FFFF` of banks `$00-$7D`, `$80-$FF` |
| HiROM (mode $21/$31) | `$FFC0` | 64 KiB banks at `$C0-$FF` (mirrors in `$00-$3F:8000`) |
| ExHiROM | `$40FFC0` | later |

SRAM: LoROM `$70-$7D:0000-7FFF`, HiROM `$20-$3F:6000-7FFF`. Cartridges whose
header declares `0 KiB` SRAM and no battery allocate `0` bytes of SRAM and
return open-bus on reads from `$70-$7D:0000-7FFF` (required by SRAM copy-protection
checks).
Copier headers (512 B when `size % 1024 == 512`, or 1024 B double copier headers
when `size % 0x8000 == 1024` and the header score improves at `+1024`) are stripped. Detection by
header checksum scoring; config `map` overrides. Mirror banks (`$00` vs `$80`)
are canonicalized for function naming: the **executing** bank is used
(`func_80_8000` ≠ `func_00_8000` for timing—FastROM—but may share code).
Coprocessors (SA-1, SuperFX, DSP-1) are out of scope initially.

## 6. Milestones

### S1 — 65816 interpreter + bus
- Interpreter with all opcodes, M/X/E, decimal; LoROM/HiROM loader; WRAM,
  DMA; minimal PPU stubs.
- **Acceptance**: community CPU test ROMs (e.g. PeterLemon/krom SNES CPU
  tests, `cputest` by gilyon) pass; CTest green.

### S2 — PPU + SPC700 + frontend
- Modes 0/1 + sprites + windows + color math; HDMA; SPC700 + DSP; SDL3.
- **Acceptance**: Super Mario World boots to title and is playable in
  interpreter mode with sound.

### S3 — Recompiler for SMW
- `snesrecomp` with M/X variant tracking and `smw.toml` (mx_hints, jump
  tables — SMW uses many `JSL ExecutePtr`-style inline pointer tables).
- **Acceptance**: SMW plays Yoshi's Island 1 → Yoshi's House with zero
  fallbacks; lock-step `--verify` shows no divergence.

### S4 — Coverage & polish
- Mode 7, hi-res, interlace, more games (HiROM title), FastROM timing,
  open-bus accuracy.
- **Acceptance**: one HiROM and one Mode-7-heavy game run recompiled.
