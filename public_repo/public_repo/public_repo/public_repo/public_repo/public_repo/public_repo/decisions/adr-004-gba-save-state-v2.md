# ADR-004: GBA Save State v2 con payload de hardware completo y retrocompatibilidad

## Contexto
El formato de Save States de DragonRecomp (`DRST`) en GBA solo serializaba el estado de la CPU, las memorias RAM y los búferes de vídeo. Los 1024 bytes de registros I/O (`io_regs_`), temporizadores de hardware, canales DMA y registros de interrupción (`IME`, `IE`, `IF`) quedaban sin guardar. Al cargar un save state (F7), la desincronización entre la CPU y el hardware producía cuelgues o congelamientos.

## Decisión
1. Incrementar el número de versión en `SaveStateHeader` a `version = 2` para el sistema `"GBA "`.
2. Definir una estructura `GbaStatePayload` que serialice:
   - Estado de CPU (`CpuState`).
   - Registros I/O (`io_regs_`, 1024 bytes).
   - Registros y estado interno de los 4 Timers (`timers_`).
   - Registros de los 4 canales DMA (`dma_`).
   - Flags de interrupción (`ime_`, `ie_`, `if_`).
   - PPU (registros y estado de scanline) y memorias RAM (EWRAM, IWRAM, SRAM, EEPROM, Palette, VRAM, OAM).
3. En `Bus::load_state`, admitir tanto `version == 2` (completo) como `version == 1` (legacy, emitiendo advertencia informativa), garantizando que las partidas previas no se pierdan.

## Consecuencias
- Carga y guardado de partidas en GBA 100% confiable y sin congelamiento.
- Compatibilidad hacia atrás asegurada.
