# ADR-002: Targets de motor-de-saltos/truco-RTS por configuración (quitar 0x0884)

## Contexto
`runtime_dispatch` del runtime SNES (`snes/runtime/src/bus.cpp:935`) tiene
`target != 0x0884U` hardcodeado para que el RTS del motor de saltos de SMB SNES
no termine el despacho. Es un parche específico de un juego en infraestructura
compartida y no comprueba el banco. Hay que decidir cómo generalizarlo.

## Opciones consideradas
1. **Tabla generada por el recompilador** — `nesrecomp/snesrecomp` emite
   `kRecompJumpEngineTargets[] = {{bank, addr}, …}` desde el TOML
   (`jump_engine`/`inline_data_calls`/`stack_tricks`), y el runtime la consulta
   vía `Bus::is_jump_engine_target(bank, addr)`. Pro: runtime genérico, fuente
   de verdad en config+recompilador (coherente con `kRecompDispatchTable`).
   Contra: toca el emisor SNES.
2. **Registro desde el `main` por juego** — `snes_recomp_game_main.cpp` lee la
   config en runtime y llama `bus.set_jump_engine_targets(...)`. Pro: no toca
   el emisor. Contra: el `main` por juego debe cargar/parsear config al
   arrancar; dependencia extra en runtime.
3. **Mantener el literal** — descartada: es la causa del bug.

## Decisión
**Opción 1 (tabla generada)** como preferida, por coherencia con
`kRecompDispatchTable` y runtime genérico. La opción 2 es alternativa válida si
se prefiere no tocar el emisor. En ambos casos: comprobar **banco + addr**, y
una **prueba de regresión obligatoria** (`snes_smb_snes_boot_test`) que
confirme comportamiento idéntico de SMB SNES antes de retirar el `0x0884`.

## Consecuencias
- Nuevo contrato pequeño config→runtime (tabla o setter). Documentar en
  `docs/SNES_SPEC.md` el origen de los targets de motor de saltos.
- Riesgo medio: cambia control de flujo del despacho. Mitigado por el test de
  regresión de SMB SNES.
- Primero hay que **averiguar qué es 0x0884 en SMB SNES** (banco, ROM/RAM, por
  qué es jump-engine) para mapearlo a la entrada de config correcta.

## Spec relacionada
`specs/001-studio-runtime-fixes/`
