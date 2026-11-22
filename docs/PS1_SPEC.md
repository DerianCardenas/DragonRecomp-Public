# DragonRecomp PlayStation 1 (PS1 / PSX) Specification

## 1. Architecture Overview

PlayStation 1 (SCPH-1000..9000) hardware targeted by `psxrecomp` + `ps1/runtime`:

- **CPU**: LSI Logic R3000A (MIPS I, 32-bit little-endian, 33.8688 MHz) — decoded via shared `common/mips` (`MipsProfile::R3000A`).
- **COP0**: System Control Coprocessor (`SR`, `CAUSE`, `EPC`, `BADVADDR`, `PRID`, `RFE`, IRQ & Syscall exceptions).
- **COP2 (GTE)**: Geometry Transformation Engine — 32 data registers (`VXY0..2`, `VZ0..2`, `RGBC`, `OTZ`, `IR0..3`, `SXY0..2/P`, `SZ0..3`, `RGB0..2`, `MAC0..3`, `IRGB/ORGB`, `LZCS/LZCR`) and 32 control registers (`RT`, `TR`, `LLM`, `BK`, `LCM`, `FC`, `OFX/OFY`, `H`, `DQA/DQB`, `ZSF3/ZSF4`, `FLAG`) with full fixed-point commands (`RTPS`, `RTPT`, `NCLIP`, `MVMVA`, `AVSZ3`, `AVSZ4`, `OP`, `SQR`, `DPCS`, `DPCT`, `DCPL`, `INTPL`, `NCS`, `NCT`, `NCCS`, `NCCT`, `NCDS`, `NCDT`, `CC`, `CDP`, `GPF`, `GPL`).
- **Memory Map**:
  - `0x00000000–0x001FFFFF` (KUSEG), `0x80000000–0x801FFFFF` (KSEG0), `0xA0000000–0xA01FFFFF` (KSEG1): **2 MB Main RAM** (mirrored across 8 MB window).
  - `0x1F800000–0x1F8003FF`: **1 KB Scratchpad** (D-Cache fast RAM).
  - `0x1F801000–0x1F802FFF`: **Hardware I/O Registers** (Memory Control, SIO0 Pad/MemCard, Interrupt Control `I_STAT`/`I_MASK`, DMA Ch0–Ch6, Timers 0/1/2, CD-ROM `0x1F801800`, GPU `GP0`/`GP1` `0x1F801810`, MDEC `0x1F801820`, SPU `0x1F801C00`).
  - `0x1FC00000–0x1FC7FFFF` (`0xBFC00000`): **512 KB BIOS ROM** (optional real BIOS or built-in HLE BIOS stub trampolines).
- **GPU**: 1 MB VRAM (`1024×512` × 16-bit BGR555) with `GP0`/`GP1` command processor, linked-list DMA (Channel 2 + Channel 6 OTC), flat/Gouraud/textured triangles & quads (4-bit CLUT, 8-bit CLUT, 15-bit direct), 4 semi-transparency blend modes, sprites/rectangles, lines, CPU↔VRAM and VRAM↔VRAM transfers, 256/320/512/640 × 240/480 15-bit & 24-bit display output.
- **SPU**: 24-voice 4-bit ADPCM synthesizer with 512 KB SPU RAM, 5 PSX ADPCM filter coefficients, ADSR envelopes, loop flags, and 44.1 kHz stereo output.
- **Input & Storage**: SIO0 Digital/DualShock controller + 128 KB Memory Card slots, CD-ROM ISO9660 reader (2048-byte `.iso` and 2352-byte Mode 2/Form 1 `.bin`/`.cue`), and `PS-X EXE` direct executables.

## 2. Executable & Disc Formats

1. **`PS-X EXE`** (`.exe`, `.psexe`, `.psx`): 2048-byte header (`"PS-X EXE"`), `initial_pc` (`+0x10`), `initial_gp` (`+0x14`), `load_addr` (`+0x18`), `file_size` (`+0x1C`), `sp_base` (`+0x30`), `sp_offset` (`+0x34`).
2. **CD-ROM Disc Images** (`.cue` + `.bin`, `.bin`, `.iso`):
   - Parses `.cue` to locate Track 1 binary image and sector mode (`MODE2/2352` or `MODE1/2048`).
   - Parses ISO9660 Primary Volume Descriptor, locates `SYSTEM.CNF`, reads `BOOT = cdrom:\<EXE>;1`, and loads the `PS-X EXE` automatically into Main RAM while keeping the disc image mounted for CD-ROM I/O & `BIOS` file calls.

## 3. BIOS HLE Kernel

Built-in High-Level BIOS emulation eliminates any copyrighted BIOS requirement while remaining compatible with optional BIOS files:
- Vector trampolines at `0x800000A0` (`A(xx)`), `0x800000B0` (`B(xx)`), `0x800000C0` (`C(xx)`) and `0x80000080` (exception/VBlank handler).
- Implements `FlushCache`, `FileOpen`/`Read`/`Seek`/`Close`, `InitPAD`/`StartPAD`/`StopPAD`, `OpenEvent`/`EnableEvent`/`TestEvent`/`WaitEvent`, `EnterCriticalSection`/`ExitCriticalSection`, `ChangeClearRCnt`, `ChangeClearPAD`, `DeliverEvent`, `SetDefaultExitFromException`, `ReturnFromException`, `printf`, `memcpy`/`memset`/`strcmp`/`strlen`, and `CdInit` support.

## 4. Static Recompiler (`psxrecomp`)

- Uses shared `dragon::mips::decode_mips(..., MipsProfile::R3000A)`.
- Discovers functions starting from `initial_pc` plus `jal` targets and MIPS prologue scans (`addiu $sp, $sp, -N` + `sw $ra`).
- Emits portable **C99** (`func_80XXXXXX(Ps1CpuState* cpu)`) with exact MIPS I delay slots, GTE COP2 helper calls, hardware bus helpers (`ps1_read*`/`ps1_write*`), and `ps1_runtime_dispatch()` fallback to the R3000A interpreter for dynamic/overlay code.

## 5. Milestones

- **PS1-1**: `docs/PS1_SPEC.md`, `knowledge/ps1.toml`, config support.
- **PS1-2**: `ps1/runtime` loader (`PS-X EXE` + `.cue`/`.bin`/`.iso`), R3000A + COP0 + GTE interpreter, DMA, Timers, IRQ, SIO0, CD-ROM & BIOS HLE + unit tests.
- **PS1-3**: PS1 GPU (`GP0`/`GP1`, VRAM 1024×512 rasterizer), SPU 24-ch ADPCM, SDL3 4:3 frontend, Save States (`DRST` `"PS1 "`) + unit tests.
- **PS1-4**: `psxrecomp` static recompiler (R3000A CFG + C99 emitter) + unit tests.
- **PS1-5**: DragonRecomp Studio integration (detection, cover art `Sony - PlayStation`, controls, auto-pipeline, packager, docs, `studio_unit_tests`).
