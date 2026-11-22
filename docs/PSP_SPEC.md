# PSP_SPEC.md — PlayStation Portable (`psprecomp` & `psp/runtime`)

Especificación técnica para la recompilación estática de juegos y homebrew de **Sony PlayStation Portable (PSP)** a C99 nativo dentro de **DragonRecomp**.

---

## 1. Arquitectura de Hardware PSP

| Subsistema | Detalle técnico |
| :--- | :--- |
| **CPU principal** | **Sony Allegrex** (MIPS32 R1/R2 custom, little-endian, 1–333 MHz, 32 GPRs de 32 bits, HI/LO, delay slots) |
| **Extensiones Allegrex** | `clz`/`clo` (`SPECIAL` `0x16`/`0x17`), `max`/`min` (`SPECIAL` `0x2C`/`0x2D`), `madd`/`maddu`/`msub`/`msubu`, `movz`/`movn`, `rotr`/`rotrv`, `ext`/`ins`, `wsbh`/`wsbw`/`seb`/`seh`/`bitrev` (`SPECIAL3` `0x20`) |
| **FPU (COP1)** | IEEE-754 single-precision (`f0`–`f31`, `fcr31` con bit `CC` 23), `lwc1`/`swc1`, `mfc1`/`mtc1`/`cfc1`/`ctc1`, `bc1f`/`bc1t`/`bc1fl`/`bc1tl` |
| **VFPU (COP2)** | 128 registros flotantes de 32 bits (8 matrices `4×4`), `lv.s`/`sv.s`, `lv.q`/`sv.q`, `lv.l/r`, `sv.l/r`, operaciones vectoriales y matriciales |
| **Memoria** | **Scratchpad:** 16 KiB (`0x00010000–0x00013FFF`) · **VRAM:** 2 MiB (`0x04000000–0x041FFFFF`, espejada en `0x04200000–0x047FFFFF` / `0x44000000`) · **Main RAM:** 32 MiB PSP-1000 / 64 MiB PSP-2000+ (`0x08000000–0x0BFFFFFF`, User RAM desde `0x08800000`) |
| **Video (GE + Display)** | Pantalla nativa **480×272** (16:9), stride típico 512 px; formatos `RGB565`, `RGBA5551`, `RGBA4444`, `RGBA8888`. Motor **Graphics Engine (GE)** por Display Lists de comandos de 32 bits (`opcode[31:24]`, `arg[23:0]`), transformaciones World/View/Proj, vértices 8/16/float, CLUT y texturas swizzled (conectado a `common/gpu3d`) |
| **Audio** | 8 canales hardware HLE vía `sceAudio` (PCM 16-bit estéreo/mono, 44.1 kHz / 48 kHz) + mezclador estéreo SDL3 |
| **Controles** | D-Pad, Stick analógico (`Lx`, `Ly` 0–255, centro 128), `×`, `○`, `□`, `△`, `L`, `R`, `Start`, `Select`, `Home` vía `sceCtrl` |
| **Formatos soportados** | `.elf` (ejecutable ELF/PRX sin cifrar), `.pbp` (EBOOT.PBP con `PARAM.SFO` + `DATA.PSP`), `.iso` (UMD ISO9660 con `PSP_GAME/SYSDIR/BOOT.BIN` / `EBOOT.BIN`), `.cso` (CISO comprimido con zlib) |

---

## 2. Mapa de Memoria y Traducción de Direcciones

Todas las direcciones virtuales MIPS (`KUSEG` `0x00000000`, `KSEG0` `0x80000000`, `KSEG1` `0xA0000000` y bit uncached `0x40000000`) se normalizan mediante `phys = addr & 0x3FFFFFFF`:

- `0x00010000 – 0x00013FFF`: Scratchpad RAM (16 KiB)
- `0x04000000 – 0x041FFFFF`: VRAM (2 MiB, espejada en `0x04200000–0x047FFFFF` con `& 0x001FFFFF`)
- `0x08000000 – 0x0BFFFFFF`: Main RAM (64 MiB totales; partición kernel `0x08000000–0x087FFFFF`, partición usuario `0x08800000–0x0BFFFFFF`)

---

## 3. Carga de Ejecutables y Resolución de NIDs (HLE)

1. **Contenedor:**
   - **PBP (`\0PBP`):** extrae `PARAM.SFO` (título/DISC_ID) y el segmento `DATA.PSP` (offset tabla `[6]`).
   - **ISO / CSO:** monta el sistema de archivos ISO9660 (descomprimiendo bloques de 2 KiB vía `zlib` en `.cso`), lee `PSP_GAME/PARAM.SFO` y carga `PSP_GAME/SYSDIR/BOOT.BIN` o `EBOOT.BIN` (ELF/PRX).
   - **ELF / PRX:** carga segmentos `PT_LOAD`, aplica relocaciones PRX (`SHT_PRXRELOC` tipo `0x700000A0` cuando `e_type == 0xFFA0`, base por defecto `0x08804000`), localiza `.rodata.sceModuleInfo` (`SceModuleInfo`) y recorre `.lib.stub` (`stub_top..stub_end`).
2. **Parcheo de Stubs a Syscalls HLE:**
   - Cada entrada de `PspModuleImport` vincula un `library_name` y un arreglo de hashes **NID** de 32 bits con los stubs de 8 bytes en `.text`.
   - El cargador registra cada par `(library_name, nid)` en la tabla de despacho HLE y reemplaza el stub por:
     ```mips
     jr      $ra
     syscall <hle_index>
     ```
   - Tanto el intérprete como el código C99 recompilado ejecutan `psp_hle_syscall(cpu, code20)` en el delay slot / stub, logrando 100 % paridad entre intérprete y recompilación estática.
3. **Módulos HLE principales:**
   - `ThreadManForUser`: `sceKernelCreateThread`, `sceKernelStartThread`, `sceKernelExitThread`, `sceKernelExitDeleteThread`, `sceKernelSleepThread`, `sceKernelSleepThreadCB`, `sceKernelDelayThread`, `sceKernelDelayThreadCB`, `sceKernelCreateCallback`, `sceKernelCreateSema`, `sceKernelSignalSema`, `sceKernelWaitSema`, `sceKernelDeleteSema`, `sceKernelGetSystemTimeLow`, `sceKernelGetSystemTimeWide`.
   - `LoadExecForUser`: `sceKernelRegisterExitCallback`, `sceKernelExitGame`.
   - `SysMemUserForUser`: `sceKernelAllocPartitionMemory`, `sceKernelGetBlockHeadAddr`, `sceKernelFreePartitionMemory`, `sceKernelMaxFreeMemSize`, `sceKernelTotalFreeMemSize`.
   - `sceDisplay`: `sceDisplaySetMode`, `sceDisplaySetFrameBuf`, `sceDisplayGetFrameBuf`, `sceDisplayWaitVblank`, `sceDisplayWaitVblankStart`, `sceDisplayWaitVblankCB`, `sceDisplayWaitVblankStartCB`, `sceDisplayGetVcount`.
   - `sceCtrl`: `sceCtrlSetSamplingCycle`, `sceCtrlSetSamplingMode`, `sceCtrlReadBufferPositive`, `sceCtrlPeekBufferPositive`, `sceCtrlReadLatch`, `sceCtrlPeekLatch`.
   - `sceGe_user`: `sceGeEdramGetAddr`, `sceGeEdramGetSize`, `sceGeListEnQueue`, `sceGeListEnQueueHead`, `sceGeListUpdateStallAddr`, `sceGeListSync`, `sceGeDrawSync`, `sceGeSetCallback`, `sceGeUnsetCallback`.
   - `sceAudio`: `sceAudioChReserve`, `sceAudioChRelease`, `sceAudioOutput`, `sceAudioOutputBlocking`, `sceAudioOutputPanned`, `sceAudioOutputPannedBlocking`, `sceAudioChangeChannelVolume`, `sceAudioSetChannelDataLen`.
   - `IoFileMgrForUser`: `sceIoOpen`, `sceIoClose`, `sceIoRead`, `sceIoWrite`, `sceIoLseek`, `sceIoLseek32`, `sceIoGetstat` (sobre ISO/UMD `disc0:/` y directorio `ms0:/`).
   - `UtilsForUser`: `sceKernelDcacheWritebackAll`, `sceKernelDcacheWritebackRange`, `sceKernelDcacheWritebackInvalidateAll`, `sceKernelDcacheWritebackInvalidateRange`, `sceKernelIcacheInvalidateAll`, `sceKernelLibcTime`, `sceKernelLibcClock`, `sceKernelLibcGettimeofday`.
   - `sceRtc`: `sceRtcGetCurrentClockLocalTime`, `sceRtcGetCurrentTick`, `sceRtcGetTickResolution`.

---

## 4. Recompilador Estático (`psprecomp`)

- **Núcleo compartido:** utiliza `common/mips` (`dragon::mips::decode_mips(..., MipsProfile::Allegrex)`).
- **Descubrimiento CFG:** desde `e_entry` (y opcionalmente símbolos ELF / stubs), desensambla recursivamente siguiendo `j`, `jal`, ramas condicionales (incluyendo delay slots y branch-likely), tablas de salto y funciones detectadas por prólogo (`addiu $sp, $sp, -N`).
- **Emisión C99:**
  - Funciones nombradas `func_08XXXXXX(PspCpuState* cpu)`.
  - Manejo correcto de **delay slots** (la condición/objetivo del salto se evalúa antes de ejecutar la instrucción del delay slot; en ramas *likely* el delay slot solo se ejecuta si la rama se toma).
  - Despacho indirecto (`jr`/`jalr`) vía `psp_runtime_dispatch(cpu, target, pc)` con fallback transparente al intérprete si el destino es código dinámico o no descubierto.
  - Los stubs HLE emiten directamente `psp_hle_syscall(cpu, code20); return;`.

---

## 5. Hitos de Aceptación (`PSP1`–`PSP5`)

| Hito | Alcance | Criterio de aceptación |
| :--- | :--- | :--- |
| **PSP1** | SPEC, `knowledge/psp.toml` y `common/mips` | `dragon_mips_tests` pasa (MIPS32 + Allegrex + FPU + VFPU + R4300i hooks) |
| **PSP2** | Cargador ELF/PBP/ISO/CSO + CPU + HLE NIDs | `psp_cpu_unit_tests` y `psp_hle_unit_tests` pasan |
| **PSP3** | Motor GE 3D sobre `common/gpu3d` + `sceAudio` + SDL3 480×272 | `psp_ge_unit_tests` pasa; Display Lists 3D y 2D renderizan a 480×272 |
| **PSP4** | Recompilador estático `psprecomp` (MIPS32 → C99) | `psp_recomp_unit_tests` pasa; binario ELF sintético recompila y ejecuta idéntico al intérprete |
| **PSP5** | Integración DragonRecomp Studio + empaquetador | Detección `.iso`/`.cso`/`.pbp`/`.elf`, pipeline, controles, carátulas y `studio_unit_tests` pasan |
