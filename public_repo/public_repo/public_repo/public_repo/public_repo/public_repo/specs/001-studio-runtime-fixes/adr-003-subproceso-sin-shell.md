# ADR-003: Ejecución de subprocesos sin shell (argv explícito)

## Contexto
El Studio lanza clang/clang++/recompilador con `popen(cmd, "r")` (4 sitios) y
el juego con `fork()/execv()` manual. Los comandos de `popen` se arman
concatenando rutas de ROM y títulos en una cadena de shell con comillas
ingenuas → ruptura e inyección (`/bin/sh`) con nombres que contienen `"`,
`` ` `` o `$`. Además el hijo del `fork` ejecuta código no async-signal-safe.

## Opciones consideradas
1. **Helper argv sin shell** (`posix_spawn` o `fork`+`execvp` con vector de
   argumentos y `pipe` de captura) — Pro: elimina la inyección (los args son
   datos, no shell), corrige la seguridad async-signal (argv se arma antes del
   fork), unifica 5 sitios en uno, errores siempre poblados. Contra: algo más
   de código que un `popen`.
2. **Seguir con `popen` pero escapando** — escapar comillas/metacaracteres de
   shell manualmente. Pro: cambio menor. Contra: el escaping de shell es
   propenso a errores y sigue invocando `/bin/sh`; no corrige el problema
   async-signal del `launch_game`.

## Decisión
**Opción 1**: un helper de subproceso común sin shell, usado por la
compilación, el recompilador y el lanzamiento del juego. Corrige F5
(inyección), F6 (async-signal-safe) y F8 (error siempre poblado) de una vez y
reduce duplicación.

## Consecuencias
- Nuevo helper interno en el Studio (`run_process` + variante con pid para el
  juego). Patrón a seguir por futuros lanzamientos de procesos.
- `-DDRAGON_GAME_TITLE=<valor>` pasa como un único elemento argv, sin comillas
  anidadas; desaparece el frágil `"\\\"...\\\""`.
- Convención emergente para CLAUDE.md/AGENTS.md: "en el Studio, lanzar procesos
  con argv explícito vía el helper, nunca `popen`/shell".

## Spec relacionada
`specs/001-studio-runtime-fixes/`
