# ADR-007: Resolución de ROMs por `DRAGON_ROM_DIR` y principio "skip ≠ pass"

## Contexto
Los tests buscan la ROM en rutas fijas (`${CMAKE_SOURCE_DIR}/roms/nes/smb.nes`) y,
si falta, hacen `return 0` → CTest los cuenta como **passed**. Con `roms/`
gitignored, en un clon limpio los E2E que sostienen la tesis pasan por saltarse,
no por funcionar. Además la biblioteca del autor usa nombres distintos
(`Super Mario Bros. (Europe).nes`, `Donkey Kong (JU).nes`), no `smb.nes`.

## Opciones consideradas
1. **Rutas fijas + skip con `return 0`** (actual). Contra: skip indistinguible de
   pass; nombres rígidos; no usa la biblioteca real.
2. **Copiar/symlink las ROMs a `roms/`**. Contra: duplica datos fuera de git,
   frágil, y roza "modificar fuera del repo".
3. **`DRAGON_ROM_DIR` + resolución por SHA-1 + skip real** — variable de entorno
   (default `roms/`) y búsqueda por hash dentro de ella; tests devuelven `77`
   (SKIP_RETURN_CODE) y `DRAGON_REQUIRE_ROMS=1` convierte skip en fallo.

## Decisión
**Opción 3.** Helper `find_rom_by_sha1(dir, sha1)` en `common/`. Los configs ya
llevan `sha1`, así que la resolución es por contenido, no por nombre. CTest
reporta *Skipped* (no *Passed*) sin ROM; en el nivel local, `DRAGON_REQUIRE_ROMS=1`
hace que una ROM ausente **falle**. Nunca se committean ROMs comerciales ni
capturas; solo hashes golden y las ROMs públicas del Nivel 1.

## Consecuencias
- "N passed, M skipped" es honesto en hosted; el nivel local falla si falta una
  ROM esperada.
- La biblioteca del autor se usa tal cual, por SHA-1, sin tocarla (HARD RULE).
- Requiere descomprimir ROMs comprimidas (p. ej. SMB SNES `.zip`) a un dir
  temporal en tiempo de test, nunca en la biblioteca.

## Spec relacionada
`specs/003-ci-e2e-verificacion/`
