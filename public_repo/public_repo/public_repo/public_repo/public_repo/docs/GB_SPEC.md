# Game Boy & Game Boy Color Specification (Sharp SM83 + PPU + APU + MBC)

See [SPEC.md](SPEC.md) for the general pipeline and conventions.  
Primary references:
- Pan Docs: <https://gbdev.io/pandocs/>
- gbdev Development Community: <https://gbdev.io/>
- The Ultimate Game Boy Talk (Gooee / Michael Steil): <https://www.youtube.com/watch?v=HyzD8pNfaAI>

---

## 1. Overview & Hardware Modes

The Game Boy subsystem (`gb/`) unifies support for both the original **Game Boy (DMG)** and the **Game Boy Color (CGB)** within a shared recompiler tool (`gbrecomp`) and runtime (`dragon-gb-runtime`).

| Feature | DMG (Game Boy Classic) | CGB (Game Boy Color) |
| :--- | :--- | :--- |
| **CPU Core** | Sharp SM83 @ 4.194304 MHz | Sharp SM83 @ 4.194304 MHz or 8.388608 MHz (Double Speed) |
| **Address Space** | 64 KiB total | 64 KiB total |
| **VRAM** | 8 KiB (1 bank) | 16 KiB (2 banks of 8 KiB conmuted via `VBK`) |
| **WRAM** | 8 KiB (Bank 0: 4 KiB, Bank 1: 4 KiB) | 32 KiB (Bank 0: 4 KiB, Banks 1..7: conmuted via `SVBK`) |
| **Palettes** | 3 registers (BGP, OBP0, OBP1) mapping 2-bit IDs to 4 grayscale shades | 8 BG palettes + 8 OBJ palettes, 4 colors each, 15-bit RGB555 |
| **DMA** | OAM DMA ($FF46, 160 bytes) | OAM DMA + General DMA (GDMA) + HBlank DMA (HDMA) |

### 1.1 Cartridge Header ($0100 - $014F)

Every Game Boy cartridge contains an internal header parsed during ROM loading:
- `$0100-$0103`: Entry point (usually `NOP` followed by `JP $0150`).
- `$0104-$0133`: Nintendo Scrolling Graphic (validated on boot).
- `$0134-$0143`: Title (uppercase ASCII, up to 16 bytes; on CGB truncated to 15 or 11 bytes).
- `$0143`: CGB Flag:
  - `$80`: Game supports CGB functions, but still runs on DMG.
  - `$C0`: Game requires CGB (does not work on DMG).
  - Other: Monochrome DMG game.
- `$0147`: Cartridge Type / MBC identifier:
  - `$00`: ROM ONLY
  - `$01`, `$02`, `$03`: MBC1 (plain, RAM, RAM+BATTERY)
  - `$05`, `$06`: MBC2 (built-in 512×4-bit RAM)
  - `$0F`, `$10`, `$11`, `$12`, `$13`: MBC3 (with RTC and/or RAM+BATTERY)
  - `$19`, `$1A`, `$1B`, `$1C`, `$1D`: MBC5 (clean 16-bit banking, optional rumble)
- `$0148`: ROM Size (`32 KiB << value`, from 32 KiB up to 8 MiB).
- `$0149`: RAM Size (`0`, `2 KiB`, `8 KiB`, `32 KiB`, `128 KiB`, `64 KiB`).
- `$014A`: Destination Code (`$00` Japan, `$01` Overseas).
- `$014D`: Header Checksum (verified using `x = x - [i] - 1`).

---

## 2. CPU: Sharp SM83 (DMG-CPU)

The SM83 is an 8-bit RISC microprocessor combining features of the Zilog Z80 and Intel 8080, with specific additions and removals.

### 2.1 Register Architecture

- **8-bit registers:** `A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`.
- **16-bit register pairs:** `AF`, `BC`, `DE`, `HL`.
- **16-bit stack and program counters:** `SP`, `PC`.
- **Flags Register (`F`):** Upper 4 bits represent status; lower 4 bits are **always zero**:
  - Bit 7: `Z` (Zero Flag) — set if result of operation is 0.
  - Bit 6: `N` (Subtraction Flag / BCD direction) — set if last operation was subtraction.
  - Bit 5: `H` (Half-Carry Flag) — set if carry occurred from bit 3 to bit 4 (or bit 11 to bit 12 in 16-bit additions).
  - Bit 4: `C` (Carry Flag) — set if carry occurred from bit 7 (or bit 15).
  - Bits 3-0: Fixed to `0`. Writing to `F` (via `POP AF`) clears bits 0..3.

```c
/* CpuState layout for Game Boy in gb/runtime/include/gb/cpu.h */
typedef struct CpuState {
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;
    uint16_t sp;
    uint16_t pc;
    
    /* Internal interrupt control */
    uint8_t ime;          /* Interrupt Master Enable (0 or 1) */
    uint8_t ime_delay;    /* EI delay counter (activates IME after next instruction) */
    uint8_t halted;       /* Set by HALT instruction */
    uint8_t halt_bug;     /* Set if HALT executed with IME=0 and IF & IE != 0 */
    uint8_t double_speed; /* 1 if CGB double-speed mode active, 0 otherwise */
    
    uint64_t cycles;      /* Clock cycles (T-cycles) elapsed */
} CpuState;
```

### 2.2 Instruction Set (500 opcodes)

- **256 Base Opcodes:** Standard 8-bit arithmetic, 16-bit arithmetic (`ADD HL, rr`, `ADD SP, e8`, `INC/DEC rr`), 8-bit/16-bit loads, jumps (`JP`, `JR`), calls (`CALL`, `RET`, `RST`), stack operations (`PUSH`, `POP`), and system control (`NOP`, `STOP`, `HALT`, `DI`, `EI`).
- **256 CB-Prefixed Opcodes:** Bit manipulations:
  - `RLC r`, `RRC r`, `RL r`, `RR r`, `SLA r`, `SRA r`, `SRL r` (Rotates & Shifts).
  - `SWAP r` (Swap upper and lower nibbles of byte).
  - `BIT b, r` (Test bit `b` in register `r` or `(HL)`).
  - `RES b, r` (Reset bit `b` to 0).
  - `SET b, r` (Set bit `b` to 1).
- Single opcode & cycle lookup table shared by recompiler, interpreter, and tests: `gb/runtime/include/gb/opcodes.h`.

### 2.3 Hardware Quirks & Crucial Semantics

1. **`EI` Delay:**
   - The `EI` instruction enables interrupts **after** the instruction following `EI` is executed.
   - Example: `EI` followed by `RET` allows `RET` to complete before any pending interrupt vector is serviced.
2. **The `HALT` Bug:**
   - When `HALT` is executed while `IME == 0` and an interrupt is pending (`(IE & IF & 0x1F) != 0`), the CPU does not halt; instead, the instruction immediately following `HALT` suffers a PC increment failure, reading its first byte twice.
3. **`ADD SP, e8` and `LD HL, SP+e8` Flags:**
   - In both instructions, `e8` is a signed 8-bit offset. However, the `C` and `H` flags are set based on an **unsigned 8-bit addition** of the lower byte of `SP` and `(uint8_t)e8` (carry out of bit 7 and bit 3), **not** a 16-bit carry. Flags `Z` and `N` are always reset to 0.
4. **Stack Tricks & Subroutine Desynchronization:**
   - Just like in NES/SNES, routines frequently manipulate `SP` directly (`ADD SP, n`, `LD SP, HL`, or unmatched `POP` sequences).
   - In recompiled code, calls to `runtime_poll()` compare `cpu->sp` against entry conditions to ensure native call stacks properly unwind without infinite loops.

---

## 3. Memory Map & Bus

The CPU communicates with memory and peripherals across a 16-bit address bus ($0000 - $FFFF):

```
$0000 - $3FFF: 16 KiB Fixed ROM Bank 00
$4000 - $7FFF: 16 KiB Switchable ROM Bank 01..NN (via MBC)
$8000 - $9FFF:  8 KiB Video RAM (VRAM) (CGB: Banks 0-1 conmuted via VBK $FF4F)
$A000 - $BFFF:  8 KiB External RAM (Cartridge SRAM / RTC registers via MBC)
$C000 - $CFFF:  4 KiB Work RAM (WRAM) Bank 0
$D000 - $DFFF:  4 KiB Work RAM (WRAM) Bank 1 (CGB: Banks 1-7 conmuted via SVBK $FF70)
$E000 - $FDFF:  Echo RAM (Mirror of $C000-$DDFF; writes affect WRAM)
$FE00 - $FE9F:  Object Attribute Memory (OAM: 40 sprites × 4 bytes = 160 bytes)
$FEA0 - $FEFF:  Prohibited / Unusable (Returns open-bus or $00/$FF depending on revision)
$FF00 - $FF7F:  Memory-Mapped I/O Registers (PPU, APU, Timers, Joypad, CGB registers)
$FF80 - $FFFE:  High RAM (HRAM, 127 bytes)
$FFFF:          Interrupt Enable Register (IE)
```

### 3.1 I/O Registers Allocation

- `$FF00`: `JOYP` (Joypad input register).
- `$FF01-$FF02`: `SB`, `SC` (Serial transfer data and control).
- `$FF04-$FF07`: `DIV`, `TIMA`, `TMA`, `TAC` (Hardware timers).
- `$FF0F`: `IF` (Interrupt Flag / request).
- `$FF10-$FF3F`: APU sound registers and Wave pattern RAM ($FF30-$FF3F).
- `$FF40-$FF4B`: PPU control, status, scroll, and LCD coordinates (`LCDC`, `STAT`, `SCY`, `SCX`, `LY`, `LYC`, `DMA`, `BGP`, `OBP0`, `OBP1`, `WY`, `WX`).
- `$FF4D`: `KEY1` (CGB CPU speed switch preparation).
- `$FF4F`: `VBK` (CGB VRAM Bank select: 0 or 1).
- `$FF51-$FF55`: `HDMA1-HDMA5` (CGB DMA source/dest and transfer trigger).
- `$FF68-$FF6B`: `BCPS`, `BCPD`, `OCPS`, `OCPD` (CGB Background and Object Palette Specifications and Data).
- `$FF70`: `SVBK` (CGB WRAM Bank select: 1..7; writing 0 selects bank 1).

---

## 4. Cartridge Mappers (MBCs)

The runtime provides clean C++ implementations for the 4 essential mappers:

### 4.1 ROM-Only (Type $00)
- Fixed 32 KiB ROM ($0000-$7FFF).
- Optional 8 KiB SRAM at $A000-$BFFF (rare).
- Writes to $0000-$7FFF are ignored.

### 4.2 MBC1 (Type $01, $02, $03)
- Supports up to 2 MiB ROM and 32 KiB RAM.
- **$0000-$1FFF:** RAM Enable (write `$0A` enables RAM, any other value disables).
- **$2000-$3FFF:** ROM Bank Number (lower 5 bits).
  - *Quirk:* Writing 0 maps to Bank 1 (banks 0, $20, $40, $60 cannot be mapped to $4000-$7FFF directly).
- **$4000-$5FFF:** RAM Bank Number / Upper 2 bits of ROM Bank Number.
- **$6000-$7FFF:** Banking Mode Select:
  - `0`: Simple ROM Banking Mode (fixed Bank 0 at $0000-$3FFF, up to 2 MiB ROM at $4000-$7FFF, RAM bank 0 only).
  - `1`: Advanced Banking Mode (upper 2 bits affect $0000-$3FFF and RAM banks).

### 4.3 MBC3 (Type $0F, $10, $11, $12, $13)
- Essential for *Pokémon Red/Blue/Yellow* and *Pokémon Gold/Silver*.
- Supports up to 2 MiB ROM, 32 KiB RAM, and **Real-Time Clock (RTC)**.
- **$0000-$1FFF:** RAM & Timer Enable (`$0A` enables).
- **$2000-$3FFF:** ROM Bank Number (7 bits, 1..127). Value 0 translates to 1.
- **$4000-$5FFF:** RAM Bank Number (0..3) or RTC Register Select ($08: Seconds, $09: Minutes, $0A: Hours, $0B: Day Low, $0C: Day High/Flags).
- **$6000-$7FFF:** Latch Clock Data (writing `0x00` then `0x01` latches current host time into RTC registers).

### 4.4 MBC5 (Type $19, $1A, $1B, $1C, $1D)
- Essential for *Pokémon Crystal* and the vast majority of Game Boy Color titles.
- Supports up to 8 MiB ROM and 128 KiB RAM.
- **$0000-$1FFF:** RAM Enable (`$0A` enables).
- **$2000-$2FFF:** 8 lower bits of ROM Bank Number.
- **$3000-$3FFF:** 9th bit of ROM Bank Number (allows addressing all 512 banks).
  - Unlike MBC1/3, Bank 00 **can** be mapped to $4000-$7FFF.
- **$4000-$5FFF:** RAM Bank Number (0..15).

---

## 5. PPU (LCD Controller)

### 5.1 Scanline-Based Catch-Up Model

Rendering is performed on a **scanline basis** with state catch-up on I/O writes:
- Screen dimensions: **160 × 144 pixels**.
- 154 scanlines per frame (Lines 0–143: visible frame; Lines 144–153: VBlank).
- Line duration: **456 T-cycles** (114 M-cycles). Total frame: 70,224 T-cycles (~59.7275 Hz).

```
Scanline (456 dots):
[ Mode 2: OAM Scan (80 dots) ] -> [ Mode 3: Pixel Transfer (172 dots) ] -> [ Mode 0: HBlank (204 dots) ]
Lines 144-153:
[ Mode 1: VBlank (456 dots per scanline) ]
```

- When the recompiled CPU runs, cycles accumulate. If an instruction writes to `$FF40` (`LCDC`), `$FF42` (`SCY`), `$FF43` (`SCX`), `$FF44` (`LY`), etc., the PPU catches up to the current CPU cycle.
- At the end of Mode 3 of each scanline (dot 252), the scanline pixels are rendered into the 160×144 32-bit RGBA framebuffer.

### 5.2 LCD Status & Interrupts (`STAT` $FF41)

- Bit 6: LYC=LY Coincidence Interrupt Enable.
- Bit 5: Mode 2 OAM Interrupt Enable.
- Bit 4: Mode 1 VBlank Interrupt Enable.
- Bit 3: Mode 0 HBlank Interrupt Enable.
- Bit 2: LYC=LY Coincidence Flag (1 if `LY == LYC`).
- Bits 1-0: Current Mode (0: HBlank, 1: VBlank, 2: OAM Scan, 3: Drawing).

### 5.3 Palettes

- **DMG Monochrome Palettes:**
  - `BGP` ($FF47), `OBP0` ($FF48), `OBP1` ($FF49).
  - Each 8-bit register defines four 2-bit shades: Dot Color 0 (bits 1-0), 1 (bits 3-2), 2 (bits 5-4), 3 (bits 7-6).
  - Configurable in runtime to classic pea-green DMG palette or high-contrast grayscale.
- **CGB Color Palettes:**
  - 8 Background palettes + 8 Sprite palettes, each with 4 colors of 15-bit RGB555 (`0bbbbbgggggrrrrr`).
  - Access via `BCPS` ($FF68) / `BCPD` ($FF69) and `OCPS` ($FF6A) / `OCPD` ($FF6B) with auto-increment bit support.

### 5.4 DMA Controllers

- **OAM DMA (`$FF46`):**
  - Writing value `$XX` copies 160 bytes from source address `$XX00-$XX9F` to OAM (`$FE00-$FE9F`).
  - Stalls CPU execution by 160 M-cycles (640 T-cycles).
- **CGB General DMA (GDMA) & HBlank DMA (HDMA) (`$FF51-$FF55`):**
  - **GDMA:** High-speed bulk transfer of `(length + 1) * 16` bytes from ROM/RAM to VRAM immediately, halting CPU.
  - **HDMA:** Transfers 16 bytes into VRAM during each HBlank (Mode 0) between scanlines 0–143.

---

## 6. APU (Audio Processing Unit)

Operates 4 hardware audio channels with sound registers mapped between `$FF10` and `$FF3F`:

1. **Channel 1 (Pulse 1):** Sweep frequency unit, variable length counter, volume envelope, and 4 duty cycle modes (12.5%, 25%, 50%, 75%).
2. **Channel 2 (Pulse 2):** Same as Channel 1 but without frequency sweep.
3. **Channel 3 (Wave):** Custom 32-sample 4-bit wave table stored in Wave RAM ($FF30-$FF3F).
4. **Channel 4 (Noise):** Pseudo-random noise generator with 7-bit or 15-bit Linear Feedback Shift Register (LFSR) and envelope.

- **Frame Sequencer:** Clocked at 512 Hz by the timer (`DIV` bit 4 / bit 5 in double speed) to step through Envelope (64 Hz), Sweep (128 Hz), and Sound Length (256 Hz).
- **Master Control:** `NR50` ($FF24, master volume / vin), `NR51` ($FF25, channel panning L/R), and `NR52` ($FF26, APU power control).

---

## 7. Timers & Hardware Interrupts

### 7.1 Timers
- **`DIV` ($FF04):** 16-bit internal counter incremented at 16384 Hz (every 256 T-cycles). Writing any value to `DIV` resets the counter to 0.
- **`TIMA` ($FF05):** Programmable timer counter.
- **`TMA` ($FF06):** Timer modulo reload value.
- **`TAC` ($FF07):** Timer control (Bit 2: Start/Stop; Bits 1-0: Clock select: 4096 Hz, 262144 Hz, 65536 Hz, 16384 Hz).
- When `TIMA` overflows, it reloads `TMA` after a 1 M-cycle delay and sets the Timer interrupt flag in `IF`.

### 7.2 Interrupt Priority & Vectors

When `IME == 1` and `(IE & IF & 0x1F) != 0`, the highest priority interrupt is dispatched:

| Priority | Bit | Interrupt Name | Vector | Trigger Condition |
| :---: | :---: | :--- | :---: | :--- |
| 1 | 0 | **VBlank** | `$0040` | Scanline transitions from 143 to 144 |
| 2 | 1 | **LCD STAT** | `$0048` | Configured condition in `STAT` matches |
| 3 | 2 | **Timer** | `$0050` | `TIMA` counter overflows |
| 4 | 3 | **Serial** | `$0058` | Serial 8-bit shift transfer completes |
| 5 | 4 | **Joypad** | `$0060` | High-to-low transition on joypad input pins |

---

## 8. Static Recompiler (`gbrecomp`)

### 8.1 Symbol Naming Conventions

Generated C symbols follow DragonRecomp global conventions:

| Symbol | Address Space | Example |
| :--- | :--- | :--- |
| `func_XXXX` | Fixed ROM Bank 00 ($0000 - $3FFF) | `func_0150`, `func_0040` |
| `func_BB_XXXX` | Switchable ROM Bank $01..$NN ($4000 - $7FFF) | `func_01_4000`, `func_0A_6B20` |
| `L_XXXX` | Intra-function basic block labels | `L_0158`, `L_42A0` |

### 8.2 Recursive Descent & Entry Points

The disassembler seeds analysis from:
1. Header entry point: `$0100`.
2. Hardware interrupt vectors: `$0040`, `$0048`, `$0050`, `$0058`, `$0060`.
3. RST vectors: `$0000`, `$0008`, `$0010`, `$0018`, `$0020`, `$0028`, `$0030`, `$0038`.
4. Configured seeds: `[[entry_points]]` in `<game>.toml`.
5. Jump table targets: `[[jump_tables]]` and `[[jump_engine]]` inline tables.
6. Interpreter fallback log: dynamically discovered `(bank, addr)` pairs.

### 8.3 C99 Emitter Format

Each guest function translates into a standard C function:

```c
#include "gb/runtime.h"

void func_01_4A20(CpuState *cpu) {
    /* Stack trick unwinding detection */
    uint16_t const entry_sp = cpu->sp;
    
    L_4A20:
        /* LD A, ($FF44) -> read LY */
        cpu->a = gb_bus_read8(cpu, 0xFF44U);
        cpu->cycles += 12;
        /* CP 144 */
        gb_cp8(cpu, 144U);
        cpu->cycles += 8;
        /* JR C, L_4A20 */
        if (cpu->f & GB_FLAG_C) {
            cpu->cycles += 12;
            goto L_4A20;
        }
        cpu->cycles += 8;
        
        /* Check if runtime needs catch-up/interrupt dispatch */
        if (gb_runtime_poll(cpu)) {
            if (cpu->halted || cpu->sp > entry_sp) {
                return; /* Unwind call frame if stack was manipulated */
            }
        }
        
    L_4A32:
        /* RET */
        gb_ret(cpu);
        return;
}
```

---

## 9. DragonRecomp Studio Integration

Game Boy and Game Boy Color are first-class targets in **DragonRecomp Studio**:

1. **Auto-Detection:** Files ending in `.gb` or `.gbc` are inspected automatically on drag-and-drop.
2. **Metadata Extraction:** Title, cartridge type (MBC), ROM/RAM sizes, and SHA-1 checksum are derived from the header.
3. **Automatic Config Generation:** Studio generates `gb/games/<game>/<game>.toml`:
   ```toml
   [game]
   title = "Pokemon Red"
   system = "gb"              # "gb" or "gbc"
   sha1 = "ea9bcae617fdf159b045185467ae58b2e4a48b9a"
   mbc = "mbc3"
   rom_size_kb = 1024
   ram_size_kb = 32
   
   [[entry_points]]
   addr = 0x0100
   bank = 0
   name = "Entry"
   ```
4. **Autonomous Self-Learning:** The Studio monitors `gb_fallback.log`, parses uncompiled calls `FALLBACK: bank=0x02 addr=0x4120`, deduplicates structurally via `(bank, addr)`, and appends to the game config for hot re-compilation.
5. **Packager Support:** One-click generation of portable Linux binary packages and Windows `.exe` bundles with bundled SDL3.

---

## 10. Milestones & Verification Criteria

| ID | Milestone | Key Acceptance Criterion |
| :---: | :--- | :--- |
| **G1** | SM83 CPU Interpreter + Bus + ROM-Only/MBC1/MBC3/MBC5 Loader | Passes Blargg's `cpu_instrs.gb` test suite in interpreter mode. |
| **G2** | PPU (Scanline renderer) + Palettes (DMG/CGB) + Joypad + SDL3 Frontend | *Tetris* and *Super Mario Land* fully playable in interpreter mode. |
| **G3** | `gbrecomp` tool + C99 Emitter + CFG Builder | *Tetris* and *Pokemon Red/Blue* recompiled natively with 0 fallbacks. |
| **G4** | APU (4 channels) + MBC3 RTC + CGB Double Speed + HDMA | *Pokemon Gold/Silver/Crystal* boot, audio functional, save & clock operational. |
| **G5** | DragonRecomp Studio Integration | Drag-and-drop `.gb`/`.gbc`, auto-pipeline, auto-repair, and Linux/Windows packager. |
