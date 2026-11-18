# Notas de Viabilidad y Arquitectura: Recompilación Estática de Nintendo Switch (`nxrecomp`)

Fecha: 2026-11-15  
Estado: **Investigación / Referencia Técnica**

Este documento conserva el análisis técnico sobre la viabilidad, retos y atajos arquitectónicos para construir un recompilador estático de juegos de **Nintendo Switch (ARM64 / Horizon OS / Maxwell GPU)** hacia ejecutables nativos de PC (`x86_64` en Linux/Windows), en comparación con los targets actuales de DragonRecomp (`nes`, `snes`, `gb`, `gba`) y con los enfoques existentes en macOS (Apple Silicon).

---

## 1. Diferencias Clave Frente a Consolas Retro (NES / SNES / GB / GBA)

### Lo que es MÁS FÁCIL en Switch:
1. **Descubrimiento de funciones (CFG) casi exacto:**
   * Los binarios de Switch son archivos **NSO** (derivados de **ELF de 64 bits** compilados con Clang/LLVM).
   * Tienen separación estricta entre código (`.text`), constantes (`.rodata`) y datos (`.data`).
   * Incluyen tablas `.eh_frame` (Unwind Info) y `.dynsym` que delimitan el inicio y fin de prácticamente todas las funciones del binario sin ambigüedad entre código y datos.
2. **Arquitectura AArch64 (ARMv8-A 64-bit) limpia:**
   * 31 registros de propósito general de 64 bits (`X0`–`X30`) + 32 registros vectoriales/SIMD de 128 bits (`V0`–`V31` NEON).
   * Sin modos de instrucción mezclados (como ARM/Thumb en GBA) ni flags de ancho dinámico (como M/X en el 65816 de SNES).
   * Convención de llamadas estándar (**AAPCS64**).
3. **Sin sincronización ciclo a ciclo:**
   * No requiere conteo estricto de ciclos por instrucción ni emulación de *raster effects* por scanline. Opera sobre hilos del sistema operativo y espacio de memoria virtual plano de 64 bits.

### Lo que es MUCHO MÁS DIFÍCIL (Cuellos de Botella):
1. **GPU Programable (NVIDIA Maxwell Tegra X1) y Shaders (`NVN` $\rightarrow$ Vulkan):**
   * No es una PPU de función fija con pocos registros; los juegos envían *command buffers* mediante la API propietaria **NVN**.
   * **Recompilación de Shaders:** Los shaders vienen precompilados en bytecode propietario de NVIDIA (**Maxwell SASS**). Requiere un decompilador/traductor de **Maxwell SASS $\rightarrow$ SPIR-V / Vulkan** (estático para shaders en RomFS y con caché/JIT en runtime para shaders dinámicos).
2. **Sistema Operativo (Horizon OS + `nnSdk`):**
   * En vez de hablar directo con el hardware, el juego usa **SVCs (Supervisor Calls)** y servicios IPC (`nn::fs`, `nn::hid`, `nn::audio`, `nn::vi`, `nn::oe`, sockets, etc.).
   * **Multithreading real:** Uso de 3 a 4 núcleos con primitivas atómicas (`LDXR`/`STXR`, `CAS`), mutexes, semáforos y *fibers*, que deben mapearse a `std::atomic` y `std::thread`.
3. **Escala y Cifrado:**
   * Los dumps `.nsp`/`.xci` requieren desencriptación previa (`prod.keys`) para extraer el `ExeFS` (`rtld`, `main`, `subsdk*`, `sdk`) y el `RomFS`.
   * Un título comercial grande contiene entre **100,000 y 500,000 funciones**, por lo que el emisor debe particionar el código generado en cientos de archivos `.c`/`.cpp` para compilación paralela.

---

## 2. ¿Por qué no sirve directamente el enfoque de Mac (Apple Silicon)?

1. **NCE / Hypervisor en Mac (Ryujinx / Yuzu en M1/M2/M3/M4):**
   * Tanto la Switch (Tegra X1) como los chips Apple Silicon usan exactamente la misma arquitectura de CPU: **ARM64 (AArch64)**.
   * En Mac no se recompila el CPU: usan `Hypervisor.framework` / **NCE (Native Code Execution)** para ejecutar las instrucciones del juego directamente en el silicio del Mac a velocidad 1:1, interceptando solo las llamadas al OS y traduciendo GPU a Metal (MoltenVK).
   * **En PC (`x86_64` Intel/AMD):** El procesador no entiende instrucciones ARM64 de forma nativa, por lo que **es obligatorio traducir/recompilar** cada instrucción ARM64 + NEON a C/C++ o `x86_64`.
2. **Rosetta 2 (Traductor AOT de Apple):**
   * Rosetta 2 recompila estáticamente en la dirección opuesta (**`x86_64` $\rightarrow$ `ARM64`**) para correr apps de Intel en Macs ARM, además de ser software propietario cerrado. Para Switch en PC se necesita la dirección inversa (**`ARM64` $\rightarrow$ `x86_64`**).

---

## 3. Atajos Arquitectónicos y Herramientas Reutilizables para `nxrecomp`

Para que un recompilador estático de Switch a PC sea viable sin invertir años reescribiendo todo desde cero:

1. **Lifting de CPU y SIMD (`AArch64 + NEON` $\rightarrow$ `C / x86_64`):**
   * **LLVM Remill / McSema:** Librerías open-source que ya levantan instrucciones **AArch64 a LLVM IR**, permitiendo emitir binarios `x86_64` optimizados con Clang o código C sin programar a mano miles de opcodes de ARMv8.
   * **SIMDe (SIMD Everywhere):** Librería *header-only* que traduce intrínsecos de **ARM NEON (128-bit)** directamente a **AVX2 / SSE de `x86_64`** en C/C++ con rendimiento nativo.
2. **Intercepción HLE a nivel de `nnSdk` (en vez de emular el Kernel Horizon):**
   * Casi todos los juegos comerciales enlazan dinámicamente contra `nnSdk` (presente como un binario `.nso` separado en `ExeFS`).
   * En lugar de recompilar `nnSdk` y emular los SVCs del kernel, el recompilador puede **interceptar los símbolos exportados de `nnSdk`** (`nn::fs::ReadFile`, `nn::hid::GetNpadState`, `nvn*`, etc.) y redirigirlos directamente a funciones nativas de PC (C stdio, SDL3, Vulkan).
3. **Backend de GPU y Shaders (`NVN` / Maxwell SASS $\rightarrow$ Vulkan):**
   * Reutilizar decompiladores open-source existentes de **Maxwell SASS $\rightarrow$ SPIR-V** (como `sirit` / decompiladores de shaders de la escena de Switch en C++) y mapear la API **NVN** directamente sobre **Vulkan** (ambas APIs tienen un diseño de bajo nivel casi 1:1).
