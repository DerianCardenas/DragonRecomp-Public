# ADR-001: Diagnóstico/auto-reparación → hacerlo honesto

## Contexto
`diagnose_and_repair` (`studio/src/pipeline.cpp`) promete reparar 11 síntomas,
pero siempre devuelve `success=true` con textos que afirman arreglos de
hardware que no ocurren; varias ramas son recompilaciones sin cambios. Hay que
decidir qué hacer con la feature.

## Opciones consideradas
1. **Hacerlo honesto** — reportar solo la acción real y su resultado; quitar
   los textos y el feedback falsos. Pro: esfuerzo bajo, elimina el engaño,
   conserva lo útil (recompilar / toggle mirroring). Contra: la feature sigue
   siendo modesta (no "diagnostica" de verdad).
2. **Diagnóstico real basado en señales** — detectar por logs del runtime,
   estado de framebuffer/audio, y aplicar fixes dirigidos por síntoma. Pro:
   cumpliría la promesa. Contra: mucho trabajo, incierto, superficie grande,
   fuera del alcance de un fix de bugs.
3. **Eliminar la feature** — quitar el asistente y su UI. Pro: elimina el
   engaño sin mantener código dudoso. Contra: pierde la UX anunciada y el
   trabajo ya hecho de toggle mirroring / reintento.

## Decisión
**Opción 1 (honesto)**, elegida por el usuario. El diagnóstico reporta la
acción efectiva ("se integraron N rutinas y se recompiló", "se alternó
mirroring", "no hay auto-fix para este síntoma") y respeta la asincronía del
pipeline (no afirma "reparado" instantáneo). La opción 2 queda como posible
spec futura.

## Consecuencias
- La UI debe reflejar el resultado del pipeline (`Ready`/`Failed`), no un "✔"
  fijo. `app.cpp` cambia en consecuencia.
- Se deja de escribir "reparado" en `*_feedback.toml`.
- Deuda futura: si se quiere un diagnóstico de verdad, se abre nueva spec
  (opción 2), ahora sobre una base honesta.

## Spec relacionada
`specs/001-studio-runtime-fixes/`
