# ADR-006: Topología de CI en dos niveles

## Contexto
No existe CI (`.github/workflows/` ausente). "Build limpio y tests en verde" solo
se verifica en la máquina del autor, con sus ROMs. Gemini también commitea a
`main` sin gate. Las ROMs comerciales no pueden committearse (legal + `.gitignore`),
así que un único CId hosted no puede correr los boot tests de juegos reales.

## Opciones consideradas
1. **Solo CI hosted** (GitHub Actions). Pro: protege contra regresiones de
   cualquier committer en entorno limpio. Contra: no puede ejecutar los E2E de
   juegos comerciales (sin ROMs).
2. **Solo local/pre-push**. Pro: usa la biblioteca de ROMs del autor; cero
   infraestructura. Contra: no fuerza nada (bypasseable con `--no-verify`), no
   protege de otros committers (Gemini), y "funciona en mi máquina" se cuela.
3. **Dos niveles** — hosted para todo lo que no necesita ROM comercial;
   local/self-hosted para los E2E ROM-gated. Pro: cada test en el nivel donde es
   legal y reproducible; red de seguridad real + verificación de juegos reales.
   Contra: dos pipelines que mantener.

## Decisión
**Opción 3.** Nivel A (GitHub Actions hosted): matriz Linux `{clang, gcc}` +
Windows MSVC, build + unit tests + E2E de Nivel 0 (sintético) y Nivel 1 (ROMs
públicas). Nivel B (local/self-hosted, `workflow_dispatch`/nocturno o git hook
`pre-push`): E2E de Nivel 2 con la biblioteca de ROMs vía `DRAGON_ROM_DIR` y
`DRAGON_REQUIRE_ROMS=1`.

## Consecuencias
- El Nivel A es la verdad compartida y bloquea regresiones de todos los committers.
- El Nivel B no sustituye al A (es opcional y bypasseable); valida juegos reales.
- Primera verificación de la compilación Windows que M6 afirma soportar.

## Spec relacionada
`specs/003-ci-e2e-verificacion/`
