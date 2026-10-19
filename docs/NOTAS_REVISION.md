# Notas de consistencia entre el borrador inicial y el software entregado

Al preparar el artículo se verificaron los archivos fuente y los logs para evitar atribuir resultados a módulos distintos de los realmente evaluados.

1. **Wake word:** el firmware actual de CoreS3 implementa MFCC + SVM. Los CSV ubicados en carpetas denominadas `yolo` corresponden a otros experimentos y no demuestran que el wake word ejecutado en la campaña use YOLO. El manuscrito, por tanto, documenta MFCC + SVM.
2. **ASR:** el software de Jetson contiene soporte para Whisper/whisper.cpp, pero `ws_server.py` de la campaña importa y utiliza Vosk. Los tiempos de ASR se reportan como Vosk.
3. **Latencia conversacional:** los logs entregados no sustentan una media extremo-a-extremo de 3 s. Se recalculó el total exclusivamente con las 9 interacciones exitosas que tienen tiempos completos explícitos: media 39,090 s, mediana 29,307 s. Los comandos IoT de ruta rápida promediaron 6,684 s.
4. **Fallos:** dos consultas terminaron por timeout de ACK durante la transferencia TTS. Sus tiempos aproximados no se usaron para rellenar datos faltantes.

Si el prototipo se optimiza antes de la entrega final, conviene repetir la campaña y regenerar el artículo ejecutando `scripts/recompute_metrics.py` con el nuevo CSV.
