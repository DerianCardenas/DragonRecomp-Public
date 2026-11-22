# DragonRecomp — Nintendo DS (NTR-001) Technical Specification

Reference: [GBATEK — Nintendo DS Technical Info](https://problemkaputt.de/gbatek.htm)

## 1. Overview

`ndsrecomp` statically recompiles Nintendo DS (`.nds`) ARM946E-S (ARMv5TE + Thumb) binaries into portable C99, linked against `nds_runtime` which emulates the dual-CPU system (ARM946E-S @ 67.03 MHz + ARM7TDMI @ 33.51 MHz), IPC synchronization, dual 2D engines (A & B), 3D GX Geometry Engine + shared `common/gpu3d` rasterizer, 16-channel SPU audio, NitroFS cartridge streaming, SPI Save memory, touchscreen (TSC2046), and Direct Boot BIOS HLE without requiring proprietary Nintendo DS BIOS or firmware dumps.

## 2. Dual-CPU Architecture

| CPU | Architecture | Clock | Role |
| :--- | :--- | :---: | :--- |
| **ARM946E-S** | ARMv5TE + Thumb + CP15 | 67.027964 MHz | Main game logic, 2D Engines A/B, 3D GX, NitroFS, Math |
| **ARM7TDMI** | ARMv4T + Thumb | 33.513982 MHz | Audio SPU, Touchscreen/SPI, RTC, Powerman, Save SPI, IPC |

Synchronization ratio: exact **2:1** (2 ARM9 cycles = 1 ARM7 cycle; 1 scanline = 4260 ARM9 cycles / 2130 ARM7 cycles; 263 scanlines/frame ≈ 59.8261 Hz).

### ARMv5TE Extensions over GBA ARMv4T (ARM946E-S)
- `CLZ Rd, Rm`: Count Leading Zeros.
- `BLX <label>` (ARM unconditional `0xFA/0xFB`, switches to Thumb) and `BLX Rm` (ARM & Thumb).
- Thumb 32-bit `BLX` (`11110 imm11` + `11101 imm11`, clears bit 1 and switches to ARM).
- `LDR PC, [...]` and `LDM ..., {..., PC}` interworking: bit 0 of loaded address updates `CPSR.T`.
- Saturating arithmetic (`QADD`, `QSUB`, `QDADD`, `QDSUB`) updating `CPSR.Q` (bit 27).
- Signed 16×16 / 32×16 DSP multiplies (`SMLAxy`, `SMLAWy`, `SMULWy`, `SMLALxy`, `SMULxy`).
- Doubleword loads/stores (`LDRD`, `STRD`).
- **CP15** (`MRC`/`MCR p15`):
  - `c0, c0, 0`: Main ID (`0x41059461`).
  - `c1, c0, 0`: Control Register (bit 13 = high vectors `0xFFFF0000`, bit 16 = DTCM enable, bit 18 = ITCM enable).
  - `c7, c0, 4`: Wait For Interrupt (halts ARM9 until enabled IRQ).
  - `c9, c1, 0`: DTCM region base/size (16 KB, e.g., `0x027C0000` or `0x0B000000`).
  - `c9, c1, 1`: ITCM region base/size (32 KB at `0x00000000`).

## 3. Memory Maps

### ARM9 Bus
- `0x00000000–0x01FFFFFF`: ITCM (32 KB mirrored across configured ITCM window when enabled via CP15).
- `0x02000000–0x02FFFFFF`: Main RAM (4 MB, mirrored every 4 MB).
- `0x03000000–0x03FFFFFF`: Shared WRAM (32 KB, mapped per `WRAMCNT` `0x04000247`).
- `0x04000000–0x04FFFFFF`: ARM9 I/O Registers (2D Engine A `0x04000000`, 2D Engine B `0x04001000`, DMA, Timers, IPC, VRAMCNT, Math Div/Sqrt, GX 3D `0x04000400`, Cart I/O `0x040001A0`).
- `0x05000000–0x050007FF`: Standard Palette RAM (1 KB Engine A + 1 KB Engine B).
- `0x06000000–0x06FFFFFF`: VRAM (Banks A–I dynamically mapped via `VRAMCNT_A..I`):
  - `0x06000000`: Engine A BG (512 KB space)
  - `0x06200000`: Engine B BG (128 KB space)
  - `0x06400000`: Engine A OBJ (256 KB space)
  - `0x06600000`: Engine B OBJ (128 KB space)
  - `0x06800000`: LCDC direct mapping (656 KB contiguous A–I)
- `0x07000000–0x070007FF`: OAM (1 KB Engine A + 1 KB Engine B).
- `0xFFFF0000–0xFFFF7FFF`: ARM9 HLE BIOS stub vector table.
- Configurable DTCM window (16 KB, checked first for data reads/writes when CP15 DTCM is enabled).

### ARM7 Bus
- `0x00000000–0x00003FFF`: ARM7 HLE BIOS stub vector table.
- `0x02000000–0x02FFFFFF`: Main RAM (4 MB shared with ARM9).
- `0x03000000–0x037FFFFF`: Shared WRAM (per `WRAMCNT`, falls back to ARM7 WRAM if unmapped).
- `0x03800000–0x03FFFFFF`: ARM7 Private WRAM (64 KB).
- `0x04000000–0x04FFFFFF`: ARM7 I/O Registers (IPC, DMA, Timers, SPU `0x04000400`, SPI `0x040001C0`, Cart I/O, RTC `0x04000138`, `EXTKEYIN` `0x04000136`, `IE`/`IF`/`IME`).
- `0x06000000–0x0603FFFF`: VRAM Banks C/D when mapped to ARM7 (`MST = 2`).

## 4. Direct Boot & BIOS HLE

No copyrighted BIOS or firmware images are required:
1. Parse `.nds` 512-byte header (`0x000–0x1FF`), validating ARM9/ARM7 offsets, RAM addresses, entry points, and sizes.
2. Copy ARM9 binary into Main RAM at `arm9_ram_addr` and ARM7 binary at `arm7_ram_addr`.
3. Populate NDS Boot Info block at `0x027FFE00` (copy of header `0x000–0x16F`), chip ID (`0x027FF800` / `0x027FFC00`), and synthetic Firmware User Settings at `0x027FFC80` (with 1:1 touchscreen calibration `adc_x1=0, adc_y1=0, scr_x1=0, scr_y1=0, adc_x2=0x0FF0, adc_y2=0x0BF0, scr_x2=255, scr_y2=191`).
4. Set `POSTFLG = 1`, `POWCNT1 = 0x820F`, `WRAMCNT = 0x03`, ARM9 CP15 `CR1 = 0x00052078` (high vectors, ITCM/DTCM enabled, DTCM base `0x027C0000`).
5. Handle SWI calls via HLE (`WaitByLoop`, `IntrWait`, `VBlankIntrWait`, `Halt`, `Div`, `Sqrt`, `CpuSet`, `CpuFastSet`, `LZ77UnCompWram/Vram`, `RLUnComp`, `HuffUnComp`, `GetCRC16`, `SoundBias`, `GetSineTable`, `GetPitchTable`, `GetVolumeTable`).

## 5. Video: Dual 2D Engines + 3D GX + `common/gpu3d`

- **Dual 256×192 screens** rendered into a `256×384` RGBA framebuffer (`POWCNT1` bit 15 selects whether Engine A is top or bottom).
- **2D Engine A & Engine B**: Modes 0–5 (Text BG, Affine BG, Extended Affine / 256-color & Direct 16-bit Bitmap BG), 128 OAM sprites per engine (normal + affine, 1D/2D tile mapping), Standard & Extended Palettes, Master Brightness, Alpha Blending.
- **3D GX Engine**:
  - `GXFIFO` (`0x04000400`) + direct command ports (`0x04000440–0x040005FF`), 4×4 fixed-point (`1.19.12`) matrix stacks (Projection, Position, Direction, Texture), lighting, vertex submission, primitive assembly, texture decoding (formats 1–7 from VRAM texture slots + palette slots), and `SWAP_BUFFERS`.
  - Dispatches transformed triangles to the shared **`common/gpu3d`** backend (`SoftGpu3d` deterministic software rasterizer with Z-buffer, perspective-correct UV interpolation, alpha blending, depth test, and face culling; reusable by PSP, PS1, and N64).
  - Composites 3D output into Engine A BG0 when `DISPCNT_A` bit 3 is enabled.

## 6. Audio (SPU — 16 Channels)

- 16 channels (`0x04000400–0x040004FF` on ARM7): PCM8, PCM16, IMA-ADPCM, plus PSG Square (ch 8–13) and White Noise (ch 14–15), stereo panning and master volume, resampled to 48 kHz stereo for SDL3.

## 7. Static Recompilation (`ndsrecomp`)

- **RAM-resident code**: Disassembles the ARM9 boot binary at `arm9_ram_addr` (plus optional ARM9 NitroFS overlay table entries) using a recursive ARMv5TE + Thumb CFG disassembler.
- **Symbols**: `func_02XXXXXX_arm` / `func_02XXXXXX_thumb` (and `func_00XXXXXX_*` for ITCM when configured).
- **Dynamic fallback**: Indirect branches, ITCM-copied code, and dynamically loaded overlays seamlessly fall back to the ARM946E-S interpreter (`nds_runtime_dispatch`) while ARM7 runs on the synchronized ARM7TDMI core.
