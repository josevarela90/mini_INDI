#include <Arduino.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============================================================
// MINI INDI - AtomS3 Lite
// BLE Bluefruit Connect + ESP-NOW + LED + UART hacia OpenRB-150
// BLE y ESP-NOW pueden controlar las mismas funciones del robot.
// ============================================================

// ------------------------------------------------------------
// LED RGB integrado AtomS3 Lite
// ------------------------------------------------------------

#define LED_PIN 35

void setLED(uint8_t r, uint8_t g, uint8_t b)
{
  rgbLedWrite(LED_PIN, r, g, b);
}


// ------------------------------------------------------------
// UART AtomS3 Lite <-> OpenRB-150
//
// GPIO1 = TX
// GPIO2 = RX
// ------------------------------------------------------------

#define ROBOT_TX_PIN 2
#define ROBOT_RX_PIN 1

HardwareSerial RobotSerial(1);

const uint32_t ROBOT_BAUD = 115200;


// ------------------------------------------------------------
// Nordic UART Service
// Compatible con Bluefruit Connect
// ------------------------------------------------------------

#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_RX   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_TX   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *txCharacteristic;

bool deviceConnected = false;


// ============================================================
// ESP-NOW - protocolo CitizenPacket
// ============================================================

static constexpr uint8_t WIFI_CHANNEL = 6;
static constexpr uint32_t CITIZEN_MAGIC = 0x4D494E49;
static constexpr uint8_t CITIZEN_VERSION = 1;

// Si el movimiento se inició mediante ESP-NOW, el control remoto
// debe continuar enviando MSG_HEARTBEAT (o cualquier paquete válido).
// Si se pierde el enlace durante este tiempo, las orugas se detienen.
const unsigned long ESPNOW_CONTROL_TIMEOUT_MS = 1000;

enum CitizenMsgType : uint8_t
{
  MSG_LIGHT_SET = 1,
  MSG_VIBRATION_ALERT = 2,
  MSG_HEARTBEAT = 3,
  MSG_DRIVE = 4,
  MSG_HEAD = 5,
  MSG_EMERGENCY_STOP = 6,
  MSG_LIGHT_COLOR = 7,
  MSG_DANCE = 8
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

// Códigos para MSG_DRIVE en packet.value.
enum DriveAction : uint8_t
{
  DRIVE_STOP = 0,
  DRIVE_FORWARD = 1,
  DRIVE_BACKWARD = 2,
  DRIVE_LEFT = 3,
  DRIVE_RIGHT = 4,
  DRIVE_SLOW = 5,
  DRIVE_MEDIUM = 6,
  DRIVE_FAST = 7,
  DRIVE_DANCE = 8
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

struct EspNowRxItem
{
  uint8_t mac[6];
  CitizenPacket packet;
};

// IMPORTANTE PARA ARDUINO .ino:
// Declaracion manual para evitar que el preprocesador de Arduino
// genere este prototipo antes de conocer EspNowRxItem.
void processEspNowPacket(const EspNowRxItem &item);

QueueHandle_t espNowRxQueue = nullptr;
unsigned long lastEspNowRx = 0;
uint32_t lastEspNowSeq = 0;

// Estado normal de la luz. Durante BAILE se usan colores temporales.
uint8_t normalLightR = 255;
uint8_t normalLightG = 255;
uint8_t normalLightB = 255;
bool normalLightOn = true;

enum ControlSource : uint8_t
{
  SOURCE_NONE = 0,
  SOURCE_BLE = 1,
  SOURCE_ESPNOW = 2
};

ControlSource motionSource = SOURCE_NONE;
ControlSource danceSource = SOURCE_NONE;

// Prototipos usados por el procesamiento ESP-NOW.
void startDance(ControlSource source);
void cancelDance(bool notifyPhone);


// ------------------------------------------------------------
// Parámetros del robot
// ------------------------------------------------------------

const float TRACK_WIDTH = 0.118;  // m

float driveSpeed = 0.08;          // velocidad inicial: 8 cm/s

// Tercer motor (inclinación): comando relativo de 0 a 50.
// La OpenRB convierte este valor al recorrido físico 220°..270°.
const float TILT_INPUT_MIN = 0.0;
const float TILT_INPUT_MAX = 50.0;

bool buttonUp    = false;
bool buttonDown  = false;
bool buttonLeft  = false;
bool buttonRight = false;

bool robotMoving = false;

unsigned long lastMotionSend = 0;

const unsigned long HEARTBEAT_MS = 200;


// ============================================================
// Rutina especial BAILE
// ============================================================

// 16 pasos x 500 ms = 8 segundos.
// El límite solicitado es máximo 10 segundos.
const unsigned long DANCE_STEP_MS = 500;
const uint8_t DANCE_STEP_COUNT = 16;

bool danceActive = false;
unsigned long danceStartTime = 0;
unsigned long lastDanceStepTime = 0;
uint8_t danceStep = 0;

struct DanceStep
{
  float v;
  float w;
  float tilt;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// tilt usa siempre el rango lógico 0..50, equivalente a 220°..270°.
const DanceStep DANCE[DANCE_STEP_COUNT] =
{
  { 0.060,  0.00,  5.0, 255,   0,   0 },
  { 0.000,  1.20, 15.0, 255, 120,   0 },
  { 0.000, -1.20, 35.0, 255, 255,   0 },
  { 0.050,  0.80, 50.0,   0, 255,   0 },
  {-0.050, -0.80, 35.0,   0, 255, 255 },
  { 0.000,  1.50, 15.0,   0,   0, 255 },
  { 0.000, -1.50,  0.0, 120,   0, 255 },
  { 0.060,  0.00, 25.0, 255,   0, 255 },
  {-0.060,  0.00, 45.0, 255, 255, 255 },
  { 0.000,  1.30, 50.0, 255,   0,   0 },
  { 0.000, -1.30, 30.0,   0, 255,   0 },
  { 0.050, -0.80, 10.0,   0,   0, 255 },
  {-0.050,  0.80,  0.0, 255, 255,   0 },
  { 0.000,  1.50, 20.0,   0, 255, 255 },
  { 0.000, -1.50, 40.0, 255,   0, 255 },
  { 0.000,  0.00, 25.0, 255, 255, 255 }
};


// ============================================================
// Comunicación con OpenRB
// ============================================================

void sendRobotCommand(float v, float w)
{
  RobotSerial.print("M;");
  RobotSerial.print(v, 3);
  RobotSerial.print(";");
  RobotSerial.println(w, 3);

  Serial.print("ENVIADO OpenRB: [M;");
  Serial.print(v, 3);
  Serial.print(";");
  Serial.print(w, 3);
  Serial.println("]");

  robotMoving =
      (fabs(v) > 0.001 || fabs(w) > 0.001);

  lastMotionSend = millis();
}


void sendTiltCommand(float inputAngle)
{
  inputAngle = constrain(
      inputAngle,
      TILT_INPUT_MIN,
      TILT_INPUT_MAX
  );

  RobotSerial.print("A;");
  RobotSerial.println(inputAngle, 1);

  Serial.print("ENVIADO OpenRB: [A;");
  Serial.print(inputAngle, 1);
  Serial.println("]");
}


void stopRobot()
{
  buttonUp    = false;
  buttonDown  = false;
  buttonLeft  = false;
  buttonRight = false;

  sendRobotCommand(0.0, 0.0);
  motionSource = SOURCE_NONE;
}


// ------------------------------------------------------------
// Calcula v y omega según botones pulsados
// ------------------------------------------------------------

void updateRobotMotion()
{
  float v = 0.0;
  float w = 0.0;

  // Movimiento lineal
  if (buttonUp && !buttonDown)
  {
    v = driveSpeed;
  }
  else if (buttonDown && !buttonUp)
  {
    v = -driveSpeed;
  }

  // Para giro puro:
  // cada rueda tendrá aproximadamente driveSpeed
  float turnSpeed =
      (2.0 * driveSpeed) / TRACK_WIDTH;

  if (buttonLeft && !buttonRight)
  {
    w = turnSpeed;
  }
  else if (buttonRight && !buttonLeft)
  {
    w = -turnSpeed;
  }

  sendRobotCommand(v, w);
}


// ============================================================
// CRC de paquetes Bluefruit Controller
// ============================================================

bool validCRC(const uint8_t *data, size_t len)
{
  if (len < 2)
    return false;

  uint8_t sum = 0;

  for (size_t i = 0; i < len - 1; i++)
  {
    sum += data[i];
  }

  sum = ~sum;

  return sum == data[len - 1];
}


// ============================================================
// Respuesta BLE al iPhone
// ============================================================

void sendBLEMessage(const String &message)
{
  if (!deviceConnected)
    return;

  txCharacteristic->setValue(message);
  txCharacteristic->notify();
}


// ============================================================
// Luz normal + colores ESP-NOW
// ============================================================

void applyNormalLight()
{
  if (normalLightOn)
    setLED(normalLightR, normalLightG, normalLightB);
  else
    setLED(0, 0, 0);
}

void setNormalLightColor(uint8_t r, uint8_t g, uint8_t b)
{
  normalLightR = r;
  normalLightG = g;
  normalLightB = b;
  normalLightOn = true;

  // Durante BAILE solo guardamos el color normal; la secuencia
  // mantiene sus propios colores hasta finalizar.
  if (!danceActive)
    applyNormalLight();
}

bool setNormalLightColorCode(uint8_t code)
{
  switch (code)
  {
    case LIGHT_RED:    setNormalLightColor(255,   0,   0); return true;
    case LIGHT_GREEN:  setNormalLightColor(  0, 255,   0); return true;
    case LIGHT_BLUE:   setNormalLightColor(  0,   0, 255); return true;
    case LIGHT_YELLOW: setNormalLightColor(255, 255,   0); return true;
    case LIGHT_PURPLE: setNormalLightColor(255,   0, 255); return true;
    case LIGHT_CYAN:   setNormalLightColor(  0, 255, 255); return true;
    case LIGHT_WHITE:  setNormalLightColor(255, 255, 255); return true;
    case LIGHT_ORANGE: setNormalLightColor(255, 128,   0); return true;
    default: return false;
  }
}


// ============================================================
// ESP-NOW - recepción en cola
//
// El callback WiFi solo copia el paquete. Toda acción sobre BLE,
// UART, motores y LEDs se ejecuta luego desde loop().
// ============================================================

void queueEspNowPacket(
    const uint8_t *mac,
    const uint8_t *incomingData,
    int len
)
{
  if (len != sizeof(CitizenPacket) || espNowRxQueue == nullptr)
    return;

  EspNowRxItem item;
  memcpy(item.mac, mac, 6);
  memcpy(&item.packet, incomingData, sizeof(CitizenPacket));

  // Si la cola está llena se descarta el paquete más nuevo.
  xQueueSend(espNowRxQueue, &item, 0);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(
    const esp_now_recv_info_t *info,
    const uint8_t *incomingData,
    int len
)
{
  queueEspNowPacket(info->src_addr, incomingData, len);
}
#else
void onEspNowRecv(
    const uint8_t *mac,
    const uint8_t *incomingData,
    int len
)
{
  queueEspNowPacket(mac, incomingData, len);
}
#endif


void setEspNowDriveDirection(uint8_t action, float requestedSpeed)
{
  // packet.level puede definir opcionalmente la velocidad lineal
  // para FWD/BACK/LEFT/RIGHT. Si vale 0, se conserva driveSpeed.
  if (requestedSpeed >= 0.01 && requestedSpeed <= 0.12)
  {
    driveSpeed = requestedSpeed;
  }

  buttonUp = false;
  buttonDown = false;
  buttonLeft = false;
  buttonRight = false;

  switch (action)
  {
    case DRIVE_FORWARD:  buttonUp = true; break;
    case DRIVE_BACKWARD: buttonDown = true; break;
    case DRIVE_LEFT:     buttonLeft = true; break;
    case DRIVE_RIGHT:    buttonRight = true; break;
    default: return;
  }

  motionSource = SOURCE_ESPNOW;
  updateRobotMotion();
}


void processEspNowPacket(const EspNowRxItem &item)
{
  const CitizenPacket &packet = item.packet;

  if (packet.magic != CITIZEN_MAGIC)
  {
    Serial.println("ESP-NOW: MAGIC incorrecto");
    return;
  }

  if (packet.version != CITIZEN_VERSION)
  {
    Serial.println("ESP-NOW: VERSION incorrecta");
    return;
  }

  lastEspNowRx = millis();
  lastEspNowSeq = packet.seq;

  Serial.printf(
      "ESP-NOW RX %02X:%02X:%02X:%02X:%02X:%02X type=%u value=%u level=%.2f seq=%lu\n",
      item.mac[0], item.mac[1], item.mac[2],
      item.mac[3], item.mac[4], item.mac[5],
      packet.type, packet.value, packet.level,
      (unsigned long)packet.seq
  );

  switch (packet.type)
  {
    case MSG_HEARTBEAT:
      // lastEspNowRx ya fue actualizado.
      return;

    case MSG_EMERGENCY_STOP:
      if (danceActive)
        cancelDance(false);
      else
        stopRobot();

      sendBLEMessage("ESP-NOW: EMERGENCY STOP");
      return;

    case MSG_LIGHT_SET:
      normalLightOn = (packet.value == 1);
      if (!danceActive)
        applyNormalLight();
      return;

    case MSG_LIGHT_COLOR:
      if (!setNormalLightColorCode(packet.value))
        Serial.println("ESP-NOW: codigo de color invalido");
      return;

    case MSG_HEAD:
      // packet.level usa exactamente el mismo rango lógico 0..50.
      if (packet.level < TILT_INPUT_MIN ||
          packet.level > TILT_INPUT_MAX)
      {
        Serial.println("ESP-NOW: HEAD fuera de rango 0..50");
        return;
      }

      if (danceActive)
        cancelDance(false);

      sendTiltCommand(packet.level);
      return;

    case MSG_DANCE:
      startDance(SOURCE_ESPNOW);
      return;

    case MSG_DRIVE:
      break;

    case MSG_VIBRATION_ALERT:
      // Reservado por el protocolo original. No cambia el robot.
      Serial.println("ESP-NOW: VIBRATION_ALERT recibido");
      return;

    default:
      Serial.println("ESP-NOW: tipo de mensaje no soportado");
      return;
  }

  // ----------------------------------------------------------
  // MSG_DRIVE
  // ----------------------------------------------------------

  if (packet.value == DRIVE_DANCE)
  {
    startDance(SOURCE_ESPNOW);
    return;
  }

  if (danceActive)
    cancelDance(false);

  switch (packet.value)
  {
    case DRIVE_STOP:
      stopRobot();
      return;

    case DRIVE_FORWARD:
    case DRIVE_BACKWARD:
    case DRIVE_LEFT:
    case DRIVE_RIGHT:
      setEspNowDriveDirection(
          packet.value,
          fabs(packet.level)
      );
      return;

    case DRIVE_SLOW:
      driveSpeed = 0.04;
      motionSource = SOURCE_ESPNOW;
      updateRobotMotion();
      return;

    case DRIVE_MEDIUM:
      driveSpeed = 0.08;
      motionSource = SOURCE_ESPNOW;
      updateRobotMotion();
      return;

    case DRIVE_FAST:
      driveSpeed = 0.12;
      motionSource = SOURCE_ESPNOW;
      updateRobotMotion();
      return;

    default:
      Serial.println("ESP-NOW: DRIVE value invalido");
      return;
  }
}


void processEspNowQueue()
{
  if (espNowRxQueue == nullptr)
    return;

  EspNowRxItem item;

  while (xQueueReceive(espNowRxQueue, &item, 0) == pdTRUE)
  {
    processEspNowPacket(item);
  }
}


void initEspNow()
{
  WiFi.mode(WIFI_STA);
  delay(100);

  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_wifi_set_promiscuous(true);
  esp_err_t channelResult =
      esp_wifi_set_channel(
          WIFI_CHANNEL,
          WIFI_SECOND_CHAN_NONE
      );
  esp_wifi_set_promiscuous(false);

  Serial.print("ESP-NOW canal config: ");
  Serial.println(channelResult);

  uint8_t primaryChannel = 0;
  wifi_second_chan_t secondChannel;
  esp_wifi_get_channel(&primaryChannel, &secondChannel);

  Serial.print("ESP-NOW canal real: ");
  Serial.println(primaryChannel);

  Serial.print("MAC AtomS3: ");
  Serial.println(WiFi.macAddress());

  espNowRxQueue = xQueueCreate(8, sizeof(EspNowRxItem));

  if (espNowRxQueue == nullptr)
  {
    Serial.println("ERROR: no se pudo crear cola ESP-NOW");
    return;
  }

  esp_err_t initResult = esp_now_init();

  Serial.print("ESP-NOW init: ");
  Serial.println(initResult);

  if (initResult != ESP_OK)
  {
    Serial.println("ERROR iniciando ESP-NOW");
    return;
  }

  esp_err_t cbResult =
      esp_now_register_recv_cb(onEspNowRecv);

  Serial.print("ESP-NOW callback: ");
  Serial.println(cbResult);

  Serial.println("ESP-NOW listo");
}


// ============================================================
// BAILE: control no bloqueante
// ============================================================

void finishDance(bool notifyPhone)
{
  danceActive = false;
  danceStep = 0;
  danceSource = SOURCE_NONE;

  // Seguridad: detener orugas y dejar tilt centrado.
  stopRobot();
  sendTiltCommand(25.0);

  // Recuperar la luz normal elegida por BLE o ESP-NOW.
  applyNormalLight();

  if (notifyPhone)
    sendBLEMessage("BAILE terminado");
}


void cancelDance(bool notifyPhone)
{
  if (!danceActive)
    return;

  finishDance(false);

  if (notifyPhone)
    sendBLEMessage("BAILE cancelado");
}


void applyDanceStep(uint8_t step)
{
  if (step >= DANCE_STEP_COUNT)
    return;

  const DanceStep &s = DANCE[step];

  // Movimiento rítmico de las orugas.
  sendRobotCommand(s.v, s.w);

  // Tilt limitado al rango lógico 0..50.
  sendTiltCommand(s.tilt);

  // Cambio de color sincronizado con cada paso.
  setLED(s.r, s.g, s.b);
}


void startDance(ControlSource source)
{
  // Reinicia la rutina si BAILE se envía nuevamente.
  if (danceActive)
    cancelDance(false);

  buttonUp    = false;
  buttonDown  = false;
  buttonLeft  = false;
  buttonRight = false;

  motionSource = source;
  danceSource = source;
  danceActive = true;
  danceStartTime = millis();
  lastDanceStepTime = danceStartTime;
  danceStep = 0;

  sendBLEMessage(
      source == SOURCE_ESPNOW
      ? "BAILE ESP-NOW iniciado - 8 s"
      : "BAILE iniciado - 8 s"
  );

  // Ejecutar el primer paso inmediatamente.
  applyDanceStep(danceStep);
  danceStep++;
}


void updateDance()
{
  if (!danceActive)
    return;

  unsigned long now = millis();

  // Fin duro a los 8 segundos. Nunca supera 10 segundos.
  if (now - danceStartTime >=
      DANCE_STEP_MS * DANCE_STEP_COUNT)
  {
    finishDance(true);
    return;
  }

  if (
      danceStep < DANCE_STEP_COUNT &&
      now - lastDanceStepTime >= DANCE_STEP_MS
     )
  {
    lastDanceStepTime = now;
    applyDanceStep(danceStep);
    danceStep++;
  }
}


// ============================================================
// Procesar Control Pad de Bluefruit
// ============================================================

void processButton(uint8_t button, bool pressed)
{
  Serial.print("Boton: ");
  Serial.print((char)button);

  Serial.print(" estado: ");
  Serial.println(pressed ? "PRESSED" : "RELEASED");

  // Cualquier pulsación manual interrumpe BAILE inmediatamente.
  if (danceActive && pressed)
  {
    cancelDance(false);
  }

  // Los botones Bluefruit toman el control de movimiento.
  motionSource = SOURCE_BLE;

  switch (button)
  {
    // --------------------------------------------------------
    // Botón 1 = STOP
    // --------------------------------------------------------

    case '1':

      if (pressed)
      {
        stopRobot();
        sendBLEMessage("STOP");
      }

      return;


    // --------------------------------------------------------
    // Botón 2 = lento
    // --------------------------------------------------------

    case '2':

      if (pressed)
      {
        driveSpeed = 0.04;

        sendBLEMessage(
            "Speed: 0.04 m/s"
        );

        updateRobotMotion();
      }

      return;


    // --------------------------------------------------------
    // Botón 3 = medio
    // --------------------------------------------------------

    case '3':

      if (pressed)
      {
        driveSpeed = 0.08;

        sendBLEMessage(
            "Speed: 0.08 m/s"
        );

        updateRobotMotion();
      }

      return;


    // --------------------------------------------------------
    // Botón 4 = rápido
    // --------------------------------------------------------

    case '4':

      if (pressed)
      {
        driveSpeed = 0.12;

        sendBLEMessage(
            "Speed: 0.12 m/s"
        );

        updateRobotMotion();
      }

      return;


    // --------------------------------------------------------
    // UP
    // --------------------------------------------------------

    case '5':

      buttonUp = pressed;
      break;


    // --------------------------------------------------------
    // DOWN
    // --------------------------------------------------------

    case '6':

      buttonDown = pressed;
      break;


    // --------------------------------------------------------
    // LEFT
    // --------------------------------------------------------

    case '7':

      buttonLeft = pressed;
      break;


    // --------------------------------------------------------
    // RIGHT
    // --------------------------------------------------------

    case '8':

      buttonRight = pressed;
      break;
  }

  updateRobotMotion();
}


// ============================================================
// Comandos escritos desde UART Terminal
// ============================================================

void processTextCommand(String command)
{
  command.trim();
  command.toUpperCase();

  Serial.print("Texto BLE: ");
  Serial.println(command);


  // ----------------------------------------------------------
  // RUTINA ESPECIAL
  // ----------------------------------------------------------

  if (command == "BAILE")
  {
    startDance(SOURCE_BLE);
    return;
  }

  // Cualquier otro comando escrito devuelve el control manual.
  if (danceActive)
  {
    cancelDance(false);
  }


  // ----------------------------------------------------------
  // TERCER MOTOR - INCLINACIÓN POR ÁNGULO
  //
  // Desde Bluefruit UART Terminal:
  //   ANGLE;25
  //   TILT;25
  // Rango permitido: 0..50
  //
  // La AtomS3 reenvía a la OpenRB:
  //   A;25.0
  // ----------------------------------------------------------

  if (
      command.startsWith("ANGLE;") ||
      command.startsWith("TILT;")
     )
  {
    int separator = command.indexOf(';');

    String angleString =
        command.substring(separator + 1);

    angleString.trim();

    if (angleString.length() == 0)
    {
      sendBLEMessage("ERROR: use ANGLE;0..50");
      return;
    }

    float angle = angleString.toFloat();

    if (
        angle < TILT_INPUT_MIN ||
        angle > TILT_INPUT_MAX
       )
    {
      sendBLEMessage(
          "ERROR: valor 0..50"
      );
      return;
    }

    sendTiltCommand(angle);

    sendBLEMessage(
        "Tilt enviado: " +
        String(angle, 1) +
        " (0..50)"
    );

    return;
  }


  // ----------------------------------------------------------
  // COLORES
  // ----------------------------------------------------------

  if (command == "RED")
  {
    setNormalLightColor(255, 0, 0);
  }

  else if (command == "GREEN")
  {
    setNormalLightColor(0, 255, 0);
  }

  else if (command == "BLUE")
  {
    setNormalLightColor(0, 0, 255);
  }

  else if (command == "WHITE")
  {
    setNormalLightColor(255, 255, 255);
  }

  else if (command == "YELLOW")
  {
    setNormalLightColor(255, 255, 0);
  }

  else if (command == "PURPLE")
  {
    setNormalLightColor(255, 0, 255);
  }

  else if (command == "CYAN")
  {
    setNormalLightColor(0, 255, 255);
  }

  else if (command == "OFF")
  {
    normalLightOn = false;
    applyNormalLight();
  }


  // ----------------------------------------------------------
  // MOVIMIENTO
  // ----------------------------------------------------------

  else if (command == "FWD")
  {
    motionSource = SOURCE_BLE;
    buttonUp    = true;
    buttonDown  = false;
    buttonLeft  = false;
    buttonRight = false;

    updateRobotMotion();
  }

  else if (command == "BACK")
  {
    motionSource = SOURCE_BLE;
    buttonUp    = false;
    buttonDown  = true;
    buttonLeft  = false;
    buttonRight = false;

    updateRobotMotion();
  }

  else if (command == "LEFT")
  {
    motionSource = SOURCE_BLE;
    buttonUp    = false;
    buttonDown  = false;
    buttonLeft  = true;
    buttonRight = false;

    updateRobotMotion();
  }

  else if (command == "RIGHT")
  {
    motionSource = SOURCE_BLE;
    buttonUp    = false;
    buttonDown  = false;
    buttonLeft  = false;
    buttonRight = true;

    updateRobotMotion();
  }

  else if (command == "STOP")
  {
    stopRobot();
  }


  // ----------------------------------------------------------
  // VELOCIDAD
  // ----------------------------------------------------------

  else if (command == "SLOW")
  {
    motionSource = SOURCE_BLE;
    driveSpeed = 0.04;
    updateRobotMotion();
  }

  else if (command == "MEDIUM")
  {
    motionSource = SOURCE_BLE;
    driveSpeed = 0.08;
    updateRobotMotion();
  }

  else if (command == "FAST")
  {
    motionSource = SOURCE_BLE;
    driveSpeed = 0.12;
    updateRobotMotion();
  }


  sendBLEMessage(
      "OK: " + command
  );
}


// ============================================================
// BLE Server
// ============================================================

class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *server)
  {
    deviceConnected = true;

    Serial.println(
        "iPhone conectado"
    );

    // Verde inicialmente
    setNormalLightColor(0, 255, 0);

    sendBLEMessage(
        "MiniINDI conectado"
    );
  }


  void onDisconnect(BLEServer *server)
  {
    deviceConnected = false;

    Serial.println(
        "iPhone desconectado"
    );

    // SEGURIDAD:
    // Una desconexión BLE solo cancela acciones originadas por BLE.
    // El control ESP-NOW continúa independiente.
    if (danceActive && danceSource == SOURCE_BLE)
      cancelDance(false);
    else if (!danceActive && motionSource == SOURCE_BLE)
      stopRobot();

    if (!danceActive)
      applyNormalLight();

    delay(100);

    BLEDevice::startAdvertising();
  }
};


// ============================================================
// Recepción BLE
// ============================================================

class RXCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(
      BLECharacteristic *characteristic
  )
  {
    String value =
        characteristic->getValue();

    size_t len =
        characteristic->getLength();

    if (len == 0)
      return;

    const uint8_t *data =
        characteristic->getData();


    // ========================================================
    // BLUEFRUIT CONTROLLER: BUTTON
    //
    // ! B button state checksum
    //
    // tamaño = 5 bytes
    // ========================================================

    if (
        len == 5 &&
        data[0] == '!' &&
        data[1] == 'B' &&
        validCRC(data, len)
       )
    {
      uint8_t button = data[2];

      bool pressed =
          data[3] == '1';

      processButton(
          button,
          pressed
      );

      return;
    }


    // ========================================================
    // BLUEFRUIT COLOR PICKER
    //
    // ! C R G B checksum
    //
    // tamaño = 6 bytes
    // ========================================================

    if (
        len == 6 &&
        data[0] == '!' &&
        data[1] == 'C' &&
        validCRC(data, len)
       )
    {
      uint8_t r = data[2];
      uint8_t g = data[3];
      uint8_t b = data[4];

      setNormalLightColor(r, g, b);

      Serial.print("RGB: ");
      Serial.print(r);
      Serial.print(",");
      Serial.print(g);
      Serial.print(",");
      Serial.println(b);

      return;
    }


    // ========================================================
    // UART Terminal tradicional
    // ========================================================

    processTextCommand(value);
  }
};


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(500);


  // ----------------------------------------------------------
  // UART hacia OpenRB
  // ----------------------------------------------------------

  RobotSerial.begin(
      ROBOT_BAUD,
      SERIAL_8N1,
      ROBOT_RX_PIN,
      ROBOT_TX_PIN
  );


  // Azul al arrancar
  setLED(0, 0, 255);


  // ----------------------------------------------------------
  // ESP-NOW
  // ----------------------------------------------------------

  initEspNow();


  // ----------------------------------------------------------
  // BLE
  // ----------------------------------------------------------

  BLEDevice::init(
      "MiniINDI"
  );

  BLEServer *server =
      BLEDevice::createServer();

  server->setCallbacks(
      new ServerCallbacks()
  );


  BLEService *service =
      server->createService(
          SERVICE_UUID
      );


  // Atom -> iPhone
  txCharacteristic =
      service->createCharacteristic(
          CHARACTERISTIC_TX,
          BLECharacteristic::PROPERTY_NOTIFY
      );

  txCharacteristic->addDescriptor(
      new BLE2902()
  );


  // iPhone -> Atom
  BLECharacteristic *rxCharacteristic =
      service->createCharacteristic(
          CHARACTERISTIC_RX,
          BLECharacteristic::PROPERTY_WRITE
      );

  rxCharacteristic->setCallbacks(
      new RXCallbacks()
  );


  service->start();


  BLEAdvertising *advertising =
      BLEDevice::getAdvertising();

  advertising->addServiceUUID(
      SERVICE_UUID
  );

  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();


  Serial.println();
  Serial.println(
      "======================"
  );

  Serial.println(
      "MiniINDI AtomS3 Lite"
  );

  Serial.println(
      "BLE + ESP-NOW + UART listo"
  );

  Serial.println(
      "======================"
  );


  // Amarillo = sistema listo / esperando control
  normalLightR = 255;
  normalLightG = 255;
  normalLightB = 0;
  normalLightOn = true;
  applyNormalLight();
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  // Procesar paquetes ESP-NOW fuera del callback WiFi.
  processEspNowQueue();

  // ----------------------------------------------------------
  // Watchdog del control ESP-NOW
  // ----------------------------------------------------------

  if (
      !danceActive &&
      motionSource == SOURCE_ESPNOW &&
      robotMoving &&
      millis() - lastEspNowRx > ESPNOW_CONTROL_TIMEOUT_MS
     )
  {
    Serial.println("ESP-NOW TIMEOUT -> STOP");
    stopRobot();
    sendBLEMessage("ESP-NOW timeout -> STOP");
  }

  // ----------------------------------------------------------
  // Heartbeat hacia OpenRB
  //
  // Mientras el robot esté moviéndose,
  // repetimos la orden cada 200 ms.
  // ----------------------------------------------------------

  // Durante BAILE, la secuencia genera sus propios comandos cada
  // 500 ms. El heartbeat manual se desactiva para no interferir.
  if (danceActive)
  {
    updateDance();
  }
  else if (
      robotMoving &&
      millis() - lastMotionSend >
      HEARTBEAT_MS
     )
  {
    updateRobotMotion();
  }


  // ----------------------------------------------------------
  // Recibir mensajes de la OpenRB
  // y retransmitirlos al iPhone
  // ----------------------------------------------------------

  static String openRBMessage;

  while (RobotSerial.available())
  {
    char c = RobotSerial.read();

    if (c == '\n')
    {
      openRBMessage.trim();

      if (openRBMessage.length() > 0)
      {
        Serial.print(
            "OpenRB => "
        );

        Serial.println(
            openRBMessage
        );

        sendBLEMessage(
            openRBMessage
        );
      }

      openRBMessage = "";
    }
    else
    {
      openRBMessage += c;
    }
  }


  delay(5);
}