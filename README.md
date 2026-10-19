# Mini INDI — Concurso Nacional IoT ULEAM 2026

Repositorio de transparencia y reproducibilidad para **Mini INDI: Sistema AIoT Distribuido para Interacción Ciudadana y Resiliencia en Espacios Urbanos Inteligentes**.

## Qué contiene

- `paper/`: artículo en LaTeX basado en la plantilla oficial IEEEtran del concurso, bibliografía y figuras.
- `firmware/cores3/`: firmware del nodo principal M5Stack CoreS3 y esquema de mensajes.
- `firmware/atom/`: firmware del nodo AtomS3 Lite con BLE + ESP-NOW + UART.
- `firmware/openrb/`: control del actuador móvil mediante OpenRB-150.
- `edge/jetson/`: servidor WebSocket, ASR local, router de comandos, integración Ollama/Piper, modos y seguridad.
- `results/processed/`: tablas de resultados de la campaña.
- `results/raw/`: logs utilizados para trazabilidad experimental.
- `scripts/recompute_metrics.py`: recálculo reproducible de las métricas usadas en el artículo.
- `docs/`: notas de seguridad, reproducibilidad y matriz contra la rúbrica.

## Reproducir métricas

```bash
python -m pip install pandas
python scripts/recompute_metrics.py
```

La media de latencia extremo-a-extremo se calcula **solo con interacciones exitosas que poseen tiempos completos explícitos**. Los dos ensayos con timeout TTS no se completan con tiempos aproximados.

## Configuración de la CoreS3

El repositorio **no publica credenciales reales**. Copiar:

```bash
cp firmware/cores3/include/secrets.example.h firmware/cores3/include/secrets.h
```

y editar la red local antes de compilar.

## Nota científica importante

La versión de firmware incluida implementa el wake word local mediante **MFCC + SVM**. El software de Jetson dispone de backends ASR locales Vosk y Whisper/whisper.cpp; la campaña experimental documentada en este repositorio se ejecutó con **Vosk**, por lo que las latencias publicadas corresponden a ese backend.

## Privacidad

El conjunto crudo de voces usado durante el desarrollo del wake word no se incluye porque contiene archivos potencialmente identificables. El repositorio publica únicamente código, logs de evaluación y resultados necesarios para reproducir las métricas reportadas.

