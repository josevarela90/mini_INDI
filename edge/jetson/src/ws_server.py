import asyncio
import json
import re
import sys
import tempfile
import time
import unicodedata
from pathlib import Path

import requests
import websockets

from asr.vosk_asr import cargar_modelo, transcribir_vosk
from server import procesar_texto
from environment_state import environment
from safety_manager import safety
from system_mode import mode_manager



HOST = "0.0.0.0"
PORT = 8765

MODEL = "robot-assistant-fast"

COMMAND_KEYWORD = "mini indi"

MAX_QUERY_BYTES = 1024 * 1024


# ==========================================================
# NORMALIZAR
# ==========================================================

def normalizar(texto):
    texto = (texto or "").lower().strip()

    texto = unicodedata.normalize(
        "NFD",
        texto,
    )

    texto = "".join(
        c
        for c in texto
        if unicodedata.category(c) != "Mn"
    )

    texto = re.sub(
        r"\s+",
        " ",
        texto,
    )

    return texto


# ==========================================================
# COMANDOS FISICOS
# ==========================================================

def comando_desde_transcripcion(texto):
    t = normalizar(texto)

    if not t:
        return None

    # =========================================================
    # LUZ - COLORES
    # Se procesa ANTES que cabeza/ruedas.
    # =========================================================

    colores = {
        "rojo": "red",
        "roja": "red",
        "verde": "green",
        "azul": "blue",
        "amarillo": "yellow",
        "amarilla": "yellow",
        "morado": "purple",
        "morada": "purple",
        "violeta": "purple",
        "cian": "cyan",
        "blanco": "white",
        "blanca": "white",
        "naranja": "orange",
    }

    palabras_luz = (
        "luz" in t
        or "led" in t
        or "color" in t
    )

    if palabras_luz:
        for palabra, color in colores.items():
            if palabra in t:
                return {
                    "keyword": COMMAND_KEYWORD,
                    "target": "light",
                    "action": "color",
                    "color": color,
                }

    # También aceptar:
    # "pon rojo"
    # "cambia a azul"
    # pero solamente cuando aparece una palabra de intención de color.

    intencion_color = (
        "pon " in t
        or "cambia " in t
        or "cambiar " in t
        or "color " in t
    )

    if intencion_color:
        for palabra, color in colores.items():
            if palabra in t:
                return {
                    "keyword": COMMAND_KEYWORD,
                    "target": "light",
                    "action": "color",
                    "color": color,
                }

    # =========================================================
    # LUZ ON
    # =========================================================

    frases_luz_on = (
        "prende la luz",
        "enciende la luz",
        "prender la luz",
        "encender la luz",
        "prende el led",
        "enciende el led",
    )

    if any(frase in t for frase in frases_luz_on):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "light",
            "action": "on",
        }

    # =========================================================
    # LUZ OFF
    # =========================================================

    frases_luz_off = (
        "apaga la luz",
        "apagar la luz",
        "apaga el led",
        "apagar el led",
    )

    if any(frase in t for frase in frases_luz_off):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "light",
            "action": "off",
        }

    # =========================================================
    # STOP
    # =========================================================

    frases_stop = (
        "detente",
        "parate",
        "para el robot",
        "frena",
    )

    if any(frase in t for frase in frases_stop):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "drive",
            "action": "stop",
        }

    # =========================================================
    # RUEDAS
    # =========================================================

    if (
        "avanza" in t
        or "ve hacia adelante" in t
        or t == "adelante"
    ):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "drive",
            "action": "forward",
            "value": 60,
        }

    if (
        "retrocede" in t
        or "ve hacia atras" in t
        or t == "atras"
    ):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "drive",
            "action": "backward",
            "value": 55,
        }

    if (
        "gira a la izquierda" in t
        or "gira izquierda" in t
    ):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "drive",
            "action": "left",
            "value": 55,
        }

    if (
        "gira a la derecha" in t
        or "gira derecha" in t
    ):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "drive",
            "action": "right",
            "value": 55,
        }

    # =========================================================
    # CABEZA
    # SOLO frases claras.
    # No aceptar "izquierda" o "derecha" solas.
    # =========================================================

    frases_cabeza_izq = (
        "mira a la izquierda",
        "mira hacia la izquierda",
        "mueve la cabeza a la izquierda",
        "cabeza a la izquierda",
    )

    if any(frase in t for frase in frases_cabeza_izq):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "head",
            "action": "left",
        }

    frases_cabeza_der = (
        "mira a la derecha",
        "mira hacia la derecha",
        "mueve la cabeza a la derecha",
        "cabeza a la derecha",
    )

    if any(frase in t for frase in frases_cabeza_der):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "head",
            "action": "right",
        }

    frases_cabeza_centro = (
        "centra la cabeza",
        "mira al centro",
        "cabeza al centro",
    )

    if any(frase in t for frase in frases_cabeza_centro):
        return {
            "keyword": COMMAND_KEYWORD,
            "target": "head",
            "action": "center",
        }

    return None


def validar_comando(comando):
    if not isinstance(comando, dict):
        return False

    if comando.get("keyword") != COMMAND_KEYWORD:
        return False

    target = comando.get("target")
    action = comando.get("action")

    if target == "light":
        if action in {"on", "off"}:
            return True

        if action == "color":
            return comando.get("color") in {
                "red",
                "green",
                "blue",
                "yellow",
                "purple",
                "cyan",
                "white",
                "orange",
            }

        return False

    if target == "drive":
        if action not in {
            "forward",
            "backward",
            "left",
            "right",
            "stop",
        }:
            return False

        if action == "stop":
            return True

        try:
            value = int(comando.get("value", 0))
        except Exception:
            return False

        return 0 <= value <= 100

    if target == "head":
        return action in {
            "left",
            "right",
            "center",
        }

    return False


def probar_comandos():
    pruebas = [
        "prende la luz",
        "apaga la luz",
        "avanza",
        "retrocede",
        "gira a la izquierda",
        "gira a la derecha",
        "mira a la izquierda",
        "mira a la derecha",
        "centra la cabeza",
        "detente",
        "que hora es",
    ]

    print()
    print("=" * 60)
    print("PRUEBA JSON MINI INDI")
    print("=" * 60)

    for frase in pruebas:
        comando = comando_desde_transcripcion(
            frase
        )

        print()
        print(
            "Texto:",
            frase,
        )

        print(
            "JSON:",
            json.dumps(
                comando,
                ensure_ascii=False,
            ),
        )

        if comando:
            print(
                "Valido:",
                validar_comando(
                    comando
                ),
            )

    print()
    print("=" * 60)


# ==========================================================
# QWEN
# ==========================================================

def precargar_ollama():
    print()
    print("=" * 60)
    print("PRECARGANDO QWEN")
    print("=" * 60)

    inicio = time.time()

    respuesta = requests.post(
        "http://127.0.0.1:11434/api/generate",
        json={
            "model": MODEL,
            "prompt": "Responde solamente: listo",
            "stream": False,
            "keep_alive": -1,
            "options": {
                "num_ctx": 2048,
                "num_predict": 5,
                "temperature": 0,
            },
        },
        timeout=120,
    )

    respuesta.raise_for_status()

    print(
        "Qwen listo en:",
        round(
            time.time() - inicio,
            3,
        ),
        "s",
    )


# ==========================================================
# PROCESAR WAV COMPLETO
# ==========================================================

async def procesar_audio(
    websocket,
    audio_bytes,
):
    inicio_total = time.time()

    print()
    print("=" * 60)
    print("AUDIO COMPLETO RECIBIDO")
    print(
        "Bytes:",
        len(audio_bytes),
    )
    print("=" * 60)

    with tempfile.NamedTemporaryFile(
        suffix=".wav",
        delete=False,
    ) as archivo:

        archivo.write(
            audio_bytes
        )

        wav_entrada = Path(
            archivo.name
        )

    try:
        # ==================================================
        # VOSK
        # ==================================================

        loop = asyncio.get_running_loop()

        inicio_asr = time.time()

        texto = await loop.run_in_executor(
            None,
            transcribir_vosk,
            wav_entrada,
        )

        tiempo_asr = (
            time.time()
            -
            inicio_asr
        )

        print(
            "Vosk:",
            round(
                tiempo_asr,
                3,
            ),
            "s",
        )

        print(
            "TRANSCRIPCION:",
            texto,
        )

        if not texto:
            await websocket.send(
                json.dumps(
                    {
                        "type": "error",
                        "error": "No entendi el audio",
                    },
                    ensure_ascii=False,
                )
            )

            return

        # ==================================================
        # DETECTAR COMANDO - MODO RAPIDO
        # ==================================================


        # ==================================================
        # MODO EMERGENCIA POR VOZ
        # ==================================================

        texto_norm = normalizar(
            texto
        )

        activar_emergencia = (
            "activa modo emergencia" in texto_norm
            or "activa modo de emergencia" in texto_norm
            or "activar modo emergencia" in texto_norm
            or "activar modo de emergencia" in texto_norm
            or "activa la emergencia" in texto_norm
            or "activar la emergencia" in texto_norm
            or "activar emergencia" in texto_norm
            or "activamos de emergencia" in texto_norm
        )

        if activar_emergencia:

            mode_manager.emergency_on()
            safety.activate_emergency()
            environment.set_emergency(
                True
            )

            print()
            print(
                "========================================"
            )
            print(
                "EMERGENCIA ACTIVADA POR VOZ"
            )
            print(
                "========================================"
            )

            paquete_emergencia = {
                "type": "command",
                "transcript": texto,
                "command": {
                    "keyword": COMMAND_KEYWORD,
                    "target": "system",
                    "action": "emergency_on",
                },
            }

            await websocket.send(
                json.dumps(
                    paquete_emergencia,
                    ensure_ascii=False,
                )
            )

            return

        comando = comando_desde_transcripcion(
            texto
        )

        comando_permitido = True
        motivo_bloqueo = None

        if comando is not None:

            target_seguridad = comando.get(
                "target",
                ""
            )

            action_seguridad = comando.get(
                "action",
                ""
            )

            clave_seguridad = (
                target_seguridad
                + "_"
                + action_seguridad
            )

            comando_permitido, motivo_bloqueo = (
                safety.command_allowed(
                    clave_seguridad
                )
            )

            if not comando_permitido:

                print()
                print(
                    "========================================"
                )
                print(
                    "COMANDO BLOQUEADO POR SEGURIDAD"
                )
                print(
                    "COMANDO:",
                    clave_seguridad
                )
                print(
                    "MOTIVO:",
                    motivo_bloqueo
                )
                print(
                    "========================================"
                )

                paquete_bloqueado = {
                    "type": "safety_block",
                    "transcript": texto,
                    "mode": (
                        mode_manager.current()
                    ),
                    "command": comando,
                    "reason": motivo_bloqueo,
                }

                await websocket.send(
                    json.dumps(
                        paquete_bloqueado,
                        ensure_ascii=False,
                    )
                )

                return

        if (
            comando is not None
            and
            comando_permitido
            and
            validar_comando(
                comando
            )
        ):
            print()
            print(
                "========================================"
            )
            print(
                "COMANDO RAPIDO - SIN QWEN / SIN PIPER"
            )
            print(
                "========================================"
            )

            print(
                json.dumps(
                    comando,
                    ensure_ascii=False,
                )
            )

            # La CoreS3 recibe el comando dentro de RESULT.
            # audio_bytes=0 significa:
            # - ejecutar accion
            # - no esperar audio TTS
            # - volver a reposo inmediatamente.
            paquete_rapido = {
                "type": "command",
                "transcript": texto,
                "command": comando,
            }

            await websocket.send(
                json.dumps(
                    paquete_rapido,
                    ensure_ascii=False,
                )
            )

            print(
                "COMANDO ENVIADO A CORES3"
            )

            print(
                "Qwen omitido"
            )

            print(
                "Piper omitido"
            )

            print(
                "TOTAL COMANDO:",
                round(
                    time.time()
                    -
                    inicio_total,
                    3,
                ),
                "s",
            )

            return

        comando = None

        print(
            "No es comando fisico"
        )

        # ==================================================
        # QWEN / ROUTER / PIPER
        # ==================================================

        print()
        print(
            "Procesando respuesta..."
        )

        inicio_procesamiento = time.time()

        resultado = await loop.run_in_executor(
            None,
            procesar_texto,
            texto,
        )

        tiempo_procesamiento = (
            time.time()
            -
            inicio_procesamiento
        )

        print(
            "Qwen/router/Piper:",
            round(
                tiempo_procesamiento,
                3,
            ),
            "s",
        )

        speech = resultado.get(
            "speech",
            "",
        )

        # ==================================================
        # LIMITAR RESPUESTA HABLADA
        # ==================================================
        # Evita WAV gigantes que pueden saturar al CoreS3.
        # Aproximadamente 220 caracteres suelen dar una
        # respuesta natural de 1-3 frases.
        MAX_SPEECH_CHARS = 220

        if len(speech) > MAX_SPEECH_CHARS:
            corte = speech[:MAX_SPEECH_CHARS]

            # Intentar cortar al final de una frase.
            ultimo_punto = max(
                corte.rfind("."),
                corte.rfind("?"),
                corte.rfind("!")
            )

            if ultimo_punto >= 80:
                speech = corte[:ultimo_punto + 1]
            else:
                ultimo_espacio = corte.rfind(" ")

                if ultimo_espacio > 0:
                    speech = corte[:ultimo_espacio] + "..."
                else:
                    speech = corte + "..."

            print(
                "RESPUESTA DE VOZ RECORTADA:",
                len(speech),
                "caracteres"
            )

        print(
            "Respuesta:",
            speech,
        )

        ruta_audio = Path(
            resultado[
                "audio_path"
            ]
        )

        audio_tts = ruta_audio.read_bytes()

        print(
            "Audio Piper:",
            len(audio_tts),
            "bytes",
        )

        # ==================================================
        # RESULTADO
        # ==================================================

        paquete = {
            "type": "result",
            "route": resultado.get(
                "route"
            ),
            "speech": speech,
            "action": resultado.get(
                "action"
            ),
            "action_valid": resultado.get(
                "action_valid",
                False,
            ),
            "action_error": resultado.get(
                "action_error"
            ),
            "transcript": texto,
            "audio_bytes": len(
                audio_tts
            ),
        }

        await websocket.send(
            json.dumps(
                paquete,
                ensure_ascii=False,
            )
        )

        # ==================================================
        # TTS EN BLOQUES
        # ==================================================

        # ==================================================
        # TTS EN BLOQUES ESTABLES PARA ESP32
        # ==================================================
        # ==================================================
        # TTS CON CONTROL DE FLUJO POR ACK
        # ==================================================
        # Jetson envia un bloque y espera confirmacion
        # real del CoreS3 antes de continuar.
        # ==================================================

        bloque = 2048
        enviados = 0

        for i in range(
            0,
            len(audio_tts),
            bloque,
        ):
            chunk = audio_tts[
                i:
                i + bloque
            ]

            await websocket.send(
                chunk
            )

            enviados += len(chunk)

            print(
                f"TTS TX: {enviados} / {len(audio_tts)}"
            )

            # ----------------------------------------------
            # ESPERAR ACK DEL CORES3
            # ----------------------------------------------

            try:
                ack_raw = await asyncio.wait_for(
                    websocket.recv(),
                    timeout=5.0,
                )

            except asyncio.TimeoutError:
                raise RuntimeError(
                    f"TIMEOUT esperando TTS ACK "
                    f"en {enviados}/{len(audio_tts)}"
                )

            if not isinstance(
                ack_raw,
                str,
            ):
                raise RuntimeError(
                    "TTS ACK invalido: "
                    "se esperaba JSON de texto"
                )

            try:
                ack = json.loads(
                    ack_raw
                )

            except Exception as e:
                raise RuntimeError(
                    f"TTS ACK JSON invalido: {repr(e)}"
                )

            if (
                ack.get("type") !=
                "tts_ack"
            ):
                raise RuntimeError(
                    f"Mensaje inesperado durante TTS: {ack}"
                )

            recibido_core = int(
                ack.get(
                    "received",
                    -1,
                )
            )

            if recibido_core != enviados:
                raise RuntimeError(
                    f"TTS ACK no coincide: "
                    f"Jetson={enviados}, "
                    f"CoreS3={recibido_core}"
                )

            print(
                f"TTS ACK: {recibido_core}"
            )


        await websocket.send(
            json.dumps(
                {
                    "type": "audio_end",
                    "bytes": len(
                        audio_tts
                    ),
                }
            )
        )

        print()
        print(
            "TOTAL:",
            round(
                time.time()
                -
                inicio_total,
                3,
            ),
            "s",
        )

    except Exception as e:
        print()
        print(
            "ERROR PROCESANDO AUDIO:",
            repr(e),
        )

        try:
            await websocket.send(
                json.dumps(
                    {
                        "type": "error",
                        "error": str(e),
                    },
                    ensure_ascii=False,
                )
            )

        except Exception:
            pass

    finally:
        try:
            wav_entrada.unlink(
                missing_ok=True
            )

        except Exception:
            pass



# ==========================================================
# VERIFICAR "MINI INDI" CON VOSK
# ==========================================================

async def verificar_wake_audio(
    websocket,
    audio_bytes,
):
    print()
    print("=" * 60)
    print("VERIFICANDO WAKE: MINI INDI")
    print("Bytes:", len(audio_bytes))
    print("=" * 60)

    wav_entrada = None

    try:
        with tempfile.NamedTemporaryFile(
            suffix=".wav",
            delete=False,
        ) as archivo:

            archivo.write(
                audio_bytes
            )

            wav_entrada = Path(
                archivo.name
            )

        loop = asyncio.get_running_loop()

        inicio = time.time()

        texto = await loop.run_in_executor(
            None,
            transcribir_vosk,
            wav_entrada,
        )

        texto_normalizado = normalizar(
            texto
        )

        aceptado = (
            texto_normalizado ==
            "mini indi"
        )

        print(
            "WAKE VOSK:",
            texto,
        )

        print(
            "WAKE NORMALIZADO:",
            texto_normalizado,
        )

        print(
            "WAKE RESULTADO:",
            "ACEPTADO"
            if aceptado
            else
            "RECHAZADO",
        )

        print(
            "WAKE VOSK TIEMPO:",
            round(
                time.time() - inicio,
                3,
            ),
            "s",
        )

        await websocket.send(
            json.dumps(
                {
                    "type": "wake_verify_result",
                    "accepted": aceptado,
                    "transcript": texto_normalizado,
                },
                ensure_ascii=False,
            )
        )

    except Exception as e:
        print(
            "ERROR WAKE VERIFY:",
            repr(e),
        )

        await websocket.send(
            json.dumps(
                {
                    "type": "wake_verify_result",
                    "accepted": False,
                    "transcript": "",
                    "error": str(e),
                },
                ensure_ascii=False,
            )
        )

    finally:
        if wav_entrada is not None:
            try:
                wav_entrada.unlink(
                    missing_ok=True
                )
            except Exception:
                pass


# ==========================================================
# CLIENTE CORES3
# ==========================================================

async def cliente_conectado(
    websocket,
):
    remoto = websocket.remote_address

    print()
    print("=" * 60)
    print(
        "CoreS3 conectado:",
        remoto,
    )
    print("=" * 60)

    # Para recepcion por bloques
    query_buffer = None
    query_expected = 0

    # Para verificacion de wake word
    wake_buffer = None
    wake_expected = 0

    try:
        async for mensaje in websocket:

            # ==================================================
            # MENSAJE JSON
            # ==================================================

            if isinstance(
                mensaje,
                str,
            ):
                try:
                    datos = json.loads(
                        mensaje
                    )

                except Exception as e:
                    print(
                        "JSON invalido:",
                        repr(e),
                    )

                    continue

                tipo = datos.get(
                    "type",
                    "",
                )


                # ----------------------------------------------
                # VERIFICACION DE WAKE WORD
                # ----------------------------------------------


                # ======================================================
                # TELEMETRIA AMBIENTAL
                # ======================================================

                if tipo == "telemetry":

                    node = datos.get(
                        "node",
                        "ATOM_ENV"
                    )

                    temperature = datos.get(
                        "temperature"
                    )

                    humidity = datos.get(
                        "humidity"
                    )

                    emergency = bool(
                        datos.get(
                            "emergency",
                            False
                        )
                    )

                    environment.update(
                        temperature=temperature,
                        humidity=humidity,
                        emergency=emergency,
                        node=node,
                    )

                    print()
                    print(
                        "ENV TELEMETRIA:"
                    )
                    print(
                        environment.snapshot()
                    )

                    if emergency:

                        mode_manager.emergency_on()
                        safety.activate_emergency()

                    continue

                # ======================================================
                # EVENTO DE EMERGENCIA
                # ======================================================

                if tipo == "emergency":

                    active = bool(
                        datos.get(
                            "active",
                            True
                        )
                    )

                    source = datos.get(
                        "source",
                        "unknown"
                    )

                    if active:

                        mode_manager.emergency_on()
                        safety.activate_emergency()
                        environment.set_emergency(
                            True
                        )

                        print()
                        print(
                            "!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                        )
                        print(
                            "MODO EMERGENCIA ACTIVADO"
                        )
                        print(
                            "FUENTE:",
                            source
                        )
                        print(
                            "!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
                        )

                    else:

                        mode_manager.emergency_off()
                        safety.deactivate_emergency()
                        environment.set_emergency(
                            False
                        )

                        print()
                        print(
                            "MODO NORMAL RESTAURADO"
                        )

                    respuesta_estado = {
                        "type": "system_mode",
                        "mode": (
                            mode_manager.current()
                        ),
                        "emergency": (
                            mode_manager.is_emergency()
                        ),
                    }

                    await websocket.send(
                        json.dumps(
                            respuesta_estado
                        )
                    )

                    continue

                # ======================================================
                # CONSULTA DEL ESTADO DEL SISTEMA
                # ======================================================

                if tipo == "system_status":

                    respuesta_estado = {
                        "type": "system_status_result",
                        "mode": (
                            mode_manager.current()
                        ),
                        "environment": (
                            environment.snapshot()
                        ),
                    }

                    await websocket.send(
                        json.dumps(
                            respuesta_estado
                        )
                    )

                    continue

                if tipo == "wake_verify_start":
                    wake_expected = int(
                        datos.get(
                            "bytes",
                            0,
                        )
                    )

                    if (
                        wake_expected <= 44
                        or
                        wake_expected > 100000
                    ):
                        wake_buffer = None
                        wake_expected = 0

                        await websocket.send(
                            json.dumps(
                                {
                                    "type": "wake_verify_result",
                                    "accepted": False,
                                    "transcript": "",
                                    "error": "tamano wake invalido",
                                }
                            )
                        )

                        continue

                    wake_buffer = bytearray()

                    print()
                    print(
                        "WAKE_VERIFY_START:",
                        wake_expected,
                        "bytes",
                    )

                    continue

                if tipo == "wake_verify_end":
                    if wake_buffer is None:
                        continue

                    recibido = len(
                        wake_buffer
                    )

                    print(
                        "WAKE_VERIFY_END:",
                        recibido,
                        "/",
                        wake_expected,
                    )

                    if recibido != wake_expected:
                        wake_buffer = None
                        wake_expected = 0

                        await websocket.send(
                            json.dumps(
                                {
                                    "type": "wake_verify_result",
                                    "accepted": False,
                                    "transcript": "",
                                    "error": "wake incompleto",
                                }
                            )
                        )

                        continue

                    wake_audio = bytes(
                        wake_buffer
                    )

                    wake_buffer = None
                    wake_expected = 0

                    await verificar_wake_audio(
                        websocket,
                        wake_audio,
                    )

                    continue

                # ----------------------------------------------
                # NUEVO PROTOCOLO POR BLOQUES
                # ----------------------------------------------

                if tipo == "query_start":
                    query_expected = int(
                        datos.get(
                            "bytes",
                            0,
                        )
                    )

                    if (
                        query_expected <= 44
                        or
                        query_expected >
                        MAX_QUERY_BYTES
                    ):
                        print(
                            "Tamaño de audio invalido:",
                            query_expected,
                        )

                        query_buffer = None
                        query_expected = 0

                        await websocket.send(
                            json.dumps(
                                {
                                    "type": "error",
                                    "error": "Tamano de audio invalido",
                                }
                            )
                        )

                        continue

                    query_buffer = bytearray()

                    print()
                    print(
                        "QUERY_START:"
                    )

                    print(
                        "Esperando",
                        query_expected,
                        "bytes",
                    )

                    continue

                if tipo == "query_end":
                    if query_buffer is None:
                        print(
                            "QUERY_END sin QUERY_START"
                        )

                        continue

                    recibido = len(
                        query_buffer
                    )

                    print()
                    print(
                        "QUERY_END:"
                    )

                    print(
                        "Recibido:",
                        recibido,
                        "/",
                        query_expected,
                    )

                    if recibido != query_expected:
                        query_buffer = None
                        query_expected = 0

                        await websocket.send(
                            json.dumps(
                                {
                                    "type": "error",
                                    "error": "Audio incompleto",
                                }
                            )
                        )

                        continue

                    audio_completo = bytes(
                        query_buffer
                    )

                    query_buffer = None
                    query_expected = 0

                    await procesar_audio(
                        websocket,
                        audio_completo,
                    )

                    continue

                # ----------------------------------------------
                # COMPATIBILIDAD CON PROTOCOLO ANTERIOR
                # ----------------------------------------------

                if tipo == "query":
                    print()
                    print(
                        "QUERY antiguo recibido"
                    )

                    query_buffer = None
                    query_expected = 0

                    continue

                continue

            # ==================================================
            # BINARIO
            # ==================================================

            if not isinstance(
                mensaje,
                bytes,
            ):
                continue

            # Wake verify tiene prioridad
            if wake_buffer is not None:

                nuevo_total = (
                    len(wake_buffer)
                    +
                    len(mensaje)
                )

                if (
                    nuevo_total >
                    wake_expected
                    or
                    nuevo_total >
                    100000
                ):
                    print(
                        "ERROR: demasiados bytes WAKE"
                    )

                    wake_buffer = None
                    wake_expected = 0

                    await websocket.send(
                        json.dumps(
                            {
                                "type": "wake_verify_result",
                                "accepted": False,
                                "transcript": "",
                                "error": "wake excede tamano",
                            }
                        )
                    )

                    continue

                wake_buffer.extend(
                    mensaje
                )

                print(
                    "WAKE RX:",
                    len(wake_buffer),
                    "/",
                    wake_expected,
                )

                continue

            # Nuevo protocolo:
            # acumular los bloques.
            if query_buffer is not None:

                nuevo_total = (
                    len(query_buffer)
                    +
                    len(mensaje)
                )

                if (
                    nuevo_total >
                    query_expected
                    or
                    nuevo_total >
                    MAX_QUERY_BYTES
                ):
                    print(
                        "ERROR: demasiados bytes"
                    )

                    query_buffer = None
                    query_expected = 0

                    await websocket.send(
                        json.dumps(
                            {
                                "type": "error",
                                "error": "Audio excede tamano esperado",
                            }
                        )
                    )

                    continue

                query_buffer.extend(
                    mensaje
                )

                print(
                    "AUDIO RX:",
                    len(query_buffer),
                    "/",
                    query_expected,
                )

                continue

            # Firmware anterior:
            # un unico frame WAV.
            await procesar_audio(
                websocket,
                mensaje,
            )

    except websockets.ConnectionClosed as e:
        print()
        print(
            "CoreS3 desconectado:",
            remoto,
        )

        print(
            "Codigo:",
            getattr(
                e,
                "code",
                "?",
            ),
        )

    except Exception as e:
        print()
        print(
            "ERROR DE CONEXION:",
            repr(e),
        )


# ==========================================================
# MAIN
# ==========================================================

async def main():
    print()
    print("=" * 60)
    print(
        "JETSON ROBOT - MINI INDI"
    )
    print(
        "WAKE: Mini Indi local en CoreS3"
    )
    print(
        "AUDIO: WebSocket por bloques"
    )
    print(
        "VOSK + JSON + QWEN + PIPER"
    )
    print(
        "Puerto:",
        PORT,
    )
    print("=" * 60)

    print()
    print(
        "Precargando Vosk..."
    )

    inicio = time.time()

    cargar_modelo()

    print(
        "Vosk listo en:",
        round(
            time.time() - inicio,
            3,
        ),
        "s",
    )

    precargar_ollama()

    print()
    print(
        "Esperando CoreS3..."
    )

    async with websockets.serve(
        cliente_conectado,
        HOST,
        PORT,
        max_size=2 * 1024 * 1024,

        # La CoreS3 controla su heartbeat.
        ping_interval=None,
    ):
        await asyncio.Future()


# ==========================================================
# START
# ==========================================================

if __name__ == "__main__":

    if (
        len(sys.argv) > 1
        and
        sys.argv[1] == "--test-commands"
    ):
        probar_comandos()

    else:
        asyncio.run(
            main()
        )
