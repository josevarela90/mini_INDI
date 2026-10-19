# Matriz de cobertura de la Rúbrica A — IoT ULEAM 2026

| Criterio | Puntos | Evidencia preparada en el artículo/repositorio |
|---|---:|---|
| Innovación y relevancia | 3 | Arquitectura AIoT local-first para interacción ciudadana; combinación de interfaz conversacional, servicios urbanos y actuación distribuida; contexto de Manta y Ecuador; contribuciones explícitas en Introducción. |
| Problema y metodología | 3 | Problema territorial documentado; fases de desarrollo; criterios de diseño; beneficiarios; escenarios y métricas; regla transparente para recalcular latencia. |
| Arquitectura IoT y diseño | 6 | Diagrama de arquitectura; tabla de protocolos; topología y justificación Wi-Fi/WebSocket, HTTP/mDNS, ESP-NOW, BLE y UART; IA en el borde; esquema de comandos; seguridad, privacidad y limitaciones. |
| Prototipo y resultados | 4 | Prototipo integrado; 11 interacciones; tasas de éxito; latencia wake; latencias por etapa; latencia total recalculada solo con datos explícitos; identificación de timeouts TTS; scripts y logs reproducibles. |
| Impacto, viabilidad y escalabilidad | 2 | Aplicación en terminales, edificios, campus y servicios públicos; funcionamiento sin cloud; modularidad; expansión de nodos/protocolos; limitaciones de escalado y endurecimiento. |
| Rigor IEEE | 2 | Plantilla `IEEEtran` journal one-column; resumen 150–250 palabras; 6 palabras clave; figuras; referencias IEEE recientes; estructura oficial; límite a verificar tras completar autoría. |

## Diferenciadores de las bases

- **Edge computing:** wake word y UI en CoreS3; ASR/LLM/TTS en Jetson local.
- **Tolerancia a fallos:** modo emergencia; timeout ESP-NOW en AtomS3; segundo timeout en OpenRB; operación sin servicios cloud.
- **Ciberseguridad/privacidad:** validación por listas blancas, rangos e interbloqueos; tratamiento local de voz; secretos excluidos del repositorio; limitaciones de cifrado documentadas.
- **Apertura y reproducibilidad:** código, esquemas, logs, CSV y script de recálculo preparados para GitHub.

## Aspectos que no deben sobreafirmarse

- 2/2 comandos IoT no es suficiente para afirmar confiabilidad estadística de ESP-NOW.
- No existe verdad-terreno suficiente para reportar Accuracy/Precision/Recall/F1 del wake word.
- La campaña medida usa Vosk; no atribuir sus tiempos a Whisper.
- El wake word actual usa MFCC + SVM; no atribuirlo a YOLO salvo que se cambie y vuelva a evaluar el firmware.
