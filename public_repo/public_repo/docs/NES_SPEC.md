# NES Specification (6502 / 2A03 + 2C02)

See [SPEC.md](SPEC.md) for the general pipeline and conventions.
Primary reference: <https://www.nesdev.org/wiki/>.

## 1. CPU: Ricoh 2A03 (6502 core)

- 8-bit A, X, Y, S; 16-bit PC; flags N V - B D I Z C.
- **Decimal mode is not implemented** in the 2A03: `SED`/`CLD` set/clear D,
  but `ADC`/`SBC` always operate in binary. The recompiler must ignore D for
  arithmetic (keep the flag for `PHP`).
- Stack at `$0100-$01FF`, kept in guest memory (needed for RTS tricks).
- NTSC CPU clock 1.789773 MHz; 1 CPU cycle = 3 PPU dots (PAL: 3.2).

### 1.1 Official opcodes (151)

All must be translated with exact semantics and cycle counts
(<https://www.nesdev.org/wiki/6502_instructions>,
<https://www.nesdev.org/obelisk-6502-guide/reference.html>).

| Group | Instructions |
|-------|--------------|
| Load/store | LDA LDX LDY STA STX STY |
| Transfer | TAX TAY TXA TYA TSX TXS |
| Stack | PHA PHP PLA PLP |
| Logic | AND EOR ORA BIT |
| Arithmetic | ADC SBC CMP CPX CPY |
| Inc/dec | INC INX INY DEC DEX DEY |
| Shift | ASL LSR ROL ROR |
| Jumps | JMP (abs, ind) JSR RTS RTI BRK |
| Branches | BCC BCS BEQ BNE BMI BPL BVC BVS |
| Flags | CLC CLD CLI CLV SEC SED SEI |
| Misc | NOP |

Addressing modes: imp, acc, imm, zp, zp,X, zp,Y, abs, abs,X, abs,Y, (ind),
(zp,X), (zp),Y, rel.

Notable semantics:
- `JMP ($xxFF)` bug: high byte fetched from `$xx00`, not next page.
- Zero-page indexed wraps in page 0; `(zp,X)` and `(zp),Y` pointer fetch wraps
  in page 0.
- `BIT`: N,V from memory bits 7,6; Z from `A & M`.
- `PHP`/`BRK` push P with B=1 and bit5=1; IRQ/NMI push with B=0.
- `PLP`/`RTI` ignore B and bit5.
- `CLI`/`SEI`/`PLP` delay the effect on IRQ polling by one instruction;
  `RTI` takes effect immediately.
- RMW instructions (`INC`, `ASL` …) on memory do a dummy write of the original
  value before the final write — matters for I/O (e.g. `$4014`, mapper regs).

### 1.2 Cycle rules

- Base cycles per opcode from a single table shared by recompiler, interpreter
  and tests (a single header, e.g. `nes/runtime/include/nes/opcodes.h`, one source of truth).
- +1 for page crossing on read instructions using abs,X / abs,Y / (zp),Y.
- Branches: +1 if taken, +1 more if the target is in a different page.
- Writes and RMW with indexed modes always take the "crossed" count.
- OAM DMA (`$4014` write): 513 or 514 cycles stall (odd/even CPU cycle).
- DMC DMA steals cycles (1–4); approximated initially, refined later.

### 1.3 Unofficial opcodes policy

- Recompiler: by default an unofficial opcode encountered during recursive
  descent is treated as **"probably data"**: the block is terminated, a warning
  is printed, and a `runtime_dispatch` fallback is emitted at that address.
- Config flag `allow_unofficial = true` enables translation of the stable ones
  (LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, ANC, ALR, ARR, AXS, NOPs/SKB/IGN).
  Unstable ones (XAA, AHX, TAS, SHX, SHY, LAS) and KIL/JAM always fall back
  to the interpreter (which halts on KIL).
- The interpreter implements all stable unofficial opcodes (nestest needs them).

## 2. PPU (2C02)

Model: **dot-based, catch-up**. The PPU advances 3 dots per CPU cycle when the
CPU touches I/O or at `runtime_poll`. Rendering is done per dot (or per
8-dot fetch group with exact per-dot sprite-0 evaluation), which is accurate
enough for:
- **Sprite-0 hit** (SMB polls `$2002` bit 6 to split the status bar from the
  scrolling playfield): hit must be set at the exact dot where opaque
  sprite-0 pixel overlaps opaque BG pixel (not at x=255, not if left 8 px
  clipped, set no earlier than dot 2).
- Mid-frame scroll writes (`$2005`/`$2006`) and the `v`/`t`/`x`/`w` loopy
  registers.
- VBlank flag set at scanline 241 dot 1; NMI if enabled; `$2002` read race
  (read at dot 0/1 of 241 suppresses NMI).
- Odd-frame skipped dot (pre-render line) when rendering is enabled.

Frame: 262 scanlines × 341 dots (NTSC); visible 0–239, post 240, vblank
241–260, pre-render 261.

Registers `$2000-$2007` (mirrored to `$3FFF`), `$4014` OAM DMA. Open-bus
behavior of PPU data latch should be emulated (some games rely on it).
Mirroring: horizontal/vertical from header for NROM; mapper-controlled later.
Palette: standard 2C02 palette (64 entries) to RGBA; emphasis bits supported.

Details: see skill `.agents/skills/nes-ppu/SKILL.md`.

## 3. APU (2A03)

- Channels: 2 pulse, triangle, noise, DMC; frame counter (4/5 step, IRQ).
- Clocked per CPU cycle (catch-up on `$4000-$4017` access and at poll).
- Output: non-linear mixer (lookup tables per nesdev
  <https://www.nesdev.org/wiki/APU_Mixer>), band-limited or simple
  low-pass + decimation to 48 kHz → SDL3 audio stream.
- `$4015` status reads, length counter, sweep, envelope, linear counter.
- DMC sample fetch steals CPU cycles; DMC IRQ.

## 4. Mappers roadmap

| Order | Mapper | Name | Games | Notes |
|-------|--------|------|-------|-------|
| 1 | 0 | NROM | SMB, Donkey Kong | 16/32 KiB PRG fixed, 8 KiB CHR-ROM; no banking → `func_XXXX` only |
| 2 | 1 | MMC1 (SxROM) | Zelda, Metroid | serial shift register; PRG banking → `func_BB_XXXX` |
| 3 | 2 | UxROM | Mega Man, Castlevania | switchable $8000, fixed $C000 |
| 4 | 3 | CNROM | | CHR banking only (no CPU impact) |
| 5 | 4 | MMC3 (TxROM) | SMB3 | scanline IRQ via A12 → requires accurate PPU fetch timing |
| 6 | 40 | NTDEC 2722 | Super Mario Bros 2 (J) [p1] | Bank 6 at $6000-$7FFF, fixed $8000/$A000/$E000, switchable $C000, 4096-cycle M2 IRQ timer |
| 7 | 99 | Vs. System | Vs. Duck Hunt, Vs. Battle City | Coin/service inputs ($4016), DIP switches, RP2C04 palettes, CHR banking |
| 8 | 189 | TXC / MMC3 | Street Fighter II (NES) | 32 KiB PRG banking via $4100-$5FFF, MMC3 CHR & scanline IRQ |

For banked mappers the recompiler emits one function per (bank, address) and
`runtime_dispatch` resolves using the **current** mapping. Code in the fixed
bank calling into a switchable window is emitted as a dispatch on the current
bank unless config pins it (`bank = N` on the entry point / call site hint).

## 5. Milestones

### N1 — Interpreter + runtime core
- CPU interpreter (official + stable unofficial), shared opcode/cycle table.
- Bus + NROM mapper, iNES loader with SHA-1.
- **Acceptance**: nestest automated mode matches `nestest.log` (PC, regs, P,
  cycles) to the end of official+unofficial section; CTest passes.

### N2 — PPU + SDL3 frontend
- Dot-based PPU, NMI, sprite-0 hit, OAM DMA, controller input, SDL3 window.
- **Acceptance**: SMB and Donkey Kong boot and are playable **in interpreter
  mode**; blargg `ppu_vbl_nmi` and `sprite_hit_tests_2005.10.05` pass.

### N3 — Recompiler for SMB
- `nesrecomp` reads `smb.toml`, emits C for all reachable code, builds
  `smb` executable; JumpEngine tables and other indirects configured.
- **Acceptance**: SMB runs from title through World 1-1 to 1-2 with **zero**
  interpreter fallbacks logged; `--verify` lock-step shows no divergence for
  ≥ 5 minutes of attract mode; Donkey Kong likewise.

### N4 — APU + polish + more mappers
- Full APU with audio output; MMC1, UxROM, CNROM, MMC3.
- **Acceptance**: blargg `apu_test` passes (1–8); SMB audio matches reference
  emulator by ear/waveform; at least one game per mapper boots recompiled.
