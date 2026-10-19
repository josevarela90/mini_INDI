import subprocess
import tempfile
import os
import re


WHISPER_BIN = (
    "/home/user/robot_assistant_jetson/"
    "src/asr/whisper.cpp/build/bin/whisper-cli"
)

MODEL = (
    "/home/user/robot_assistant_jetson/"
    "src/asr/whisper.cpp/models/ggml-tiny.bin"
)


def transcribir(ruta_audio):

    if not os.path.exists(ruta_audio):
        raise FileNotFoundError(
            f"No existe el audio: {ruta_audio}"
        )

    with tempfile.NamedTemporaryFile(
        suffix=".wav",
        delete=False
    ) as temporal:

        wav_16k = temporal.name

    try:
        # Normalizar a formato compatible con Whisper
        subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-loglevel", "error",
                "-i", ruta_audio,
                "-ar", "16000",
                "-ac", "1",
                "-c:a", "pcm_s16le",
                wav_16k
            ],
            check=True
        )

        proceso = subprocess.run(
            [
                WHISPER_BIN,
                "-m", MODEL,
                "-f", wav_16k,
                "-l", "es"
            ],
            capture_output=True,
            text=True,
            check=True
        )

        salida = proceso.stdout + "\n" + proceso.stderr

        # Busca:
        # [00:00:00.000 --> 00:00:03.000] Hola robot...
        patron = (
            r"^\[\d\d:\d\d:\d\d\.\d{3}\s+-->\s+"
            r"\d\d:\d\d:\d\d\.\d{3}\]\s+(.*)$"
        )

        fragmentos = re.findall(
            patron,
            salida,
            flags=re.MULTILINE
        )

        texto = " ".join(fragmentos).strip()

        if not texto:
            raise RuntimeError(
                "Whisper no devolvió ninguna transcripción"
            )

        return texto

    finally:
        if os.path.exists(wav_16k):
            os.remove(wav_16k)


if __name__ == "__main__":

    texto = transcribir(
        "/tmp/prueba_claude_16k.wav"
    )

    print("\nTRANSCRIPCIÓN:")
    print(texto)
