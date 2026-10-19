# Firmware CoreS3

1. Copiar `include/secrets.example.h` a `include/secrets.h`.
2. Configurar SSID, clave y dirección local de la Jetson.
3. Compilar con PlatformIO.

El firmware implementa wake word local, captura de audio, interfaz visual, WebSocket/HTTP/mDNS, ESP-NOW, validación de comandos estructurados y modo de emergencia.
