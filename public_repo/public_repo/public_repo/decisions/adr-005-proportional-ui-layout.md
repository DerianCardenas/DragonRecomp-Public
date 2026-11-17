# ADR-005: Diseño proporcional y dinámico de controles en DragonRecomp Studio

## Contexto
En `studio/src/app.cpp`, varios controles (el Combo de síntomas de diagnóstico de 380px junto a un botón de 240px, las casillas de pantalla completa y filtro CRT, y los banners de rutinas aprendidas) utilizaban anchos rígidos en píxeles que excedían el ancho disponible de la columna (`col_w ~ 500-580px`). Esto provocaba que los botones se amontonaran unos sobre otros o se salieran del recuadro.

## Decisión
1. Reemplazar anchos fijos por cálculos dinámicos basados en `ImGui::GetContentRegionAvail().x`.
2. En el Asistente de Diagnóstico, asignar el 60% del ancho disponible al Combo selector y el 40% al botón `"🛠 Intentar Reparar"`, restando el espaciado exacto entre elementos.
3. Separar los checkboxes de opciones visuales y el botón de optimización de rutinas aprendidas para garantizar que nunca se solapen en ninguna resolución de pantalla.

## Consecuencias
- Interfaz gráfica limpia, responsiva y sin colisiones de controles.
- Mantenimiento coherente de la experiencia de usuario en cualquier tamaño de ventana.
