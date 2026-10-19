import json
import re
import subprocess
import time
import uuid

from pathlib import Path

import requests

from flask import (
    Flask,
    jsonify,
    request,
    send_from_directory
)

from actions.validator import validar_accion


# ==========================================================
# CONFIGURACION
# ==========================================================

MODEL = "robot-assistant-fast"

OLLAMA_URL = (
    "http://127.0.0.1:11434/api/chat"
)

PIPER_BIN = Path(
    "/home/user/robot_assistant_jetson/"
    "tools/piper/piper/piper"
)

PIPER_MODEL = Path(
    "/home/user/robot_assistant_jetson/"
    "models/piper/es_MX-claude-high.onnx"
)

AUDIO_DIR = Path(
    "/tmp/robot_audio"
)

AUDIO_DIR.mkdir(
    parents=True,
    exist_ok=True
)

app = Flask(__name__)


# ==========================================================
# NORMALIZAR TEXTO
# ==========================================================

def normalizar(texto):

    return (
        texto.lower()
        .strip()
        .replace("á", "a")
        .replace("é", "e")
        .replace("í", "i")
        .replace("ó", "o")
        .replace("ú", "u")
        .replace("ü", "u")
    )


# ==========================================================
# ROUTER RAPIDO
# ==========================================================

def detectar_comando_rapido(texto):

    t = normalizar(texto)

    # ------------------------------------------------------
    # STOP
    # ------------------------------------------------------

    palabras_stop = (
        "detente",
        "deten",
        "alto",
        "parate",
        "stop"
    )

    if (
        t == "para"
        or any(
            palabra in t
            for palabra in palabras_stop
        )
    ):
        return {
            "type": "action",
            "device": "base",
            "action": "stop"
        }

    # ------------------------------------------------------
    # CABEZA CENTRO
    # ------------------------------------------------------

    if (
        "centra la cabeza" in t
        or "cabeza al centro" in t
    ):
        return {
            "type": "action",
            "device": "head_pan",
            "action": "home",
            "value": 0
        }

    # ------------------------------------------------------
    # CABEZA IZQUIERDA
    # ------------------------------------------------------

    if (
        "mira a la izquierda" in t
        or "cabeza a la izquierda" in t
    ):
        return {
            "type": "action",
            "device": "head_pan",
            "action": "move",
            "value": -30
        }

    # ------------------------------------------------------
    # CABEZA DERECHA
    # ------------------------------------------------------

    if (
        "mira a la derecha" in t
        or "cabeza a la derecha" in t
    ):
        return {
            "type": "action",
            "device": "head_pan",
            "action": "move",
            "value": 30
        }

    # ------------------------------------------------------
    # CABEZA ARRIBA
    # ------------------------------------------------------

    if (
        "mira arriba" in t
        or "cabeza arriba" in t
    ):
        return {
            "type": "action",
            "device": "head_tilt",
            "action": "move",
            "value": 20
        }

    # ------------------------------------------------------
    # CABEZA ABAJO
    # ------------------------------------------------------

    if (
        "mira abajo" in t
        or "cabeza abajo" in t
    ):
        return {
            "type": "action",
            "device": "head_tilt",
            "action": "move",
            "value": -20
        }

    # ------------------------------------------------------
    # ANGULO EXPLICITO CABEZA
    # ------------------------------------------------------

    if "cabeza" in t:

        encontrado = re.search(
            r"(-?\d+)\s*(?:grado|grados)?",
            t
        )

        if encontrado:

            angulo = int(
                encontrado.group(1)
            )

            return {
                "type": "action",
                "device": "head_pan",
                "action": "move",
                "value": angulo
            }

    # ------------------------------------------------------
    # GIRO IZQUIERDA BASE
    # ------------------------------------------------------

    if (
        "gira a la izquierda" in t
        or "gira izquierda" in t
        or "dobla a la izquierda" in t
    ):
        return {
            "type": "action",
            "device": "base",
            "action": "turn_left",
            "speed": 0.35,
            "duration_ms": 700
        }

    # ------------------------------------------------------
    # GIRO DERECHA BASE
    # ------------------------------------------------------

    if (
        "gira a la derecha" in t
        or "gira derecha" in t
        or "dobla a la derecha" in t
    ):
        return {
            "type": "action",
            "device": "base",
            "action": "turn_right",
            "speed": 0.35,
            "duration_ms": 700
        }

    # ------------------------------------------------------
    # AVANZAR
    # ------------------------------------------------------

    if (
        "avanza" in t
        or "adelante" in t
        or "ve hacia adelante" in t
    ):
        return {
            "type": "action",
            "device": "base",
            "action": "forward",
            "speed": 0.40,
            "duration_ms": 1200
        }

    # ------------------------------------------------------
    # RETROCEDER
    # ------------------------------------------------------

    if (
        "retrocede" in t
        or "hacia atras" in t
        or "ve hacia atras" in t
    ):
        return {
            "type": "action",
            "device": "base",
            "action": "backward",
            "speed": 0.40,
            "duration_ms": 1200
        }

    return None


# ==========================================================
# FRASE DE ACCION
# ==========================================================

def frase_accion(action):

    device = action.get("device")
    accion = action.get("action")

    if device == "base":

        frases = {
            "forward": "Avanzando.",
            "backward": "Retrocediendo.",
            "turn_left":
                "Girando a la izquierda.",
            "turn_right":
                "Girando a la derecha.",
            "stop": "Detenido."
        }

        return frases.get(
            accion,
            "Entendido."
        )

    if device == "head_pan":

        if accion == "home":
            return "Centrando la cabeza."

        if accion == "stop":
            return "Cabeza detenida."

        return "Moviendo la cabeza."

    if device == "head_tilt":
        return "Moviendo la cabeza."

    return "Entendido."


# ==========================================================
# PIPER
# ==========================================================

def generar_audio(texto):

    nombre = (
        uuid.uuid4().hex +
        ".wav"
    )

    ruta = AUDIO_DIR / nombre

    inicio = time.time()

    proceso = subprocess.run(
        [
            str(PIPER_BIN),
            "--model",
            str(PIPER_MODEL),
            "--output_file",
            str(ruta)
        ],
        input=texto.encode("utf-8"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False
    )

    if proceso.returncode != 0:

        error = proceso.stderr.decode(
            "utf-8",
            errors="ignore"
        )

        raise RuntimeError(
            f"Piper fallo: {error}"
        )

    print(
        "Piper terminado en:",
        round(
            time.time() - inicio,
            3
        ),
        "s"
    )

    return ruta


# ==========================================================
# QWEN
# ==========================================================

SYSTEM_PROMPT = """
Eres el cerebro conversacional de un robot movil.

Responde siempre en español.

Las respuestas habladas deben ser breves y naturales.
Normalmente una o dos oraciones.

Si el usuario hace una pregunta normal, responde con texto.

Si solicita una accion física que no fue detectada por el
router rapido, puedes devolver UNICAMENTE un JSON.

Ejemplo:
{"type":"action","device":"head_pan","action":"move","value":30}

Dispositivos permitidos:
head_pan
head_tilt
base

No inventes dispositivos.
"""


def preguntar_ollama(texto):

    inicio = time.time()

    payload = {
        "model": MODEL,

        "keep_alive": -1,

        "stream": False,

        "messages": [
            {
                "role": "system",
                "content": SYSTEM_PROMPT
            },
            {
                "role": "user",
                "content": texto
            }
        ],

        "options": {
            "num_ctx": 2048,
            "num_predict": 40,
            "temperature": 0.2
        }
    }

    respuesta = requests.post(
        OLLAMA_URL,
        json=payload,
        timeout=120
    )

    respuesta.raise_for_status()

    datos = respuesta.json()

    contenido = (
        datos
        .get("message", {})
        .get("content", "")
        .strip()
    )

    print(
        "Ollama terminado en:",
        round(
            time.time() - inicio,
            3
        ),
        "s"
    )

    return contenido


# ==========================================================
# EXTRAER JSON
# ==========================================================

def extraer_json(texto):

    if not texto:
        return None

    texto = texto.strip()

    try:
        obj = json.loads(texto)

        if isinstance(obj, dict):
            return obj

    except Exception:
        pass

    encontrado = re.search(
        r"\{.*\}",
        texto,
        re.DOTALL
    )

    if not encontrado:
        return None

    try:
        obj = json.loads(
            encontrado.group(0)
        )

        if isinstance(obj, dict):
            return obj

    except Exception:
        return None

    return None



# ==========================================================
# CONSULTAS LOCALES DEL SISTEMA
# NO USAN OLLAMA
# ==========================================================

def respuesta_sistema_local(texto):

    t = normalizar(texto)

    from environment_state import environment
    from system_mode import mode_manager

    estado = environment.snapshot()

    temperatura = estado.get(
        "temperature"
    )

    humedad = estado.get(
        "humidity"
    )

    # ------------------------------------------------------
    # TEMPERATURA
    # ------------------------------------------------------

    if (
        "temperatura" in t
        or "que temperatura" in t
        or "cuantos grados" in t
    ):

        if temperatura is None:
            return (
                "Todavia no tengo datos de temperatura."
            )

        return (
            "La temperatura actual es de "
            + str(round(temperatura, 1))
            + " grados Celsius."
        )

    # ------------------------------------------------------
    # HUMEDAD
    # ------------------------------------------------------

    if (
        "humedad" in t
        or "que humedad" in t
    ):

        if humedad is None:
            return (
                "Todavia no tengo datos de humedad."
            )

        return (
            "La humedad actual es de "
            + str(round(humedad, 1))
            + " por ciento."
        )

    # ------------------------------------------------------
    # ESTADO GENERAL
    # ------------------------------------------------------

    if (
        "estado del sistema" in t
        or "como esta el sistema" in t
        or "modo del sistema" in t
        or "en que modo estas" in t
    ):

        modo = mode_manager.current()

        if modo == "emergency":
            texto_modo = (
                "El sistema esta en modo emergencia."
            )
        else:
            texto_modo = (
                "El sistema esta en modo normal."
            )

        if (
            temperatura is None
            and humedad is None
        ):
            return (
                texto_modo
                + " Todavia no tengo datos ambientales."
            )

        partes = [
            texto_modo
        ]

        if temperatura is not None:
            partes.append(
                "Temperatura "
                + str(round(temperatura, 1))
                + " grados Celsius."
            )

        if humedad is not None:
            partes.append(
                "Humedad "
                + str(round(humedad, 1))
                + " por ciento."
            )

        return " ".join(partes)

    return None


# ==========================================================
# PROCESAR TEXTO
# ==========================================================

def procesar_texto(texto_usuario):

    inicio_total = time.time()

    print()
    print("=" * 60)

    print(
        "TEXTO:",
        texto_usuario
    )

    # ------------------------------------------------------
    # CONSULTA LOCAL DEL SISTEMA
    # ------------------------------------------------------

    respuesta_local = respuesta_sistema_local(
        texto_usuario
    )

    if respuesta_local is not None:

        print(
            "RUTA: LOCAL_SYSTEM"
        )

        print(
            "RESPUESTA:",
            respuesta_local
        )

        ruta_audio = generar_audio(
            respuesta_local
        )

        resultado = {
            "ok": True,
            "route": "local_system",
            "speech": respuesta_local,
            "action": None,
            "action_valid": True,
            "action_error": None,
            "audio_path": str(
                ruta_audio
            ),
        }

        print(
            "TOTAL LOCAL:",
            round(
                time.time()
                -
                inicio_total,
                3
            ),
            "s"
        )

        return resultado

    # ------------------------------------------------------
    # RUTA RAPIDA
    # ------------------------------------------------------

    action = detectar_comando_rapido(
        texto_usuario
    )

    if action is not None:

        print(
            "RUTA: FAST"
        )

        print(
            "ACCION:",
            action
        )

        valido, error = validar_accion(
            action
        )

        if valido:
            speech = frase_accion(
                action
            )

        else:
            speech = (
                "No puedo realizar ese movimiento."
            )

            action = None

        ruta_audio = generar_audio(
            speech
        )

        resultado = {
            "ok": True,
            "route": "fast",
            "speech": speech,
            "action": action,
            "action_valid": valido,
            "action_error":
                None if valido else error,
            "audio_path":
                str(ruta_audio)
        }

        print(
            "TOTAL:",
            round(
                time.time() -
                inicio_total,
                3
            ),
            "s"
        )

        return resultado

    # ------------------------------------------------------
    # RUTA QWEN
    # ------------------------------------------------------

    print(
        "RUTA: LLM"
    )

    respuesta = preguntar_ollama(
        texto_usuario
    )

    action = extraer_json(
        respuesta
    )

    action_valid = False
    action_error = None

    if action:

        valido, error = validar_accion(
            action
        )

        if valido:
            action_valid = True

            speech = frase_accion(
                action
            )

        else:
            action_error = error
            action = None

            speech = (
                "No puedo realizar ese movimiento."
            )

    else:
        speech = respuesta

    ruta_audio = generar_audio(
        speech
    )

    resultado = {
        "ok": True,
        "route": "llm",
        "speech": speech,
        "response": respuesta,
        "action": action,
        "action_valid": action_valid,
        "action_error": action_error,
        "audio_path":
            str(ruta_audio)
    }

    print(
        "TOTAL:",
        round(
            time.time() -
            inicio_total,
            3
        ),
        "s"
    )

    return resultado


# ==========================================================
# FLASK
# ==========================================================

@app.get("/health")
def health():

    return jsonify(
        {
            "status": "ok",
            "model": MODEL,
            "tts": "piper"
        }
    )


@app.post("/chat")
def chat():

    datos = (
        request.get_json(
            silent=True
        )
        or {}
    )

    texto = (
        datos.get("text", "")
        .strip()
    )

    if not texto:

        return jsonify(
            {
                "ok": False,
                "error": "Texto vacio"
            }
        ), 400

    resultado = procesar_texto(
        texto
    )

    ruta = Path(
        resultado["audio_path"]
    )

    resultado["audio_url"] = (
        "/audio/" +
        ruta.name
    )

    return jsonify(
        resultado
    )


@app.get("/audio/<path:nombre>")
def audio(nombre):

    return send_from_directory(
        str(AUDIO_DIR),
        nombre,
        mimetype="audio/wav"
    )


if __name__ == "__main__":

    app.run(
        host="0.0.0.0",
        port=8000,
        debug=False
    )
