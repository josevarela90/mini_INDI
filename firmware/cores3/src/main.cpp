#include <M5Unified.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <arduinoFFT.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "indi_esp32_model.h"
#include "secrets.h"
#include "te_escucho_audio.h"

// ==========================================================
// CONFIGURACION
// ==========================================================

// Credenciales y dirección del servidor definidas en include/secrets.h.
// Copie secrets.example.h como secrets.h y ajuste los valores.
const uint16_t WS_PORT = 8765;
const char* WS_PATH = "/";

static constexpr uint32_t SAMPLE_RATE = 16000;

// Wake word local: el modelo usa exactamente 1.5 s / 24000 muestras.
// Ya NO se envian wake probes al Jetson.
static constexpr float LOCAL_WAKE_THRESHOLD = 0.25f;

static constexpr float WAKE_THRESHOLD_NORMAL = 0.14f;
static constexpr float WAKE_THRESHOLD_STRONG = 0.24f;

static constexpr uint32_t WAKE_CONFIRM_WINDOW_MS = 6000;
static constexpr uint32_t WAKE_COOLDOWN_MS = 4000;

static uint8_t wakeCandidateHits = 0;
static uint32_t wakeFirstHitMs = 0;
static uint32_t wakeLastAcceptedMs = 0;

static float wakeEvidence = 0.0f;
static uint32_t wakeEvidenceStartMs = 0;

// Una vez despierto, graba 6 segundos para tu pregunta
static constexpr uint32_t QUERY_RECORD_SECONDS = 10;

// ==========================================================
// WEBSOCKET
// ==========================================================

WebSocketsClient webSocket;

bool wsConnected = false;
bool esperandoRespuesta = false;

// ==========================================================
// METRICAS EXPERIMENTALES
// ==========================================================

static uint32_t metricTestId = 0;

static uint32_t metricWakeStartMs = 0;
static uint32_t metricWakeDetectedMs = 0;

static uint32_t metricQueryStartMs = 0;
static uint32_t metricQuerySentMs = 0;
static uint32_t metricResponseReceivedMs = 0;

static uint32_t metricTtsStartMs = 0;
static uint32_t metricTtsEndMs = 0;

void metricLog(
    const char* evento,
    uint32_t valor = 0
)
{
    Serial.print("METRIC,");
    Serial.print(metricTestId);
    Serial.print(",");
    Serial.print(evento);
    Serial.print(",");
    Serial.println(valor);
}
bool wsError = false;

// ==========================================================
// VERIFICACION FINAL DE "MINI INDI" EN JETSON
// ==========================================================

bool wakeVerifyPending = false;
bool wakeVerifyResultPending = false;
bool wakeVerifyAccepted = false;

uint32_t wakeVerifyStartMs = 0;

static constexpr uint32_t WAKE_VERIFY_TIMEOUT_MS =
    7000;

uint32_t responseStartMs = 0;
static constexpr uint32_t RESPONSE_TIMEOUT_MS = 30000;


// ==========================================================
// IOT / ESP-NOW / TELEFONO
// ==========================================================

WebServer phoneServer(80);

static constexpr uint32_t CITIZEN_MAGIC = 0x4D494E49; // "MINI"

enum CitizenMsgType : uint8_t
{
    MSG_LIGHT_SET = 1,
    MSG_VIBRATION_ALERT = 2,
    MSG_HEARTBEAT = 3,
    MSG_DRIVE = 4,
    MSG_HEAD = 5,
    MSG_EMERGENCY_STOP = 6,
    MSG_LIGHT_COLOR = 7
};

enum DriveCommand : uint8_t
{
    DRIVE_STOP = 0,
    DRIVE_FORWARD = 1,
    DRIVE_BACKWARD = 2,
    DRIVE_LEFT = 3,
    DRIVE_RIGHT = 4
};

enum HeadCommand : uint8_t
{
    HEAD_CENTER = 0,
    HEAD_LEFT = 1,
    HEAD_RIGHT = 2
};

enum LightColor : uint8_t
{
    LIGHT_RED = 1,
    LIGHT_GREEN = 2,
    LIGHT_BLUE = 3,
    LIGHT_YELLOW = 4,
    LIGHT_PURPLE = 5,
    LIGHT_CYAN = 6,
    LIGHT_WHITE = 7,
    LIGHT_ORANGE = 8
};
struct __attribute__((packed)) CitizenPacket
{
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t value;
    uint8_t reserved;
    float level;
    uint32_t seq;
};

static const uint8_t BROADCAST_MAC[6] =
{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

volatile bool vibrationPending = false;
volatile float vibrationLevel = 0.0f;
volatile uint32_t lastVibrationMs = 0;

bool lightState = false;

bool systemEmergencyMode = false;
uint32_t espNowSeq = 0;

size_t queryWavBytesCurrent = 0;

// VAD de pregunta: 10 s es el maximo.
// Si detecta ~0.9 s de silencio despues de hablar, envia antes.
static constexpr uint32_t QUERY_SILENCE_MS = 500;
static constexpr uint32_t QUERY_MIN_SPEECH_MS = 250;
static constexpr uint32_t QUERY_WAIT_SPEECH_MS = 1800;
static constexpr float QUERY_RMS_THRESHOLD = 650.0f;

// ==========================================================
// PANTALLA
// ==========================================================

M5Canvas canvas(&M5.Display);

// ==========================================================
// ESTADOS
// ==========================================================

enum RobotState
{
    ROBOT_SLEEPING,
    ROBOT_AWAKE,
    ROBOT_LISTENING,
    ROBOT_THINKING,
    ROBOT_SPEAKING,
    ROBOT_ERROR
};

RobotState robotState = ROBOT_SLEEPING;

// ==========================================================
// ANIMACION
// ==========================================================

unsigned long lastBlinkMs = 0;
unsigned long blinkEndMs = 0;
unsigned long lastThinkingMs = 0;
unsigned long lastMouthMs = 0;

int eyeOffsetX = 0;
uint8_t mouthLevel = 0;

// ==========================================================
// COLORES
// ==========================================================

uint16_t COLOR_BG;
uint16_t COLOR_WHITE;
uint16_t COLOR_PURPLE;
uint16_t COLOR_PURPLE_LIGHT;
uint16_t COLOR_PURPLE_DARK;
uint16_t COLOR_PUPIL;
uint16_t COLOR_MOUTH;
uint16_t COLOR_TONGUE;
uint16_t COLOR_ERROR;

// ==========================================================
// WAKE WORD LOCAL - FRONTEND MFCC + SVM
// ==========================================================

static constexpr size_t LOCAL_WAKE_SAMPLES = INDI_AUDIO_SAMPLES;
static constexpr size_t LOCAL_NUM_FRAMES = INDI_NUM_FRAMES;
static constexpr size_t LOCAL_N_FFT = INDI_N_FFT;
static constexpr size_t LOCAL_FFT_BINS = INDI_FFT_BINS;
static constexpr size_t LOCAL_N_MELS = INDI_N_MELS;
static constexpr size_t LOCAL_N_MFCC = INDI_N_MFCC;
static constexpr size_t LOCAL_NUM_FEATURES = INDI_NUM_FEATURES;
static constexpr size_t LOCAL_NUM_SV = INDI_NUM_SUPPORT_VECTORS;

// ==========================================================
// AUDIO PREGUNTA
// ==========================================================

static constexpr size_t QUERY_SAMPLES =
    SAMPLE_RATE * QUERY_RECORD_SECONDS;

static constexpr size_t QUERY_PCM_BYTES =
    QUERY_SAMPLES * sizeof(int16_t);

static constexpr size_t QUERY_WAV_BYTES =
    44 + QUERY_PCM_BYTES;

// ==========================================================
// BUFFERS
// ==========================================================

static constexpr size_t WAKE_SCAN_SAMPLES = 32000;
static constexpr size_t WAKE_SHIFT_SAMPLES = 8000;

// Bloque continuo:
// 4096 muestras / 16000 Hz = 256 ms
static constexpr size_t WAKE_STREAM_BLOCK = 4096;

int16_t* wakePcmBuffer = nullptr;

// Ring buffer de 2 segundos.
int16_t* wakeRawBuffer = nullptr;

// Buffer donde I2S graba mientras el CPU procesa.
int16_t* wakeCaptureBuffer = nullptr;

static size_t wakeRingWrite = 0;
static size_t wakeRingFilled = 0;

static bool wakeMicContinuous = false;
static bool wakeBlockRecording = false;
uint8_t* queryBuffer = nullptr;

uint8_t* ttsBuffer = nullptr;

size_t ttsExpected = 0;
size_t ttsReceived = 0;

bool audioReady = false;

// Buffers del detector local de "Indi".
static float wakeFftReal[LOCAL_N_FFT];
static float wakeFftImag[LOCAL_N_FFT];

static float wakeMfcc[LOCAL_NUM_FRAMES][LOCAL_N_MFCC];
static float wakeDelta1[LOCAL_NUM_FRAMES][LOCAL_N_MFCC];
static float wakeDelta2[LOCAL_NUM_FRAMES][LOCAL_N_MFCC];

static float wakeFeatures[LOCAL_NUM_FEATURES];

ArduinoFFT<float> wakeFFT(
    wakeFftReal,
    wakeFftImag,
    LOCAL_N_FFT,
    SAMPLE_RATE
);

// ==========================================================
// COLORES
// ==========================================================

void initColors()
{
    COLOR_BG =
        M5.Display.color565(0, 0, 0);

    COLOR_WHITE =
        M5.Display.color565(255, 255, 255);

    COLOR_PURPLE =
        M5.Display.color565(145, 55, 205);

    COLOR_PURPLE_LIGHT =
        M5.Display.color565(215, 115, 255);

    COLOR_PURPLE_DARK =
        M5.Display.color565(80, 15, 125);

    COLOR_PUPIL =
        M5.Display.color565(20, 5, 25);

    COLOR_MOUTH =
        M5.Display.color565(205, 40, 80);

    COLOR_TONGUE =
        M5.Display.color565(255, 95, 135);

    COLOR_ERROR =
        M5.Display.color565(255, 50, 50);
}


// ==========================================================
// DECLARACIONES ADELANTADAS DE LA INTERFAZ
// ==========================================================

void drawSleepingFace();
void drawAwakeFace();
void drawEmergencyOverlay();
void updateFaceAnimation();
void setRobotState(RobotState state);

// ==========================================================
// PANEL WEB LOCAL
// ==========================================================

const char PHONE_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mini Indi</title>
<style>
body{font-family:Arial,sans-serif;background:#0d0d12;color:#fff;margin:0;padding:20px}
.card{max-width:520px;margin:auto;background:#1a1a24;padding:22px;border-radius:18px}
.status{background:#282836;padding:14px;border-radius:12px;margin:12px 0}
button{width:100%;padding:15px;margin:7px 0;border:0;border-radius:12px;font-size:18px}
.on{background:#75d98a}.off{background:#ff7e7e}
</style>
</head>
<body>
<div class="card">
<h1>Mini Indi</h1>
<p>Panel local de asistencia ciudadana</p>
<div id="status" class="status">Cargando...</div>
<button class="on" onclick="cmd('/api/light/on')">Encender luz</button>
<button class="off" onclick="cmd('/api/light/off')">Apagar luz</button>

<hr style="border-color:#444;margin:18px 0">

<button onclick="cmd('/api/drive/forward')">Avanzar</button>
<button onclick="cmd('/api/drive/backward')">Retroceder</button>
<button onclick="cmd('/api/drive/left')">Girar izquierda</button>
<button onclick="cmd('/api/drive/right')">Girar derecha</button>
<button class="off" onclick="cmd('/api/drive/stop')">STOP</button>

<hr style="border-color:#444;margin:18px 0">

<button onclick="cmd('/api/head/left')">Cabeza izquierda</button>
<button onclick="cmd('/api/head/center')">Cabeza centro</button>
<button onclick="cmd('/api/head/right')">Cabeza derecha</button>
</div>
<script>
async function cmd(u){await fetch(u,{method:'POST'});update();}
async function update(){
 try{
  const r=await fetch('/api/status');
  const j=await r.json();
  document.getElementById('status').innerHTML=
   'Jetson: '+(j.jetson?'CONECTADO':'DESCONECTADO')+
   '<br>Luz: '+(j.light?'ENCENDIDA':'APAGADA')+
   '<br>Alerta vibracion: '+(j.vibration?'ACTIVA':'no')+
   '<br>Nivel RMS: '+Number(j.level).toFixed(4)+' g';
 }catch(e){}
}
setInterval(update,700);update();
</script>
</body>
</html>
)HTML";

// ==========================================================
// ESP-NOW: ENVIAR LUZ
// ==========================================================

void enviarLuzEspNow(bool on)
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_LIGHT_SET;
    p.value = on ? 1 : 0;
    p.level = 0.0f;
    p.seq = ++espNowSeq;

    esp_err_t err =
        esp_now_send(
            BROADCAST_MAC,
            reinterpret_cast<const uint8_t*>(&p),
            sizeof(p)
        );

    lightState = on;

    Serial.print("ESP-NOW LUZ ");
    Serial.print(on ? "ON" : "OFF");
    Serial.print(" err=");
    Serial.println((int)err);
}


// ==========================================================
// ESP-NOW: MOTORES / CABEZA / HEARTBEAT
// ==========================================================

void enviarMovimiento(uint8_t command, uint8_t speedPercent = 65)
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_DRIVE;
    p.value = command;
    p.level = (float)speedPercent;
    p.seq = ++espNowSeq;

    esp_now_send(
        BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&p),
        sizeof(p)
    );

    Serial.print("ESP-NOW DRIVE cmd=");
    Serial.print(command);
    Serial.print(" speed=");
    Serial.println(speedPercent);
}

void enviarCabeza(uint8_t command)
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_HEAD;
    p.value = command;
    p.level = 0.0f;
    p.seq = ++espNowSeq;

    esp_now_send(
        BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&p),
        sizeof(p)
    );

    Serial.print("ESP-NOW HEAD cmd=");
    Serial.println(command);
}

void enviarParadaEmergencia()
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_EMERGENCY_STOP;
    p.value = 1;
    p.level = 0.0f;
    p.seq = ++espNowSeq;

    esp_now_send(
        BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&p),
        sizeof(p)
    );

    Serial.println("ESP-NOW EMERGENCY STOP");
}

void enviarHeartbeatEspNow()
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_HEARTBEAT;
    p.value = 1;
    p.level = 0.0f;
    p.seq = ++espNowSeq;

    esp_now_send(
        BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&p),
        sizeof(p)
    );
}

// ==========================================================
// ESP-NOW: RECIBIR ALERTAS
// ==========================================================


void enviarColorEspNow(uint8_t color)
{
    CitizenPacket p{};

    p.magic = CITIZEN_MAGIC;
    p.version = 1;
    p.type = MSG_LIGHT_COLOR;
    p.value = color;
    p.level = 0.0f;
    p.seq = ++espNowSeq;

    esp_now_send(
        BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&p),
        sizeof(p)
    );

    Serial.print("ESP-NOW COLOR=");
    Serial.println(color);
}

void onEspNowRecv(
    const uint8_t* mac,
    const uint8_t* data,
    int len
)
{
    if (len != (int)sizeof(CitizenPacket))
    {
        return;
    }

    CitizenPacket p;
    memcpy(&p, data, sizeof(p));

    if (
        p.magic != CITIZEN_MAGIC ||
        p.version != 1
    )
    {
        return;
    }

    if (p.type == MSG_VIBRATION_ALERT)
    {
        vibrationLevel = p.level;
        lastVibrationMs = millis();
        vibrationPending = true;
    }
}

// ==========================================================
// INICIAR ESP-NOW
// ==========================================================

bool iniciarEspNow()
{
    if (esp_now_init() != ESP_OK)
    {
        Serial.println("ERROR: esp_now_init");
        return false;
    }

    esp_now_register_recv_cb(onEspNowRecv);

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);

    // 0 = mismo canal que MINI_INDI_LAN
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    if (!esp_now_is_peer_exist(BROADCAST_MAC))
    {
        esp_err_t err = esp_now_add_peer(&peer);

        Serial.print("esp_now_add_peer=");
        Serial.println((int)err);
    }

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;

    esp_wifi_get_channel(
        &primary,
        &second
    );

    Serial.print("Canal WiFi/ESP-NOW: ");
    Serial.println(primary);

    Serial.print("MAC CoreS3: ");
    Serial.println(WiFi.macAddress());

    return true;
}

// ==========================================================
// PANEL WEB PARA TELEFONO
// ==========================================================

void iniciarPanelTelefono()
{
    if (MDNS.begin("miniindi"))
    {
        Serial.println("Panel: http://miniindi.local");
    }

    phoneServer.on(
        "/",
        HTTP_GET,
        []()
        {
            phoneServer.send_P(
                200,
                "text/html",
                PHONE_PAGE
            );
        }
    );

    phoneServer.on(
        "/api/light/on",
        HTTP_POST,
        []()
        {
            enviarLuzEspNow(true);

            phoneServer.send(
                200,
                "application/json",
                "{\"ok\":true}"
            );
        }
    );

    phoneServer.on(
        "/api/light/off",
        HTTP_POST,
        []()
        {
            enviarLuzEspNow(false);

            phoneServer.send(
                200,
                "application/json",
                "{\"ok\":true}"
            );
        }
    );

    
    phoneServer.on(
        "/api/drive/forward",
        HTTP_POST,
        []()
        {
            enviarMovimiento(DRIVE_FORWARD, 65);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/drive/backward",
        HTTP_POST,
        []()
        {
            enviarMovimiento(DRIVE_BACKWARD, 55);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/drive/left",
        HTTP_POST,
        []()
        {
            enviarMovimiento(DRIVE_LEFT, 55);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/drive/right",
        HTTP_POST,
        []()
        {
            enviarMovimiento(DRIVE_RIGHT, 55);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/drive/stop",
        HTTP_POST,
        []()
        {
            enviarParadaEmergencia();
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/head/left",
        HTTP_POST,
        []()
        {
            enviarCabeza(HEAD_LEFT);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/head/center",
        HTTP_POST,
        []()
        {
            enviarCabeza(HEAD_CENTER);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

    phoneServer.on(
        "/api/head/right",
        HTTP_POST,
        []()
        {
            enviarCabeza(HEAD_RIGHT);
            phoneServer.send(200, "application/json", "{\"ok\":true}");
        }
    );

phoneServer.on(
        "/api/status",
        HTTP_GET,
        []()
        {
            bool recent =
                lastVibrationMs != 0 &&
                (millis() - lastVibrationMs) < 15000;

            String json = "{";

            json += "\"jetson\":";
            json += wsConnected ? "true" : "false";

            json += ",\"light\":";
            json += lightState ? "true" : "false";

            json += ",\"vibration\":";
            json += recent ? "true" : "false";

            json += ",\"level\":";
            json += String((float)vibrationLevel, 4);

            json += "}";

            phoneServer.send(
                200,
                "application/json",
                json
            );
        }
    );

    phoneServer.begin();

    Serial.println("Servidor web local iniciado");
}

// ==========================================================
// ALERTA VISUAL SIN PERDER LA CARA
// ==========================================================

void mostrarAlertaVibracion(float level)
{
    // Seguridad: ante una alerta fuerte, detener la base.
    enviarParadaEmergencia();

    RobotState estadoAnterior = robotState;

    // Dibuja la cara normal primero y agrega una banda de alerta.
    if (estadoAnterior == ROBOT_SLEEPING)
    {
        drawSleepingFace();
    }
    else
    {
        drawAwakeFace();
    }

    canvas.fillRoundRect(
        34,
        10,
        252,
        38,
        10,
        COLOR_ERROR
    );

    canvas.setTextDatum(middle_center);
    canvas.setTextColor(COLOR_WHITE, COLOR_ERROR);
    canvas.setTextSize(1);

    canvas.drawString(
        "ALERTA DE VIBRACION",
        160,
        22
    );

    canvas.drawString(
        String(level, 4) + " g RMS",
        160,
        36
    );

    canvas.pushSprite(0, 0);

    Serial.print("ALERTA VIBRACION: ");
    Serial.println(level, 4);

    // No interrumpir TTS ni grabacion con el altavoz.
    if (
        estadoAnterior != ROBOT_SPEAKING &&
        estadoAnterior != ROBOT_LISTENING
    )
    {
        M5.Mic.end();

        if (M5.Speaker.begin())
        {
            M5.Speaker.setVolume(160);
            M5.Speaker.tone(1200, 300);
            delay(340);
            M5.Speaker.end();
        }
    }

    delay(500);

    if (estadoAnterior == ROBOT_SLEEPING)
    {
        drawSleepingFace();
    }
    else
    {
        drawAwakeFace();
    }
}


// ==========================================================
// INDICADOR VISUAL DE EMERGENCIA
// Conserva la cara original.
// ==========================================================

void drawEmergencyOverlay()
{
    if (!systemEmergencyMode)
    {
        return;
    }

    const uint16_t emergencyColor =
        0xF800;

    int w =
        M5.Display.width();

    int h =
        M5.Display.height();

    M5.Display.drawRect(
        1,
        1,
        w - 2,
        h - 2,
        emergencyColor
    );

    M5.Display.drawRect(
        3,
        3,
        w - 6,
        h - 6,
        emergencyColor
    );

    M5.Display.fillRect(
        6,
        6,
        82,
        18,
        0x0000
    );

    M5.Display.setTextColor(
        emergencyColor,
        0x0000
    );

    M5.Display.setTextSize(1);

    M5.Display.setCursor(
        10,
        11
    );

    M5.Display.print(
        "EMERGENCY"
    );
}

// ==========================================================
// COMANDOS JSON ESTRUCTURADOS DESDE JETSON
// Requiere keyword exacta: "mini indi"
// ==========================================================

bool procesarComandoJson(JsonObject command)
{
    const char* keyword =
        command["keyword"] | "";

    String key =
        String(keyword);

    key.toLowerCase();
    key.trim();

    if (
        key !=
        "mini indi"
    )
    {
        Serial.println(
            "JSON RECHAZADO: keyword invalida"
        );

        return false;
    }

    const char* targetC =
        command["target"] | "";

    const char* actionC =
        command["action"] | "";

    String target =
        String(targetC);

    String action =
        String(actionC);

    target.toLowerCase();
    action.toLowerCase();

    int value =
        command["value"] | 60;

    value =
        constrain(
            value,
            0,
            100
        );

    // ---------------- SISTEMA ----------------

    if (
        target == "system"
    )
    {
        if (
            action == "emergency_on"
        )
        {
            systemEmergencyMode =
                true;

            Serial.println();
            Serial.println(
                "================================"
            );
            Serial.println(
                "CORE: MODO EMERGENCIA ACTIVO"
            );
            Serial.println(
                "================================"
            );

            // Seguridad inmediata:
            // detener cualquier movimiento.
            enviarParadaEmergencia();

            // Se?alizacion de emergencia:
            // luz encendida y color rojo.
            enviarLuzEspNow(
                true
            );

            delay(40);

            enviarColorEspNow(
                LIGHT_RED
            );

            Serial.println(
                "EMERGENCY SIGNAL: LIGHT RED"
            );

            // Mantener la cara original
            // y superponer indicador.
            drawEmergencyOverlay();

            return true;
        }

        return false;
    }

    // ---------------- LUZ ----------------

    if (
        target == "light"
    )
    {
        if (
            action == "on"
        )
        {
            enviarLuzEspNow(
                true
            );

            return true;
        }

        if (
            action == "off"
        )
        {
            if (
                systemEmergencyMode
            )
            {
                Serial.println(
                    "CORE SAFETY: LIGHT OFF BLOQUEADO"
                );

                return false;
            }

            enviarLuzEspNow(
                false
            );

            return true;
        }

        if (
            action == "color"
        )
        {
            String color =
                String(
                    command["color"] | ""
                );

            color.toLowerCase();

            uint8_t colorCode = 0;

            if (color == "red" || color == "rojo")
                colorCode = LIGHT_RED;
            else if (color == "green" || color == "verde")
                colorCode = LIGHT_GREEN;
            else if (color == "blue" || color == "azul")
                colorCode = LIGHT_BLUE;
            else if (color == "yellow" || color == "amarillo")
                colorCode = LIGHT_YELLOW;
            else if (color == "purple" || color == "morado")
                colorCode = LIGHT_PURPLE;
            else if (color == "cyan" || color == "cian")
                colorCode = LIGHT_CYAN;
            else if (color == "white" || color == "blanco")
                colorCode = LIGHT_WHITE;
            else if (color == "orange" || color == "naranja")
                colorCode = LIGHT_ORANGE;

            if (colorCode == 0)
                return false;

            enviarColorEspNow(
                colorCode
            );

            return true;
        }

        return false;
    }

    // ---------------- RUEDAS ----------------

    if (
        target == "drive"
    )
    {
        if (
            action == "forward"
        )
        {
            enviarMovimiento(
                DRIVE_FORWARD,
                value
            );

            return true;
        }

        if (
            action == "backward"
        )
        {
            enviarMovimiento(
                DRIVE_BACKWARD,
                value
            );

            return true;
        }

        if (
            action == "left"
        )
        {
            enviarMovimiento(
                DRIVE_LEFT,
                value
            );

            return true;
        }

        if (
            action == "right"
        )
        {
            enviarMovimiento(
                DRIVE_RIGHT,
                value
            );

            return true;
        }

        if (
            action == "stop"
        )
        {
            enviarParadaEmergencia();

            return true;
        }

        return false;
    }

    // ---------------- CABEZA ----------------

    if (
        target == "head"
    )
    {
        if (
            action == "left"
        )
        {
            enviarCabeza(
                HEAD_LEFT
            );

            return true;
        }

        if (
            action == "right"
        )
        {
            enviarCabeza(
                HEAD_RIGHT
            );

            return true;
        }

        if (
            action == "center"
        )
        {
            enviarCabeza(
                HEAD_CENTER
            );

            return true;
        }

        return false;
    }

    Serial.println(
        "JSON RECHAZADO: target no permitido"
    );

    return false;
}

// ==========================================================
// INTERPRETAR COMANDOS DE LUZ DESDE TRANSCRIPCION
// ==========================================================

void procesarComandoLocal(String texto)
{
    texto.toLowerCase();

    if (
        texto.indexOf("prende la luz") >= 0 ||
        texto.indexOf("enciende la luz") >= 0 ||
        texto.indexOf("prender la luz") >= 0 ||
        texto.indexOf("encender la luz") >= 0
    )
    {
        enviarLuzEspNow(true);
        return;
    }

    if (
        texto.indexOf("apaga la luz") >= 0 ||
        texto.indexOf("apagar la luz") >= 0
    )
    {
        enviarLuzEspNow(false);
        return;
    }

    if (
        texto.indexOf("avanza") >= 0 ||
        texto.indexOf("adelante") >= 0
    )
    {
        enviarMovimiento(DRIVE_FORWARD, 65);
        return;
    }

    if (
        texto.indexOf("retrocede") >= 0 ||
        texto.indexOf("atras") >= 0 ||
        texto.indexOf("atrás") >= 0
    )
    {
        enviarMovimiento(DRIVE_BACKWARD, 55);
        return;
    }

    if (
        texto.indexOf("gira a la izquierda") >= 0 ||
        texto.indexOf("izquierda") >= 0
    )
    {
        enviarMovimiento(DRIVE_LEFT, 55);
        return;
    }

    if (
        texto.indexOf("gira a la derecha") >= 0 ||
        texto.indexOf("derecha") >= 0
    )
    {
        enviarMovimiento(DRIVE_RIGHT, 55);
        return;
    }

    if (
        texto.indexOf("detente") >= 0 ||
        texto.indexOf("parate") >= 0 ||
        texto.indexOf("párate") >= 0 ||
        texto.indexOf("stop") >= 0
    )
    {
        enviarParadaEmergencia();
        return;
    }

    if (
        texto.indexOf("cabeza a la izquierda") >= 0 ||
        texto.indexOf("mira a la izquierda") >= 0
    )
    {
        enviarCabeza(HEAD_LEFT);
        return;
    }

    if (
        texto.indexOf("cabeza a la derecha") >= 0 ||
        texto.indexOf("mira a la derecha") >= 0
    )
    {
        enviarCabeza(HEAD_RIGHT);
        return;
    }

    if (
        texto.indexOf("centra la cabeza") >= 0 ||
        texto.indexOf("mira al frente") >= 0
    )
    {
        enviarCabeza(HEAD_CENTER);
        return;
    }
}


// ==========================================================
// WAV
// ==========================================================

void write16(uint8_t* p, uint16_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
}

void write32(uint8_t* p, uint32_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

void crearCabeceraWav(
    uint8_t* buffer,
    size_t pcmBytes
)
{
    memcpy(buffer + 0, "RIFF", 4);

    write32(
        buffer + 4,
        36 + pcmBytes
    );

    memcpy(buffer + 8, "WAVE", 4);
    memcpy(buffer + 12, "fmt ", 4);

    write32(buffer + 16, 16);
    write16(buffer + 20, 1);
    write16(buffer + 22, 1);

    write32(
        buffer + 24,
        SAMPLE_RATE
    );

    write32(
        buffer + 28,
        SAMPLE_RATE * 2
    );

    write16(buffer + 32, 2);
    write16(buffer + 34, 16);

    memcpy(buffer + 36, "data", 4);

    write32(
        buffer + 40,
        pcmBytes
    );
}

// ==========================================================
// PARPADEO
// ==========================================================

bool isBlinking()
{
    return millis() < blinkEndMs;
}

void iniciarParpadeo()
{
    blinkEndMs = millis() + 120;
    lastBlinkMs = millis();
}

// ==========================================================
// CEJAS
// ==========================================================

void drawEyebrows()
{
    // izquierda
    canvas.drawLine(57, 52, 68, 41, COLOR_WHITE);
    canvas.drawLine(68, 41, 82, 34, COLOR_WHITE);
    canvas.drawLine(82, 34, 96, 32, COLOR_WHITE);
    canvas.drawLine(96, 32, 111, 35, COLOR_WHITE);
    canvas.drawLine(111, 35, 126, 46, COLOR_WHITE);

    canvas.drawLine(57, 53, 68, 42, COLOR_WHITE);
    canvas.drawLine(68, 42, 82, 35, COLOR_WHITE);
    canvas.drawLine(82, 35, 96, 33, COLOR_WHITE);

    // derecha
    canvas.drawLine(194, 46, 209, 35, COLOR_WHITE);
    canvas.drawLine(209, 35, 224, 32, COLOR_WHITE);
    canvas.drawLine(224, 32, 238, 34, COLOR_WHITE);
    canvas.drawLine(238, 34, 252, 41, COLOR_WHITE);
    canvas.drawLine(252, 41, 263, 52, COLOR_WHITE);

    canvas.drawLine(209, 36, 224, 33, COLOR_WHITE);
    canvas.drawLine(224, 33, 238, 35, COLOR_WHITE);
    canvas.drawLine(238, 35, 252, 42, COLOR_WHITE);
}

// ==========================================================
// OJOS DESPIERTOS
// ==========================================================

void drawAwakeEye(
    int cx,
    int cy,
    int offsetX,
    bool blink
)
{
    if (blink)
    {
        canvas.fillRoundRect(
            cx - 38,
            cy,
            76,
            6,
            3,
            COLOR_WHITE
        );

        return;
    }

    canvas.fillEllipse(
        cx,
        cy,
        42,
        55,
        COLOR_WHITE
    );

    int irisX = cx + offsetX;

    canvas.fillEllipse(
        irisX,
        cy + 5,
        25,
        38,
        COLOR_PURPLE_DARK
    );

    canvas.fillEllipse(
        irisX,
        cy + 3,
        21,
        33,
        COLOR_PURPLE
    );

    canvas.fillEllipse(
        irisX,
        cy - 5,
        17,
        21,
        COLOR_PURPLE_LIGHT
    );

    canvas.fillEllipse(
        irisX,
        cy + 6,
        14,
        25,
        COLOR_PUPIL
    );

    canvas.fillCircle(
        irisX + 8,
        cy - 10,
        7,
        COLOR_WHITE
    );

    canvas.fillCircle(
        irisX + 1,
        cy + 3,
        3,
        COLOR_WHITE
    );
}

// ==========================================================
// OJOS DORMIDOS
// ==========================================================

void drawSleepingEye(
    int cx,
    int cy
)
{
    canvas.drawLine(
        cx - 35,
        cy - 3,
        cx - 22,
        cy + 7,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx - 22,
        cy + 7,
        cx,
        cy + 11,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx,
        cy + 11,
        cx + 22,
        cy + 7,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx + 22,
        cy + 7,
        cx + 35,
        cy - 3,
        COLOR_WHITE
    );

    // grosor
    canvas.drawLine(
        cx - 34,
        cy - 1,
        cx - 21,
        cy + 9,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx - 21,
        cy + 9,
        cx,
        cy + 13,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx,
        cy + 13,
        cx + 21,
        cy + 9,
        COLOR_WHITE
    );

    canvas.drawLine(
        cx + 21,
        cy + 9,
        cx + 34,
        cy - 1,
        COLOR_WHITE
    );
}

// ==========================================================
// SONRISA
// ==========================================================

void drawSmile()
{
    canvas.fillEllipse(
        160,
        181,
        32,
        12,
        COLOR_WHITE
    );

    canvas.fillEllipse(
        160,
        173,
        32,
        12,
        COLOR_BG
    );
}

// ==========================================================
// BOCA HABLANDO
// ==========================================================

void drawTalkingMouth(uint8_t level)
{
    const int cx = 160;

    if (level == 0)
    {
        drawSmile();
        return;
    }

    if (level == 1)
    {
        canvas.fillEllipse(
            cx,
            181,
            31,
            15,
            COLOR_WHITE
        );

        canvas.fillEllipse(
            cx,
            181,
            26,
            10,
            COLOR_MOUTH
        );

        return;
    }

    if (level == 2)
    {
        canvas.fillEllipse(
            cx,
            181,
            40,
            24,
            COLOR_WHITE
        );

        canvas.fillEllipse(
            cx,
            182,
            34,
            18,
            COLOR_MOUTH
        );

        canvas.fillEllipse(
            cx,
            190,
            19,
            7,
            COLOR_TONGUE
        );

        return;
    }

    canvas.fillEllipse(
        cx,
        182,
        45,
        30,
        COLOR_WHITE
    );

    canvas.fillEllipse(
        cx,
        183,
        39,
        24,
        COLOR_MOUTH
    );

    canvas.fillEllipse(
        cx,
        195,
        24,
        9,
        COLOR_TONGUE
    );
}

// ==========================================================
// CARA DORMIDA
// ==========================================================

void drawSleepingFace()
{
    canvas.fillSprite(
        COLOR_BG
    );

    drawSleepingEye(
        93,
        96
    );

    drawSleepingEye(
        227,
        96
    );

    // sonrisa tranquila
    canvas.fillEllipse(
        160,
        177,
        25,
        8,
        COLOR_WHITE
    );

    canvas.fillEllipse(
        160,
        172,
        25,
        8,
        COLOR_BG
    );

    canvas.pushSprite(
        0,
        0
    );
}

// ==========================================================
// CARA DESPIERTA
// ==========================================================

void drawAwakeFace()
{
    canvas.fillSprite(
        COLOR_BG
    );

    drawEyebrows();

    bool blink = isBlinking();

    drawAwakeEye(
        93,
        100,
        eyeOffsetX,
        blink
    );

    drawAwakeEye(
        227,
        100,
        eyeOffsetX,
        blink
    );

    if (
        robotState ==
        ROBOT_SPEAKING
    )
    {
        drawTalkingMouth(
            mouthLevel
        );
    }
    else if (
        robotState ==
        ROBOT_ERROR
    )
    {
        canvas.fillRoundRect(
            130,
            178,
            60,
            7,
            3,
            COLOR_ERROR
        );
    }
    else
    {
        drawSmile();
    }

    canvas.pushSprite(
        0,
        0
    );
}

// ==========================================================
// CAMBIAR ESTADO
// ==========================================================

void setRobotState(
    RobotState state
)
{
    robotState = state;

    if (
        state ==
        ROBOT_SLEEPING
    )
    {
        eyeOffsetX = 0;
        mouthLevel = 0;

        drawSleepingFace();

        Serial.println();
        Serial.println(
            "ROBOT DORMIDO"
        );

        return;
    }

    if (
        state ==
        ROBOT_AWAKE
    )
    {
        eyeOffsetX = 0;

        Serial.println();
        Serial.println(
            "ROBOT DESPIERTO"
        );
    }

    if (
        state ==
        ROBOT_LISTENING
    )
    {
        Serial.println(
            "ESCUCHANDO..."
        );
    }

    if (
        state ==
        ROBOT_THINKING
    )
    {
        Serial.println(
            "PENSANDO..."
        );
    }

    drawAwakeFace();
}

// ==========================================================
// ANIMACION
// ==========================================================

void updateFaceAnimation()
{
    unsigned long now =
        millis();

    if (
        robotState ==
        ROBOT_SLEEPING
    )
    {
        return;
    }

    bool redraw = false;

    static unsigned long blinkInterval =
        3500;

    static bool oldBlink = false;

    if (
        !isBlinking() &&
        now - lastBlinkMs >
        blinkInterval
    )
    {
        iniciarParpadeo();

        blinkInterval =
            random(
                2800,
                5000
            );

        redraw = true;
    }

    bool newBlink =
        isBlinking();

    if (
        newBlink !=
        oldBlink
    )
    {
        oldBlink =
            newBlink;

        redraw =
            true;
    }

    if (
        robotState ==
        ROBOT_THINKING
    )
    {
        if (
            now -
            lastThinkingMs >
            600
        )
        {
            lastThinkingMs =
                now;

            if (
                eyeOffsetX == -5
            )
            {
                eyeOffsetX = 0;
            }
            else if (
                eyeOffsetX == 0
            )
            {
                eyeOffsetX = 5;
            }
            else
            {
                eyeOffsetX = -5;
            }

            redraw =
                true;
        }
    }
    else if (
        eyeOffsetX != 0
    )
    {
        eyeOffsetX = 0;
        redraw = true;
    }

    if (
        robotState ==
        ROBOT_SPEAKING &&
        now -
        lastMouthMs >
        120
    )
    {
        lastMouthMs =
            now;

        uint8_t nuevo =
            random(
                0,
                4
            );

        if (
            nuevo ==
            mouthLevel
        )
        {
            nuevo =
                (mouthLevel + 1) %
                4;
        }

        mouthLevel =
            nuevo;

        redraw =
            true;
    }

    if (redraw)
    {
        drawAwakeFace();
    }
}

// ==========================================================
// CAPTURAR PREGUNTA CON VAD
// ==========================================================

size_t capturarPreguntaVAD(
    uint8_t* wav,
    size_t maxSamples
)
{
    int16_t* pcm =
        reinterpret_cast<int16_t*>(
            wav + 44
        );

    memset(
        pcm,
        0,
        maxSamples * sizeof(int16_t)
    );

    M5.Speaker.end();
    M5.Mic.begin();

    size_t capturados = 0;

    const size_t BLOCK = 320; // 20 ms a 16 kHz

    bool speechStarted = false;

    uint32_t speechMs = 0;
    uint32_t silenceMs = 0;
    uint32_t waitSpeechMs = 0;

    while (capturados < maxSamples)
    {
        size_t cantidad =
            maxSamples - capturados;

        if (cantidad > BLOCK)
        {
            cantidad = BLOCK;
        }

        bool ok =
            M5.Mic.record(
                pcm + capturados,
                cantidad,
                SAMPLE_RATE
            );

        if (!ok)
        {
            webSocket.loop();
            phoneServer.handleClient();
            delay(1);
            continue;
        }

        while (M5.Mic.isRecording())
        {
            M5.update();
            webSocket.loop();
            phoneServer.handleClient();
            updateFaceAnimation();
            delay(1);
        }

        double sumSq = 0.0;

        for (size_t i = 0; i < cantidad; i++)
        {
            const float s =
                (float)pcm[capturados + i];

            sumSq +=
                (double)s * (double)s;
        }

        const float rms =
            sqrtf(
                (float)(
                    sumSq /
                    (double)cantidad
                )
            );

        const uint32_t blockMs =
            (uint32_t)(
                (1000ULL * cantidad) /
                SAMPLE_RATE
            );

        if (rms >= QUERY_RMS_THRESHOLD)
        {
            speechStarted = true;
            speechMs += blockMs;
            silenceMs = 0;
        }
        else
        {
            if (speechStarted)
            {
                silenceMs += blockMs;
            }
            else
            {
                waitSpeechMs += blockMs;
            }
        }

        capturados += cantidad;

        if (
            speechStarted &&
            speechMs >= QUERY_MIN_SPEECH_MS &&
            silenceMs >= QUERY_SILENCE_MS
        )
        {
            Serial.println(
                "VAD: fin de voz detectado"
            );

            break;
        }

        if (
            !speechStarted &&
            waitSpeechMs >= QUERY_WAIT_SPEECH_MS
        )
        {
            Serial.println(
                "VAD: no se detecto voz"
            );

            break;
        }
    }

    M5.Mic.end();

    if (capturados < 1)
    {
        capturados = 1;
    }

    const size_t pcmBytes =
        capturados * sizeof(int16_t);

    crearCabeceraWav(
        wav,
        pcmBytes
    );

    Serial.print(
        "Audio pregunta: "
    );

    Serial.print(
        (unsigned long)(
            1000ULL * capturados /
            SAMPLE_RATE
        )
    );

    Serial.println(
        " ms"
    );

    return 44 + pcmBytes;
}

// ==========================================================
// LIBERAR TTS
// ==========================================================

void liberarTTS()
{
    if (
        ttsBuffer != nullptr
    )
    {
        heap_caps_free(
            ttsBuffer
        );

        ttsBuffer = nullptr;
    }

    ttsExpected = 0;
    ttsReceived = 0;
    audioReady = false;
}

// ==========================================================
// CAPTURA LOCAL PARA WAKE WORD
// ==========================================================





// ==========================================================
// CAPTURA LOCAL DEL WAKE WORD
// ==========================================================

bool iniciarBloqueWakeContinuo()
{
    if (
        !wakeMicContinuous ||
        wakeCaptureBuffer == nullptr
    )
    {
        return false;
    }

    bool ok =
        M5.Mic.record(
            wakeCaptureBuffer,
            WAKE_STREAM_BLOCK,
            SAMPLE_RATE
        );

    if (ok)
    {
        wakeBlockRecording = true;
    }

    return ok;
}


// ==========================================================
// INICIAR MICROFONO WAKE CONTINUO
// ==========================================================

bool iniciarWakeContinuo()
{
    if (wakeMicContinuous)
    {
        return true;
    }

    wakeRingWrite = 0;
    wakeRingFilled = 0;

    memset(
        wakeRawBuffer,
        0,
        WAKE_SCAN_SAMPLES * sizeof(int16_t)
    );

    memset(
        wakeCaptureBuffer,
        0,
        WAKE_STREAM_BLOCK * sizeof(int16_t)
    );

    if (!M5.Mic.begin())
    {
        Serial.println(
            "ERROR: no se pudo iniciar wake continuo"
        );

        return false;
    }

    wakeMicContinuous = true;
    wakeBlockRecording = false;

    if (!iniciarBloqueWakeContinuo())
    {
        M5.Mic.end();

        wakeMicContinuous = false;

        Serial.println(
            "ERROR: no se pudo iniciar primer bloque wake"
        );

        return false;
    }

    Serial.println(
        "WAKE CONTINUO INICIADO"
    );

    return true;
}


// ==========================================================
// DETENER MICROFONO WAKE CONTINUO
// ==========================================================

void detenerWakeContinuo()
{
    if (!wakeMicContinuous)
    {
        return;
    }

    M5.Mic.end();

    wakeMicContinuous = false;
    wakeBlockRecording = false;

    wakeRingWrite = 0;
    wakeRingFilled = 0;

    Serial.println(
        "WAKE CONTINUO DETENIDO"
    );
}


// ==========================================================
// COPIAR BLOQUE TERMINADO AL RING BUFFER
// ==========================================================

void agregarBloqueWakeRing()
{
    for (
        size_t i = 0;
        i < WAKE_STREAM_BLOCK;
        i++
    )
    {
        wakeRawBuffer[
            wakeRingWrite
        ] =
            wakeCaptureBuffer[i];

        wakeRingWrite++;

        if (
            wakeRingWrite >=
            WAKE_SCAN_SAMPLES
        )
        {
            wakeRingWrite = 0;
        }

        if (
            wakeRingFilled <
            WAKE_SCAN_SAMPLES
        )
        {
            wakeRingFilled++;
        }
    }
}


// ==========================================================
// EXTRAER LOS ULTIMOS 1.5 SEGUNDOS
// ==========================================================

bool copiarUltimaVentanaWake()
{
    if (
        wakeRingFilled <
        LOCAL_WAKE_SAMPLES
    )
    {
        return false;
    }

    size_t inicio;

    if (
        wakeRingWrite >=
        LOCAL_WAKE_SAMPLES
    )
    {
        inicio =
            wakeRingWrite
            - LOCAL_WAKE_SAMPLES;
    }
    else
    {
        inicio =
            WAKE_SCAN_SAMPLES
            -
            (
                LOCAL_WAKE_SAMPLES
                -
                wakeRingWrite
            );
    }

    for (
        size_t i = 0;
        i < LOCAL_WAKE_SAMPLES;
        i++
    )
    {
        size_t indice =
            inicio + i;

        if (
            indice >=
            WAKE_SCAN_SAMPLES
        )
        {
            indice -=
                WAKE_SCAN_SAMPLES;
        }

        wakePcmBuffer[i] =
            wakeRawBuffer[indice];
    }

    return true;
}
// ==========================================================
// QUITAR DC DEL AUDIO DE WAKE
// ==========================================================

void quitarDCWake()
{
    double suma = 0.0;

    for (size_t i = 0; i < LOCAL_WAKE_SAMPLES; i++)
    {
        suma += wakePcmBuffer[i];
    }

    float media =
        (float)(suma / LOCAL_WAKE_SAMPLES);

    for (size_t i = 0; i < LOCAL_WAKE_SAMPLES; i++)
    {
        float valor =
            (float)wakePcmBuffer[i] - media;

        if (valor > 32767.0f)
        {
            valor = 32767.0f;
        }

        if (valor < -32768.0f)
        {
            valor = -32768.0f;
        }

        wakePcmBuffer[i] = (int16_t)valor;
    }
}


// ==========================================================
// MFCC DEL WAKE WORD
// ==========================================================

void calcularWakeMFCC()
{
    static float powerSpectrum[LOCAL_FFT_BINS];
    float logMel[LOCAL_N_MELS];

    const float invFftSize =
        1.0f / (float)LOCAL_N_FFT;

    const float invPcm =
        1.0f / 32768.0f;

    for (
        size_t frameIndex = 0;
        frameIndex < LOCAL_NUM_FRAMES;
        frameIndex++
    )
    {
        const size_t inicio =
            frameIndex * INDI_HOP_LENGTH;

        // ==================================================
        // LIMPIAR FFT
        // ==================================================

        memset(
            wakeFftReal,
            0,
            LOCAL_N_FFT * sizeof(float)
        );

        memset(
            wakeFftImag,
            0,
            LOCAL_N_FFT * sizeof(float)
        );

        // ==================================================
        // FRAME + HANN
        // ==================================================

        for (
            size_t i = 0;
            i < INDI_FRAME_LENGTH;
            i++
        )
        {
            const float muestra =
                (float)wakePcmBuffer[
                    inicio + i
                ]
                *
                invPcm;

            wakeFftReal[i] =
                muestra
                *
                indi_hann_window[i];
        }

        // ==================================================
        // FFT
        // ==================================================

        wakeFFT.compute(
            FFTDirection::Forward
        );

        // IMPORTANTE:
        //
        // Ya NO llamamos:
        //
        // wakeFFT.complexToMagnitude();
        //
        // Magnitud^2 = Real^2 + Imag^2
        //
        // Como necesitamos potencia, evitamos completamente
        // la raiz cuadrada de complexToMagnitude().
        // ==================================================

        for (
            size_t k = 0;
            k < LOCAL_FFT_BINS;
            k++
        )
        {
            const float re =
                wakeFftReal[k];

            const float im =
                wakeFftImag[k];

            powerSpectrum[k] =
                (
                    re * re
                    +
                    im * im
                )
                *
                invFftSize;
        }

        // ==================================================
        // MEL FILTERBANK
        // ==================================================

        for (
            size_t m = 0;
            m < LOCAL_N_MELS;
            m++
        )
        {
            float energia =
                0.0f;

            for (
                size_t k = 0;
                k < LOCAL_FFT_BINS;
                k++
            )
            {
                energia +=
                    indi_mel_filterbank[m][k]
                    *
                    powerSpectrum[k];
            }

            if (
                energia <
                1.0e-10f
            )
            {
                energia =
                    1.0e-10f;
            }

            logMel[m] =
                logf(
                    energia
                );
        }

        // ==================================================
        // DCT -> MFCC
        // ==================================================

        for (
            size_t c = 0;
            c < LOCAL_N_MFCC;
            c++
        )
        {
            float acumulado =
                0.0f;

            for (
                size_t m = 0;
                m < LOCAL_N_MELS;
                m++
            )
            {
                acumulado +=
                    indi_dct_matrix[c][m]
                    *
                    logMel[m];
            }

            wakeMfcc[
                frameIndex
            ][c] =
                acumulado;
        }
    }
}


// ==========================================================
// DELTA N=2
// ==========================================================

void calcularWakeDelta(
    float entrada[LOCAL_NUM_FRAMES][LOCAL_N_MFCC],
    float salida[LOCAL_NUM_FRAMES][LOCAL_N_MFCC]
)
{
    const int N = 2;
    float denominador = 0.0f;

    for (int n = 1; n <= N; n++)
    {
        denominador +=
            2.0f * n * n;
    }

    for (
        int t = 0;
        t < (int)LOCAL_NUM_FRAMES;
        t++
    )
    {
        for (
            int c = 0;
            c < (int)LOCAL_N_MFCC;
            c++
        )
        {
            float acumulado = 0.0f;

            for (int n = 1; n <= N; n++)
            {
                int izquierda = t - n;
                int derecha = t + n;

                if (izquierda < 0)
                {
                    izquierda = 0;
                }

                if (
                    derecha >=
                    (int)LOCAL_NUM_FRAMES
                )
                {
                    derecha =
                        (int)LOCAL_NUM_FRAMES - 1;
                }

                acumulado +=
                    (float)n
                    *
                    (
                        entrada[derecha][c]
                        -
                        entrada[izquierda][c]
                    );
            }

            salida[t][c] =
                acumulado / denominador;
        }
    }
}


// ==========================================================
// MEDIA Y DESVIACION DEL FRONTEND
// ==========================================================

void wakeMediaStd(
    float matriz[LOCAL_NUM_FRAMES][LOCAL_N_MFCC],
    float* medias,
    float* desv
)
{
    for (
        size_t c = 0;
        c < LOCAL_N_MFCC;
        c++
    )
    {
        double suma = 0.0;

        for (
            size_t t = 0;
            t < LOCAL_NUM_FRAMES;
            t++
        )
        {
            suma += matriz[t][c];
        }

        const float media =
            (float)(suma / LOCAL_NUM_FRAMES);

        medias[c] = media;

        double var = 0.0;

        for (
            size_t t = 0;
            t < LOCAL_NUM_FRAMES;
            t++
        )
        {
            const double d =
                (double)matriz[t][c]
                -
                (double)media;

            var += d * d;
        }

        var /= LOCAL_NUM_FRAMES;

        desv[c] =
            sqrtf((float)var);
    }
}


// ==========================================================
// CREAR LAS 120 FEATURES
// ==========================================================

void crearWakeFeatures()
{
    float meanMFCC[LOCAL_N_MFCC];
    float stdMFCC[LOCAL_N_MFCC];

    float meanD1[LOCAL_N_MFCC];
    float stdD1[LOCAL_N_MFCC];

    float meanD2[LOCAL_N_MFCC];
    float stdD2[LOCAL_N_MFCC];

    wakeMediaStd(
        wakeMfcc,
        meanMFCC,
        stdMFCC
    );

    wakeMediaStd(
        wakeDelta1,
        meanD1,
        stdD1
    );

    wakeMediaStd(
        wakeDelta2,
        meanD2,
        stdD2
    );

    size_t idx = 0;

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = meanMFCC[i];
    }

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = stdMFCC[i];
    }

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = meanD1[i];
    }

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = stdD1[i];
    }

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = meanD2[i];
    }

    for (size_t i = 0; i < LOCAL_N_MFCC; i++)
    {
        wakeFeatures[idx++] = stdD2[i];
    }
}


// ==========================================================
// SVM RBF LOCAL
// ==========================================================

float evaluarWakeSVM()
{
    static float normalizadas[
        LOCAL_NUM_FEATURES
    ];

    for (
        size_t i = 0;
        i < LOCAL_NUM_FEATURES;
        i++
    )
    {
        float escala =
            indi_scaler_scale[i];

        if (fabsf(escala) < 1e-12f)
        {
            escala = 1.0f;
        }

        normalizadas[i] =
            (
                wakeFeatures[i]
                -
                indi_scaler_mean[i]
            )
            /
            escala;
    }

    double decision =
        INDI_SVM_INTERCEPT;

    for (
        size_t sv = 0;
        sv < LOCAL_NUM_SV;
        sv++
    )
    {
        double distancia2 = 0.0;

        for (
            size_t f = 0;
            f < LOCAL_NUM_FEATURES;
            f++
        )
        {
            const double d =
                (double)normalizadas[f]
                -
                (double)
                indi_svm_support_vectors[sv][f];

            distancia2 += d * d;
        }

        const double kernel =
            exp(
                -
                (double)INDI_SVM_GAMMA
                *
                distancia2
            );

        decision +=
            (double)indi_svm_dual_coef[sv]
            *
            kernel;
    }

    return (float)decision;
}


// ==========================================================
// DETECTAR "INDI" LOCALMENTE
// ==========================================================

float detectarIndiLocal()
{
    metricWakeStartMs = millis();
    // ======================================================
    // INICIAR CAPTURA CONTINUA UNA SOLA VEZ
    // ======================================================

    if (!wakeMicContinuous)
    {
        if (!iniciarWakeContinuo())
        {
            return -999.0f;
        }

        // Todavia no hay suficiente audio.
        return -999.0f;
    }


    // ======================================================
    // SI EL BLOQUE TODAVIA ESTA GRABANDO,
    // NO PROCESAMOS NADA.
    // ======================================================

    if (
        wakeBlockRecording &&
        M5.Mic.isRecording()
    )
    {
        return -999.0f;
    }


    // ======================================================
    // BLOQUE TERMINADO:
    // PASARLO AL RING BUFFER
    // ======================================================

    if (wakeBlockRecording)
    {
        agregarBloqueWakeRing();

        wakeBlockRecording = false;
    }


    // ======================================================
    // ARRANCAR INMEDIATAMENTE EL SIGUIENTE BLOQUE
    //
    // Mientras este bloque se graba,
    // el CPU puede calcular MFCC + SVM.
    // ======================================================

    if (!iniciarBloqueWakeContinuo())
    {
        Serial.println(
            "ERROR: no se pudo continuar wake"
        );

        detenerWakeContinuo();

        return -999.0f;
    }


    // ======================================================
    // ESPERAR HASTA TENER 1.5 s COMPLETOS
    // ======================================================

    if (
        wakeRingFilled <
        LOCAL_WAKE_SAMPLES
    )
    {
        return -999.0f;
    }


    // ======================================================
    // TOMAR LOS ULTIMOS 1.5 s
    // ======================================================

    if (!copiarUltimaVentanaWake())
    {
        return -999.0f;
    }


    // ======================================================
    // PROCESAR WAKE
    // ======================================================

    const uint32_t inicioProceso =
        millis();


    quitarDCWake();

    calcularWakeMFCC();

    calcularWakeDelta(
        wakeMfcc,
        wakeDelta1
    );

    calcularWakeDelta(
        wakeDelta1,
        wakeDelta2
    );

    crearWakeFeatures();

    const float score =
        evaluarWakeSVM();


    const uint32_t tiempoProceso =
        millis() - inicioProceso;


    // ======================================================
    // DEBUG
    // ======================================================

    Serial.println();

    Serial.println(
        "========== WAKE CONTINUO =========="
    );

    Serial.print(
        "WAKE score: "
    );

    Serial.println(
        score,
        6
    );

    Serial.print(
        "Ring samples: "
    );

    Serial.print(
        wakeRingFilled
    );

    Serial.print(
        "/"
    );

    Serial.println(
        WAKE_SCAN_SAMPLES
    );

    Serial.print(
        "Procesamiento: "
    );

    Serial.print(
        tiempoProceso
    );

    Serial.println(
        " ms"
    );

    Serial.println(
        "===================================="
    );


    return score;
}


// ==========================================================
// REPRODUCIR "TE ESCUCHO" LOCALMENTE
// ==========================================================

void reproducirTeEscucho()
{
    Serial.println();
    Serial.println("TE ESCUCHO");

    M5.Mic.end();
    delay(30);

    if (!M5.Speaker.begin())
    {
        Serial.println("ERROR: no se pudo iniciar speaker para TE ESCUCHO");
        return;
    }

    M5.Speaker.setVolume(180);

    mouthLevel = 1;
    setRobotState(ROBOT_SPEAKING);

    bool ok = M5.Speaker.playWav(
        TE_ESCUCHO_AUDIO,
        TE_ESCUCHO_AUDIO_LEN
    );

    if (ok)
    {
        while (M5.Speaker.isPlaying())
        {
            M5.update();
            webSocket.loop();
            updateFaceAnimation();
            delay(1);
        }
    }

    M5.Speaker.end();
    mouthLevel = 0;
    setRobotState(ROBOT_AWAKE);
    delay(80);
}


// ==========================================================
// ENVIAR PREGUNTA
// ==========================================================

void enviarPregunta()
{
    metricTestId++;

    metricQueryStartMs = millis();

    metricLog(
        "QUERY_START_MS",
        metricQueryStartMs
    );
    Serial.println();
    Serial.println(
        "================================"
    );

    Serial.println(
        "HABLA - VAD ACTIVO"
    );

    Serial.println(
        "================================"
    );

    setRobotState(
        ROBOT_LISTENING
    );

    M5.Mic.end();

    queryWavBytesCurrent =
        capturarPreguntaVAD(
            queryBuffer,
            QUERY_SAMPLES
        );

    Serial.println();
    Serial.print(
        "GRABACION TERMINADA: "
    );

    Serial.print(
        queryWavBytesCurrent
    );

    Serial.println(
        " bytes"
    );

    if (!wsConnected)
    {
        Serial.println(
            "JETSON DESCONECTADO"
        );

        setRobotState(
            ROBOT_SLEEPING
        );

        return;
    }

    setRobotState(
        ROBOT_THINKING
    );

    liberarTTS();

    wsError = false;
    esperandoRespuesta = true;

    responseStartMs =
        millis();

    // ======================================================
    // AVISAR CUANTOS BYTES SE ENVIARAN
    // ======================================================

    String inicioJson =
        String(
            "{\"type\":\"query_start\",\"bytes\":"
        )
        +
        String(
            queryWavBytesCurrent
        )
        +
        "}";

    bool ok =
        webSocket.sendTXT(
            inicioJson
        );

    if (!ok)
    {
        Serial.println(
            "ERROR QUERY_START"
        );

        wsError = true;
        return;
    }

    // ======================================================
    // ENVIAR WAV EN BLOQUES PEQUENOS
    // ======================================================

    static constexpr size_t TX_CHUNK =
        8192;

    size_t enviados =
        0;

    while (
        enviados <
        queryWavBytesCurrent
    )
    {
        size_t cantidad =
            queryWavBytesCurrent
            -
            enviados;

        if (
            cantidad >
            TX_CHUNK
        )
        {
            cantidad =
                TX_CHUNK;
        }

        bool enviado =
            webSocket.sendBIN(
                queryBuffer
                +
                enviados,
                cantidad
            );

        if (!enviado)
        {
            Serial.println(
                "ERROR ENVIANDO BLOQUE DE AUDIO"
            );

            wsError = true;

            return;
        }

        enviados +=
            cantidad;

        Serial.print(
            "TX AUDIO: "
        );

        Serial.print(
            enviados
        );

        Serial.print(
            "/"
        );

        Serial.println(
            queryWavBytesCurrent
        );

        webSocket.loop();
        phoneServer.handleClient();

        delay(
            2
        );
    }

    // ======================================================
    // FIN
    // ======================================================

    ok =
        webSocket.sendTXT(
            "{\"type\":\"query_end\"}"
        );

    if (!ok)
    {
        Serial.println(
            "ERROR QUERY_END"
        );

        wsError = true;

        return;
    }

    metricQuerySentMs = millis();

    metricLog(
        "M5_AUDIO_PREP_SEND_MS",
        metricQuerySentMs - metricQueryStartMs
    );

    Serial.println(
        "PREGUNTA COMPLETA ENVIADA A JETSON"
    );
}
// ==========================================================
// REPRODUCIR RESPUESTA
// ==========================================================

void reproducirRespuesta()
{
    if (
        !audioReady ||
        ttsBuffer == nullptr
    )
    {
        return;
    }

    M5.Mic.end();

    delay(50);

    if (
        !M5.Speaker.begin()
    )
    {
        setRobotState(
            ROBOT_ERROR
        );

        return;
    }

    M5.Speaker.setVolume(
        180
    );

    mouthLevel = 1;

    setRobotState(
        ROBOT_SPEAKING
    );

    bool ok =
        M5.Speaker.playWav(
            ttsBuffer,
            ttsExpected
        );

    if (ok)
    {
        while (
            M5.Speaker.isPlaying()
        )
        {
            M5.update();

            webSocket.loop();

            updateFaceAnimation();

            delay(1);
        }
    }

    M5.Speaker.end();

    liberarTTS();

    setRobotState(
        ROBOT_SLEEPING
    );
}

// ==========================================================
// WEBSOCKET
// ==========================================================

void enviarWakeVerify()
{
    if (
        !wsConnected ||
        wakePcmBuffer == nullptr ||
        queryBuffer == nullptr
    )
    {
        Serial.println(
            "WAKE VERIFY: no se puede enviar"
        );

        wakeVerifyPending = false;

        return;
    }

    const size_t pcmBytes =
        LOCAL_WAKE_SAMPLES *
        sizeof(int16_t);

    const size_t wavBytes =
        44 +
        pcmBytes;

    // Reutilizamos queryBuffer mientras el robot duerme.
    crearCabeceraWav(
        queryBuffer,
        pcmBytes
    );

    memcpy(
        queryBuffer + 44,
        wakePcmBuffer,
        pcmBytes
    );

    StaticJsonDocument<96> doc;

    doc["type"] =
        "wake_verify_start";

    doc["bytes"] =
        (uint32_t)wavBytes;

    char jsonInicio[96];

    serializeJson(
        doc,
        jsonInicio,
        sizeof(jsonInicio)
    );

    if (
        !webSocket.sendTXT(
            jsonInicio
        )
    )
    {
        Serial.println(
            "WAKE VERIFY: error start"
        );

        wakeVerifyPending = false;

        return;
    }

    delay(5);

    const size_t BLOQUE =
        4096;

    size_t enviados =
        0;

    while (
        enviados <
        wavBytes
    )
    {
        size_t cantidad =
            wavBytes -
            enviados;

        if (
            cantidad >
            BLOQUE
        )
        {
            cantidad =
                BLOQUE;
        }

        bool ok =
            webSocket.sendBIN(
                queryBuffer +
                enviados,
                cantidad
            );

        if (!ok)
        {
            Serial.println(
                "WAKE VERIFY: error enviando audio"
            );

            wakeVerifyPending =
                false;

            return;
        }

        enviados +=
            cantidad;

        Serial.print(
            "WAKE VERIFY TX: "
        );

        Serial.print(
            enviados
        );

        Serial.print(
            "/"
        );

        Serial.println(
            wavBytes
        );

        webSocket.loop();

        delay(3);
    }

    if (
        !webSocket.sendTXT(
            "{\"type\":\"wake_verify_end\"}"
        )
    )
    {
        Serial.println(
            "WAKE VERIFY: error end"
        );

        wakeVerifyPending =
            false;

        return;
    }

    wakeVerifyPending =
        true;

    wakeVerifyStartMs =
        millis();

    Serial.println();
    Serial.println(
        "WAKE VERIFY: esperando a Vosk..."
    );
}

void webSocketEvent(
    WStype_t type,
    uint8_t* payload,
    size_t length
)
{
    if (
        type ==
        WStype_CONNECTED
    )
    {
        wsConnected = true;

        Serial.println();
        Serial.println(
            "WEBSOCKET CONECTADO"
        );

        if (!esperandoRespuesta)
        {
            setRobotState(
                ROBOT_SLEEPING
            );
        }
        else
        {
            Serial.println(
                "CONEXION RECUPERADA: SIGO ESPERANDO RESPUESTA"
            );
        }

        return;
    }

    if (
        type ==
        WStype_DISCONNECTED
    )
    {
        wsConnected = false;

        Serial.println();
        Serial.println(
            "WEBSOCKET DESCONECTADO"
        );

        // No cambiamos el estado del robot.
        // WebSocketsClient intentara reconectar automaticamente.
        return;
    }

    if (
        type ==
        WStype_TEXT
    )
    {
        DynamicJsonDocument doc(
            4096
        );

        DeserializationError error =
            deserializeJson(
                doc,
                payload,
                length
            );

        if (error)
        {
            return;
        }

        const char* tipo =
            doc["type"] | "";

        // ==================================================
        // RESULTADO FINAL DEL WAKE
        // ==================================================

        if (
            strcmp(
                tipo,
                "wake_verify_result"
            ) == 0
        )
        {
            wakeVerifyAccepted =
                doc["accepted"] |
                false;

            wakeVerifyPending =
                false;

            wakeVerifyResultPending =
                true;

            wakeVerifyStartMs =
                0;

            Serial.println();
            Serial.println(
                "WAKE VERIFY RESULTADO"
            );

            Serial.print(
                "Transcript: "
            );

            Serial.println(
                doc["transcript"] | ""
            );

            Serial.print(
                "Aceptado: "
            );

            Serial.println(
                wakeVerifyAccepted
                ? "SI"
                : "NO"
            );

            return;
        }

        if (
            strcmp(
                tipo,
                "transcript"
            ) == 0
        )
        {
            Serial.println();

            metricResponseReceivedMs = millis();

            metricLog(
                "REMOTE_ROUNDTRIP_MS",
                metricResponseReceivedMs - metricQuerySentMs
            );

            metricLog(
                "TOTAL_UNTIL_RESPONSE_MS",
                metricResponseReceivedMs - metricQueryStartMs
            );

            Serial.print(
                "YO DIJE: "
            );

            const char* transcriptText =
                doc["text"] | "";

            Serial.println(
                transcriptText
            );

            procesarComandoLocal(
                String(transcriptText)
            );

            return;
        }

        if (
            strcmp(
                tipo,
                "command"
            ) == 0
        )
        {
            Serial.println();

            Serial.print(
                "COMANDO INMEDIATO: "
            );

            Serial.println(
                doc["transcript"] | ""
            );

            if (
                doc.containsKey(
                    "command"
                )
                &&
                doc["command"].is<JsonObject>()
            )
            {
                bool commandOk =
                    procesarComandoJson(
                        doc["command"].as<JsonObject>()
                    );

                Serial.print(
                    "JSON COMMAND: "
                );

                Serial.println(
                    commandOk
                    ?
                    "ACEPTADO"
                    :
                    "RECHAZADO"
                );
            }

            return;
        }
        if (
            strcmp(
                tipo,
                "result"
            ) == 0
        )
        {
            // Jetson ya respondio.
            // Desde aqui el timeout mide INACTIVIDAD,
            // no el tiempo total de procesamiento.
            responseStartMs = millis();

            Serial.println();

            Serial.print(
                "ROBOT: "
            );

            Serial.println(
                doc["speech"] | ""
            );

            // Si Jetson incluye un comando estructurado,
            // se valida y ejecuta SOLO con keyword "mini indi".
            if (
                doc.containsKey(
                    "command"
                )
                &&
                doc["command"].is<JsonObject>()
            )
            {
                bool commandOk =
                    procesarComandoJson(
                        doc["command"].as<JsonObject>()
                    );

                Serial.print(
                    "JSON COMMAND: "
                );

                Serial.println(
                    commandOk
                    ?
                    "ACEPTADO"
                    :
                    "RECHAZADO"
                );
            }

            size_t audioBytes =
                doc["audio_bytes"] | 0;

            liberarTTS();

            if (
                audioBytes == 0
            )
            {
                wsError = true;
                return;
            }

            ttsBuffer =
                (uint8_t*)
                heap_caps_malloc(
                    audioBytes,
                    MALLOC_CAP_SPIRAM |
                    MALLOC_CAP_8BIT
                );

            if (
                !ttsBuffer
            )
            {
                Serial.println(
                    "ERROR: NO HAY PSRAM PARA TTS"
                );

                wsError = true;

                return;
            }

            ttsExpected =
                audioBytes;

            ttsReceived =
                0;

            return;
        }

        if (
            strcmp(
                tipo,
                "audio_end"
            ) == 0
        )
        {
            if (
                ttsExpected > 0 &&
                ttsReceived ==
                ttsExpected
            )
            {
                audioReady =
                    true;

                responseStartMs =
                    millis();

                Serial.print(
                    "TTS COMPLETO: "
                );

                Serial.print(
                    ttsReceived
                );

                Serial.print(
                    "/"
                );

                Serial.println(
                    ttsExpected
                );
            }
            else
            {
                Serial.println(
                    "ERROR: AUDIO INCOMPLETO"
                );

                wsError =
                    true;
            }

            return;
        }

        if (
            strcmp(
                tipo,
                "error"
            ) == 0
        )
        {
            Serial.print(
                "JETSON ERROR: "
            );

            Serial.println(
                doc["error"] | ""
            );

            wsError =
                true;

            return;
        }
    }

    if (
        type ==
        WStype_BIN
    )
    {
        if (
            ttsBuffer ==
            nullptr
        )
        {
            return;
        }

        if (
            ttsReceived >=
            ttsExpected
        )
        {
            return;
        }

        size_t restante =
            ttsExpected -
            ttsReceived;

        size_t copiar =
            length;

        if (
            copiar >
            restante
        )
        {
            copiar =
                restante;
        }

        memcpy(
            ttsBuffer +
            ttsReceived,
            payload,
            copiar
        );

        ttsReceived +=
            copiar;

        // Llegaron bytes de TTS:
        // renovar watchdog de respuesta.
        if (esperandoRespuesta)
        {
            responseStartMs = millis();
        }

        // ==================================================
        // ACK TTS
        // ==================================================
        // Confirmar a Jetson que este bloque ya fue copiado
        // al buffer del CoreS3.
        // Jetson no enviara el siguiente hasta recibir esto.
        // ==================================================

        StaticJsonDocument<96> ackDoc;

        ackDoc["type"] =
            "tts_ack";

        ackDoc["received"] =
            (uint32_t)ttsReceived;

        char ackJson[96];

        serializeJson(
            ackDoc,
            ackJson,
            sizeof(ackJson)
        );

        webSocket.sendTXT(
            ackJson
        );
    }
}

// ==========================================================
// WIFI
// ==========================================================

void conectarWiFi()
{
    WiFi.mode(
        WIFI_STA
    );

    WiFi.setSleep(
        false
    );

    esp_wifi_set_ps(
        WIFI_PS_NONE
    );

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    Serial.println(
        "Conectando a MINI_INDI_LAN..."
    );

    while (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println(
        "WiFi conectado"
    );

    Serial.print(
        "IP CoreS3: "
    );

    Serial.println(
        WiFi.localIP()
    );
}

// ==========================================================
// SETUP
// ==========================================================


bool confirmarWakeMiniIndi(float score)
{
    const uint32_t now =
        millis();

    // ======================================================
    // MODO TEMPORAL ESTRICTO
    // ======================================================
    //
    // No acumulamos evidencia.
    // No usamos hits consecutivos.
    //
    // Un ruido no puede "sumar puntos" hasta despertar.
    // ======================================================

    static constexpr float WAKE_TEMP_THRESHOLD = LOCAL_WAKE_THRESHOLD;


    if (
        wakeLastAcceptedMs != 0
        &&
        now - wakeLastAcceptedMs <
            WAKE_COOLDOWN_MS
    )
    {
        return false;
    }


    if (score <= -900.0f)
    {
        return false;
    }


    Serial.print(
        "WAKE CHECK score="
    );

    Serial.println(
        score,
        6
    );


    if (
        score <
        WAKE_TEMP_THRESHOLD
    )
    {
        return false;
    }


    Serial.print(
        "MINI INDI FUERTE ACEPTADO: "
    );

    Serial.println(
        score,
        6
    );


    wakeEvidence = 0.0f;
    wakeEvidenceStartMs = 0;

    wakeCandidateHits = 0;
    wakeFirstHitMs = 0;

    wakeLastAcceptedMs =
        now;

    metricWakeDetectedMs = millis();

    metricLog(
        "M5_WAKE_LATENCY_MS",
        metricWakeDetectedMs - metricWakeStartMs
    );

    Serial.print("METRIC,");
    Serial.print(metricTestId);
    Serial.print(",WAKE_SCORE,");
    Serial.println(score, 6);

    return true;
}
void setup()
{
    auto cfg =
        M5.config();

    cfg.internal_mic = true;
    cfg.internal_spk = true;

    M5.begin(
        cfg
    );

    Serial.begin(
        115200
    );

    delay(400);

    canvas.setColorDepth(
        16
    );

    canvas.createSprite(
        M5.Display.width(),
        M5.Display.height()
    );

    initColors();

    // Mismo ajuste usado en la prueba local que detecto correctamente.
    auto micCfg =
        M5.Mic.config();

    micCfg.noise_filter_level = 0;

    M5.Mic.config(
        micCfg
    );

    randomSeed(
        esp_random()
    );

    wakePcmBuffer =
        (int16_t*)
        heap_caps_malloc(
            LOCAL_WAKE_SAMPLES * sizeof(int16_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );
    wakeRawBuffer =
        (int16_t*)
        heap_caps_malloc(
            WAKE_SCAN_SAMPLES * sizeof(int16_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    wakeCaptureBuffer =
        (int16_t*)
        heap_caps_malloc(
            WAKE_STREAM_BLOCK * sizeof(int16_t),
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    queryBuffer =
        (uint8_t*)
        heap_caps_malloc(
            QUERY_WAV_BYTES,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (
        !wakePcmBuffer ||
        !wakeRawBuffer ||
        !wakeCaptureBuffer ||
        !queryBuffer
    )
    {
        Serial.println(
            "ERROR AL RESERVAR PSRAM"
        );

        while (true)
        {
            delay(1000);
        }
    }

    Serial.print(
        "Wake local samples: "
    );

    Serial.println(
        LOCAL_WAKE_SAMPLES
    );

    Serial.print(
        "Wake model support vectors: "
    );

    Serial.println(
        LOCAL_NUM_SV
    );

    Serial.print(
        "Query WAV bytes: "
    );

    Serial.println(
        QUERY_WAV_BYTES
    );

    drawSleepingFace();

    conectarWiFi();

    iniciarEspNow();

    iniciarPanelTelefono();

    webSocket.begin(
        WS_HOST,
        WS_PORT,
        WS_PATH
    );

    webSocket.onEvent(
        webSocketEvent
    );

    webSocket.setReconnectInterval(3000);

    webSocket.enableHeartbeat(20000, 10000, 3);

    Serial.println();
    Serial.println(
        "================================"
    );
    Serial.println(
        "MINI INDI READY"
    );
    Serial.println(
        "ACTIVACION: SOLO POR VOZ"
    );
    Serial.println(
        "PALABRA: MINI INDI"
    );
    Serial.println(
        "================================"
    );
}

// ==========================================================
// LOOP
// ==========================================================

void loop()
{
    M5.update();

    phoneServer.handleClient();

    static uint32_t lastEmergencyOverlayMs = 0;

    if (
        systemEmergencyMode
        &&
        millis() - lastEmergencyOverlayMs >= 500
    )
    {
        lastEmergencyOverlayMs = millis();

        drawEmergencyOverlay();
    }

    static unsigned long lastEspNowHeartbeatMs = 0;

    if (millis() - lastEspNowHeartbeatMs >= 500)
    {
        lastEspNowHeartbeatMs = millis();
        enviarHeartbeatEspNow();
    }

    webSocket.loop();

    updateFaceAnimation();

    // ======================================================
    // RECUPERAR WIFI SI SE PIERDE
    // ======================================================

    static unsigned long lastWiFiRetryMs = 0;

    if (
        WiFi.status() != WL_CONNECTED &&
        millis() - lastWiFiRetryMs > 2000
    )
    {
        lastWiFiRetryMs = millis();

        Serial.println(
            "WiFi perdido: intentando reconectar..."
        );

        WiFi.disconnect();
        WiFi.begin(
            WIFI_SSID,
            WIFI_PASSWORD
        );
    }

    // ======================================================
    // ALERTA ESP-NOW PENDIENTE
    // ======================================================

    if (vibrationPending)
    {
        vibrationPending = false;

        mostrarAlertaVibracion(
            vibrationLevel
        );
    }

    // ======================================================
    // RESULTADO DE VERIFICACION "MINI INDI"
    // ======================================================

    if (
        robotState ==
        ROBOT_SLEEPING &&
        wakeVerifyResultPending
    )
    {
        wakeVerifyResultPending =
            false;

        if (wakeVerifyAccepted)
        {
            Serial.println();
            Serial.println(
                "*******************************"
            );
            Serial.println(
                "*** JETSON CONFIRMO MINI INDI ***"
            );
            Serial.println(
                "*******************************"
            );

            wakeVerifyAccepted =
                false;

            setRobotState(
                ROBOT_AWAKE
            );

            delay(180);

            reproducirTeEscucho();

            enviarPregunta();

            return;
        }

        Serial.println();
        Serial.println(
            "WAKE RECHAZADO POR VOSK"
        );
        Serial.println(
            "ROBOT SIGUE DORMIDO"
        );

        wakeVerifyAccepted =
            false;

        wakeVerifyPending =
            false;

        setRobotState(
            ROBOT_SLEEPING
        );

        return;
    }


    // ======================================================
    // TIMEOUT DEL VERIFICADOR
    // ======================================================

    if (
        robotState ==
        ROBOT_SLEEPING &&
        wakeVerifyPending &&
        wakeVerifyStartMs != 0 &&
        millis() -
        wakeVerifyStartMs >
        WAKE_VERIFY_TIMEOUT_MS
    )
    {
        Serial.println(
            "WAKE VERIFY TIMEOUT"
        );

        wakeVerifyPending =
            false;

        wakeVerifyAccepted =
            false;

        wakeVerifyResultPending =
            false;

        wakeVerifyStartMs =
            0;

        return;
    }

    // ======================================================
    // DORMIDO - DETECCION LOCAL DE "INDI"
    // ======================================================

    if (
        robotState ==
        ROBOT_SLEEPING &&
        !esperandoRespuesta &&
        !wakeVerifyPending &&
        !wakeVerifyResultPending &&
        wsConnected
    )
    {
        const float wakeScore =
            detectarIndiLocal();

        if (
            confirmarWakeMiniIndi(
                wakeScore
            )
        )
        {
            detenerWakeContinuo();

            Serial.println();
            Serial.println(
                "================================"
            );
            Serial.println(
                "MINI INDI DETECTADO LOCALMENTE"
            );
            Serial.println(
                "SIN VERIFICACION VOSK DE WAKE"
            );
            Serial.println(
                "================================"
            );

            setRobotState(
                ROBOT_AWAKE
            );

            delay(180);

            reproducirTeEscucho();

            enviarPregunta();

            return;
        }

        // No fue "Indi": sigue dormido y escucha la siguiente ventana.
        delay(20);

        return;
    }

    // ======================================================
    // ESPERANDO RESPUESTA
    // ======================================================

    if (
        esperandoRespuesta
    )
    {
        if (
            responseStartMs != 0
            &&
            millis() - responseStartMs >
            RESPONSE_TIMEOUT_MS
        )
        {
            Serial.println(
                "TIMEOUT ESPERANDO JETSON"
            );

            esperandoRespuesta =
                false;

            responseStartMs =
                0;

            liberarTTS();

            setRobotState(
                ROBOT_SLEEPING
            );

            return;
        }

        if (
            wsError
        )
        {
            esperandoRespuesta =
                false;

            liberarTTS();

            setRobotState(
                ROBOT_SLEEPING
            );

            return;
        }

        if (
            audioReady
        )
        {
            esperandoRespuesta =
                false;

            reproducirRespuesta();

            return;
        }
    }

    delay(5);
}




