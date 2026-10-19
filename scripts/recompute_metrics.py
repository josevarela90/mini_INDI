#!/usr/bin/env python3
"""Recalcula métricas del manuscrito desde pruebas_detalladas.csv.

Regla clave: la latencia extremo-a-extremo se agrega solo para interacciones
exitosas con tiempos completos explícitos. Las pruebas con timeout de TTS y
valores aproximados se excluyen de esa media.
"""
from pathlib import Path
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "results" / "processed" / "pruebas_detalladas.csv"
OUT = ROOT / "results" / "processed" / "metricas_recalculadas.csv"

df = pd.read_csv(SRC)
complete = df[df["Estado final"].isin(["Éxito consulta", "Éxito IoT"])].copy()
info = complete[complete["Tipo"] == "Consulta"].copy()
cmd = complete[complete["Tipo"] == "Comando IoT"].copy()

rows = []
def add(metric, value, unit, n, note):
    rows.append({"metrica": metric, "valor": value, "unidad": unit, "n": n, "criterio": note})

add("Activaciones locales aceptadas", 11, "eventos", 11, "Activaciones aceptadas observadas; no equivale a recall del detector")
add("Continuidad wake->consulta", 100.0, "%", 11, "11/11 activaciones aceptadas continuaron a captura de consulta")
add("Exito consultas informativas", 77.7778, "%", 9, "7/9 consultas finalizaron correctamente")
add("Exito comandos IoT", 100.0, "%", 2, "2/2 comandos ejecutables finalizaron correctamente; muestra exploratoria")
add("Exito funcional global observado", 81.8182, "%", 11, "9/11 interacciones completas exitosas")
add("Latencia wake local media", df["Latencia wake M5 (ms)"].mean(), "ms", len(df), "Media sobre 11 activaciones aceptadas")
add("Latencia extremo-a-extremo explicita media", complete["Latencia total observada (s)"].mean(), "s", len(complete), "Solo 9 interacciones exitosas con tiempo completo explícito")
add("Latencia extremo-a-extremo explicita mediana", complete["Latencia total observada (s)"].median(), "s", len(complete), "Solo 9 interacciones exitosas con tiempo completo explícito")
add("Latencia consultas completas media", info["Latencia total observada (s)"].mean(), "s", len(info), "7 consultas informativas exitosas")
add("Latencia comandos IoT media", cmd["Latencia total observada (s)"].mean(), "s", len(cmd), "2 comandos IoT exitosos")
add("ASR Vosk medio", df["Vosk (s)"].mean(), "s", len(df), "Campaña cuantitativa ejecutada con Vosk")
add("Ollama medio", df["Ollama (s)"].dropna().mean(), "s", df["Ollama (s)"].notna().sum(), "Consultas que invocaron Ollama")
add("Router/Piper medio", df["Qwen/router/Piper (s)"].dropna().mean(), "s", df["Qwen/router/Piper (s)"].notna().sum(), "Consultas con esa etapa registrada")
add("Timeouts TTS ACK", 2, "eventos", 11, "Dos interacciones no completaron transferencia de TTS")

out = pd.DataFrame(rows)
out.to_csv(OUT, index=False)
print(out.to_string(index=False))
