# ADR-008: Contrato E2E de tres niveles con golden-hash y verify como equivalencia

## Contexto
"E2E" mezcla garantías distintas. Las aserciones actuales son débiles
(`dk_boot_test`: `distinct_colors >= 5`, `non_backdrop_pixels > 4000` → prueba
"pintó algo", no "pintó lo correcto"). `--verify` existe solo en NES y SNES,
compara únicamente RAM + framebuffer, y GB/GBA —las arquitecturas más frágiles—
no tienen ninguna prueba de equivalencia.

## Opciones consideradas
1. **Mantener boot tests con aserciones heurísticas**. Contra: falsos verdes ante
   corrupción; sin equivalencia; sin cobertura GB/GBA.
2. **Solo lock-step verify global**. Pro: equivalencia fuerte. Contra: no cubre
   el nivel sintético ni la precisión de hardware (ROMs públicas), y no da un
   "golden" estable de pantalla.
3. **Tres niveles**: Nivel 0 ROM sintética (pipeline completo, CI), Nivel 1 ROMs
   públicas de precisión (CI), Nivel 2 juego comercial con golden-hash de
   framebuffer **y** `--verify` lock-step (local). Extender `--verify` a GB/GBA y
   a audio + registros PPU/IO.

## Decisión
**Opción 3.** Golden-hash = CRC/SHA de framebuffer en checkpoints (se committea el
**hash**, nunca un PNG: el hash es derivado y no reproduce la obra). El Nivel 2
exige 0 fallbacks (o presupuesto declarado), input scripteado determinista,
golden-hash en checkpoints y equivalencia lock-step en los primeros N frames. Si
la extensión de `--verify` (audio/regs) no se completa, su `--help`/docs dejan de
anunciarlo como prueba de equivalencia.

## Consecuencias
- "Pintó algo" → "pintó exactamente la referencia conocida".
- GB y GBA ganan red de seguridad de corrección por primera vez.
- Golden-hash y firmas de ROMs públicas son committeables y legales.
- Juegos `interpreter_only` (Emerald, SMW) usan contrato "arranca sin cuelgue",
  no golden-hash de recompilación nativa.

## Spec relacionada
`specs/003-ci-e2e-verificacion/`
