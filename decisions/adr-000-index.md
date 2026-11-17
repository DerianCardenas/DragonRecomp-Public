# Índice de ADRs

Registro global y secuencial de decisiones de arquitectura (ver
`spec-methodology`). El número es único a nivel de proyecto, no por spec.

| ADR | Título | Estado | Spec |
|-----|--------|--------|------|
| [ADR-001](../specs/001-studio-runtime-fixes/adr-001-diagnostico-honesto.md) | Diagnóstico/auto-reparación → hacerlo honesto | Implementado | 001-studio-runtime-fixes |
| [ADR-002](../specs/001-studio-runtime-fixes/adr-002-jump-engine-por-config.md) | Targets de motor-de-saltos/truco-RTS por configuración (quitar 0x0884) | Implementado | 001-studio-runtime-fixes |
| [ADR-003](../specs/001-studio-runtime-fixes/adr-003-subproceso-sin-shell.md) | Ejecución de subprocesos sin shell (argv explícito) | Implementado | 001-studio-runtime-fixes |
| [ADR-004](adr-004-gba-save-state-v2.md) | GBA Save State v2 con hardware completo y retrocompatibilidad | Planificado | 002-studio-gba-stability |
| [ADR-005](adr-005-proportional-ui-layout.md) | Diseño proporcional y dinámico de controles en Studio | Planificado | 002-studio-gba-stability |
| [ADR-006](../specs/003-ci-e2e-verificacion/adr-006-topologia-ci-dos-niveles.md) | Topología de CI en dos niveles (hosted + local/self-hosted ROM-gated) | Planificado | 003-ci-e2e-verificacion |
| [ADR-007](../specs/003-ci-e2e-verificacion/adr-007-resolucion-roms-y-skip.md) | Resolución de ROMs por `DRAGON_ROM_DIR`/SHA-1 y "skip ≠ pass" | Planificado | 003-ci-e2e-verificacion |
| [ADR-008](../specs/003-ci-e2e-verificacion/adr-008-contrato-e2e-tres-niveles.md) | Contrato E2E de tres niveles con golden-hash y verify como equivalencia | Planificado | 003-ci-e2e-verificacion |

> Estado: `Implementado` tras la integración de la spec 001 y verificación mediante suite CTest (19/19 en verde).
