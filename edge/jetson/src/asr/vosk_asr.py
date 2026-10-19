import json
import wave
from pathlib import Path

from vosk import Model, KaldiRecognizer, SetLogLevel


# ==========================================================
# CONFIGURACION
# ==========================================================

MODEL_PATH = Path(
    "/home/user/robot_assistant_jetson/models/"
    "vosk-model-small-es-0.42"
)

# Oculta la mayoría de logs internos de Vosk
SetLogLevel(-1)

_model = None


# ==========================================================
# CARGAR MODELO UNA SOLA VEZ
# ==========================================================

def cargar_modelo():
    global _model

    if _model is None:

        print()
        print("=" * 60)
        print("CARGANDO VOSK")
        print(MODEL_PATH)
        print("=" * 60)

        if not MODEL_PATH.exists():
            raise FileNotFoundError(
                f"No existe el modelo Vosk: {MODEL_PATH}"
            )

        _model = Model(
            str(MODEL_PATH)
        )

        print("Vosk cargado")
        print("=" * 60)

    return _model


# ==========================================================
# TRANSCRIBIR WAV
# ==========================================================

def transcribir_vosk(ruta_wav):

    model = cargar_modelo()

    with wave.open(
        str(ruta_wav),
        "rb"
    ) as wf:

        # CoreS3 manda mono PCM 16-bit 16 kHz

        if wf.getnchannels() != 1:
            raise ValueError(
                "Vosk requiere audio mono"
            )

        if wf.getsampwidth() != 2:
            raise ValueError(
                "Vosk requiere PCM de 16 bits"
            )

        sample_rate = wf.getframerate()

        recognizer = KaldiRecognizer(
            model,
            sample_rate
        )

        recognizer.SetWords(False)

        while True:

            data = wf.readframes(
                4000
            )

            if not data:
                break

            recognizer.AcceptWaveform(
                data
            )

        resultado = json.loads(
            recognizer.FinalResult()
        )

        texto = (
            resultado
            .get("text", "")
            .strip()
        )

        return texto


# ==========================================================
# PRUEBA MANUAL
# ==========================================================

if __name__ == "__main__":

    import sys
    import time

    if len(sys.argv) < 2:

        print(
            "Uso:"
            "\npython src/asr/vosk_asr.py archivo.wav"
        )

        raise SystemExit(1)

    ruta = sys.argv[1]

    inicio = time.time()

    texto = transcribir_vosk(
        ruta
    )

    tiempo = (
        time.time() -
        inicio
    )

    print()
    print("Texto:", texto)
    print(
        "Tiempo:",
        round(tiempo, 3),
        "s"
    )
