# Seguridad y privacidad

## Controles implementados

- IA y voz procesadas dentro de la red local; no se requiere servicio cloud.
- Comandos físicos con palabra clave, destinos/acciones permitidos y límites de valor.
- Ruta determinista para comandos físicos frecuentes antes del LLM.
- Modo de emergencia con interbloqueos y parada del actuador.
- Timeout de control ESP-NOW en AtomS3 y timeout adicional en OpenRB.
- Credenciales separadas del código mediante `secrets.h`, excluido por `.gitignore`.
- Dataset crudo de voz identificable excluido del repositorio público.

## Limitaciones del prototipo

- El WebSocket de laboratorio no usa WSS/TLS.
- El broadcast ESP-NOW usado en el prototipo no está cifrado por pares.
- El prototipo no implementa todavía identidad de hardware ni rotación de claves.

## Requisitos antes de despliegue público

- WSS/TLS y certificados gestionados.
- Identidad por dispositivo, claves únicas y rotación.
- ESP-NOW cifrado cuando la topología lo permita.
- Segmentación de red y mínimo privilegio.
- Registro de auditoría y actualización segura de firmware.
- Política explícita de retención de audio y telemetría.
