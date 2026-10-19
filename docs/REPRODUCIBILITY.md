# Reproducibilidad experimental

La campaña procesada consta de 11 interacciones aceptadas por el wake word local.

## Criterios

- La tasa global considera las 11 interacciones.
- La latencia extremo-a-extremo se calcula únicamente sobre 9 interacciones exitosas con tiempo completo explícito.
- Dos pruebas con `TIMEOUT esperando TTS ACK` se contabilizan como fallos funcionales, pero sus tiempos remotos aproximados no se usan para completar la media de latencia.
- Las métricas del wake word son latencias de activaciones aceptadas; no existe corpus de verdad-terreno suficiente para estimar recall/F1.

## Valores principales recalculados

- Continuidad wake→consulta: 11/11 = 100%.
- Consultas informativas: 7/9 = 77,8%.
- Comandos IoT: 2/2 = 100% (muestra exploratoria).
- Éxito global: 9/11 = 81,8%.
- Wake local: 209,36 ms de media.
- Latencia extremo-a-extremo explícita: 39,090 s de media, mediana 29,307 s, rango 4,544–77,789 s, n=9.
- Consultas informativas completas: 48,349 s de media, n=7.
- Comandos IoT completos: 6,684 s de media, n=2.

Ejecutar `python scripts/recompute_metrics.py` para regenerar la tabla.
