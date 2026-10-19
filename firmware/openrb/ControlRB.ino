#include <Dynamixel2Arduino.h>

// ============================================================
// MINI INDI - OpenRB-150
// BLE y ESP-NOW son recibidos por AtomS3 y se traducen a M;v;w y A;0..50.
// BAILE tambien es orquestado por AtomS3 con esos mismos comandos.
// El tilt conserva el límite físico 220°..270°.
//
// Serial1 = DYNAMIXEL
// Serial2 = AtomS3 Lite
// Serial  = USB debug
// ============================================================

#define DXL_SERIAL   Serial1
#define ROBOT_SERIAL Serial2
#define DEBUG_SERIAL Serial

const int DXL_DIR_PIN = -1;

Dynamixel2Arduino dxl(
    DXL_SERIAL,
    DXL_DIR_PIN
);


// ============================================================
// Motores
// ============================================================

const uint8_t MOTOR_RIGHT = 2;
const uint8_t MOTOR_LEFT  = 3;
const uint8_t MOTOR_TILT  = 1;   // tercer motor: inclinación de la CoreS3


// ============================================================
// Sentido físico de los motores
//
// Como están enfrentados, normalmente uno debe invertirse.
//
// Si al probar el robot ambos no avanzan correctamente,
// cambia uno de estos signos.
// ============================================================

const int RIGHT_DIRECTION = -1;
const int LEFT_DIRECTION  = +1;


// ============================================================
// Geometría
// ============================================================

// diámetro rueda
const float WHEEL_DIAMETER = 0.033;

// distancia entre ruedas
const float TRACK_WIDTH = 0.118;

// circunferencia
const float WHEEL_CIRCUMFERENCE =
    PI * WHEEL_DIAMETER;


// ============================================================
// Límites
// ============================================================

const float MAX_RPM = 80.0;

const float MAX_LINEAR_SPEED = 0.12;

const float MAX_ANGULAR_SPEED = 2.5;

// Tercer motor: el usuario envía un valor relativo de 0 a 50.
// Ese valor se convierte linealmente al recorrido físico 220°..270°.
const float TILT_INPUT_MIN = 0.0;
const float TILT_INPUT_MAX = 50.0;

const float TILT_MIN_ANGLE = 220.0;
const float TILT_MAX_ANGLE = 270.0;


// ============================================================
// Seguridad
// ============================================================

// Atom envía heartbeat cada 200 ms.
// Si pasan 700 ms sin recibir comando,
// la OpenRB detiene los motores.

const unsigned long COMMAND_TIMEOUT = 700;

unsigned long lastCommandTime = 0;

bool robotIsMoving = false;

bool motorsReady = false;
bool tiltMotorReady = false;


// ============================================================
// Velocidad de ruedas
// ============================================================

void setWheelRPM(
    float rightRPM,
    float leftRPM
)
{
  // ----------------------------------------------------------
  // Saturación proporcional
  //
  // Evita deformar excesivamente la trayectoria.
  // ----------------------------------------------------------

  float maxValue =
      max(
          fabs(rightRPM),
          fabs(leftRPM)
      );

  if (maxValue > MAX_RPM)
  {
    float scale =
        MAX_RPM / maxValue;

    rightRPM *= scale;
    leftRPM  *= scale;
  }


  // ----------------------------------------------------------
  // Aplicar sentido físico
  // ----------------------------------------------------------

  float realRightRPM =
      RIGHT_DIRECTION *
      rightRPM;

  float realLeftRPM =
      LEFT_DIRECTION *
      leftRPM;


  dxl.setGoalVelocity(
      MOTOR_RIGHT,
      realRightRPM,
      UNIT_RPM
  );

  dxl.setGoalVelocity(
      MOTOR_LEFT,
      realLeftRPM,
      UNIT_RPM
  );


  DEBUG_SERIAL.print(
      "R: "
  );

  DEBUG_SERIAL.print(
      rightRPM,
      2
  );

  DEBUG_SERIAL.print(
      " rpm  L: "
  );

  DEBUG_SERIAL.print(
      leftRPM,
      2
  );

  DEBUG_SERIAL.println(
      " rpm"
  );
}


// ============================================================
// Cinemática diferencial
// ============================================================

void drive(
    float linearVelocity,
    float angularVelocity
)
{
  if (!motorsReady)
    return;


  // ----------------------------------------------------------
  // Límites de seguridad
  // ----------------------------------------------------------

  linearVelocity =
      constrain(
          linearVelocity,
          -MAX_LINEAR_SPEED,
          MAX_LINEAR_SPEED
      );

  angularVelocity =
      constrain(
          angularVelocity,
          -MAX_ANGULAR_SPEED,
          MAX_ANGULAR_SPEED
      );


  // ----------------------------------------------------------
  // Cinemática diferencial
  //
  // Vr = V + w L/2
  // Vl = V - w L/2
  // ----------------------------------------------------------

  float rightLinear =
      linearVelocity +
      angularVelocity *
      TRACK_WIDTH / 2.0;

  float leftLinear =
      linearVelocity -
      angularVelocity *
      TRACK_WIDTH / 2.0;


  // ----------------------------------------------------------
  // m/s -> RPM
  // ----------------------------------------------------------

  float rightRPM =
      (rightLinear /
       WHEEL_CIRCUMFERENCE)
      * 60.0;

  float leftRPM =
      (leftLinear /
       WHEEL_CIRCUMFERENCE)
      * 60.0;


  setWheelRPM(
      rightRPM,
      leftRPM
  );


  robotIsMoving =
      (
        fabs(linearVelocity) > 0.001 ||
        fabs(angularVelocity) > 0.001
      );
}


// ============================================================
// Tercer motor - control de inclinación por ángulo
// ============================================================

void setTiltInput(float inputAngle)
{
  if (!tiltMotorReady)
  {
    DEBUG_SERIAL.println(
        "ERROR: motor TILT no disponible"
    );

    ROBOT_SERIAL.println(
        "ERROR;TILT;NOT_READY"
    );

    return;
  }

  // Seguridad: solo se aceptan valores de usuario entre 0 y 50.
  if (
      inputAngle < TILT_INPUT_MIN ||
      inputAngle > TILT_INPUT_MAX
     )
  {
    DEBUG_SERIAL.println(
        "ERROR: TILT fuera de rango 0..50"
    );

    ROBOT_SERIAL.println(
        "ERROR;A;RANGE"
    );

    return;
  }

  // Conversión lineal:
  //   0  -> 220°
  //   25 -> 245°
  //   50 -> 270°
  float physicalAngle =
      TILT_MIN_ANGLE +
      (inputAngle - TILT_INPUT_MIN) *
      (TILT_MAX_ANGLE - TILT_MIN_ANGLE) /
      (TILT_INPUT_MAX - TILT_INPUT_MIN);

  // Segunda barrera de seguridad sobre el ángulo físico.
  physicalAngle = constrain(
      physicalAngle,
      TILT_MIN_ANGLE,
      TILT_MAX_ANGLE
  );

  dxl.setGoalPosition(
      MOTOR_TILT,
      physicalAngle,
      UNIT_DEGREE
  );

  DEBUG_SERIAL.print(
      "TILT comando: "
  );

  DEBUG_SERIAL.print(
      inputAngle,
      1
  );

  DEBUG_SERIAL.print(
      " -> objetivo físico: "
  );

  DEBUG_SERIAL.print(
      physicalAngle,
      1
  );

  DEBUG_SERIAL.println(
      " deg"
  );

  ROBOT_SERIAL.print(
      "OK;A;"
  );

  ROBOT_SERIAL.print(
      inputAngle,
      1
  );

  ROBOT_SERIAL.print(
      ";DEG;"
  );

  ROBOT_SERIAL.println(
      physicalAngle,
      1
  );
}


// ============================================================
// STOP
// ============================================================

void stopRobot()
{
  setWheelRPM(
      0.0,
      0.0
  );

  robotIsMoving = false;
}


// ============================================================
// Procesar comando recibido de AtomS3
//
// Formato:
//
// M;v;w
//
// ejemplo:
//
// M;0.080;0.000
// ============================================================

void processCommand(
    String command
)
{
  // Quitar espacios y posibles caracteres de fin de linea
  command.trim();

  // Mostrar exactamente lo recibido desde la AtomS3
  DEBUG_SERIAL.print("RECIBIDO: [");
  DEBUG_SERIAL.print(command);
  DEBUG_SERIAL.println("]");

  // ----------------------------------------------------------
  // Tercer motor: A;valor
  //
  // Rango de entrada: 0..50
  // Conversión física: 220°..270°
  //
  // Ejemplo:
  // A;25  -> 245°
  // ----------------------------------------------------------

  if (command.startsWith("A;"))
  {
    String angleString =
        command.substring(2);

    angleString.trim();

    if (angleString.length() == 0)
    {
      DEBUG_SERIAL.println(
          "ERROR: formato incorrecto. Use A;0..50"
      );

      ROBOT_SERIAL.println(
          "ERROR;A;FORMAT"
      );

      return;
    }

    float angle =
        angleString.toFloat();

    if (
        angle < TILT_INPUT_MIN ||
        angle > TILT_INPUT_MAX
       )
    {
      DEBUG_SERIAL.println(
          "ERROR: valor TILT fuera de rango 0..50"
      );

      ROBOT_SERIAL.println(
          "ERROR;A;RANGE"
      );

      return;
    }

    setTiltInput(angle);
    return;
  }


  // El comando de movimiento debe comenzar con M;
  if (!command.startsWith("M;"))
  {
    DEBUG_SERIAL.println(
        "ERROR: use M;v;w o A;0..50"
    );

    ROBOT_SERIAL.println(
        "ERROR;COMMAND"
    );

    return;
  }

  // Buscar los dos separadores de campos
  // Ejemplo: M;0.080;0.000
  int sep1 = command.indexOf(';');
  int sep2 = command.indexOf(';', sep1 + 1);

  if (
      sep1 < 0 ||
      sep2 < 0
     )
  {
    DEBUG_SERIAL.println("ERROR: formato incorrecto. Use M;v;w");
    return;
  }

  // Extraer velocidad lineal y angular como texto
  String vString =
      command.substring(
          sep1 + 1,
          sep2
      );

  String wString =
      command.substring(
          sep2 + 1
      );

  // Diagnostico: ver exactamente los campos recibidos
  DEBUG_SERIAL.print("vString = [");
  DEBUG_SERIAL.print(vString);
  DEBUG_SERIAL.println("]");

  DEBUG_SERIAL.print("wString = [");
  DEBUG_SERIAL.print(wString);
  DEBUG_SERIAL.println("]");

  // Convertir texto decimal a float
  // Ejemplos:
  // "0.080"  ->  0.080
  // "-0.080" -> -0.080
  float v = vString.toFloat();
  float w = wString.toFloat();

  lastCommandTime = millis();

  DEBUG_SERIAL.print("Comando: v=");
  DEBUG_SERIAL.print(v, 3);

  DEBUG_SERIAL.print(" w=");
  DEBUG_SERIAL.println(w, 3);

  // Ejecutar movimiento
  drive(v, w);

  // Confirmacion hacia AtomS3
  // Se mantiene el mismo separador ; para evitar ambiguedades
  ROBOT_SERIAL.print("OK;M;");
  ROBOT_SERIAL.print(v, 3);
  ROBOT_SERIAL.print(";");
  ROBOT_SERIAL.println(w, 3);
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  DEBUG_SERIAL.begin(
      115200
  );

  ROBOT_SERIAL.begin(
      115200
  );

  delay(500);


  // ----------------------------------------------------------
  // Inicializar DYNAMIXEL
  // ----------------------------------------------------------

  dxl.begin(
      57600
  );

  dxl.setPortProtocolVersion(
      2.0
  );


  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println(
      "========================="
  );

  DEBUG_SERIAL.println(
      "Mini INDI - OpenRB-150"
  );

  DEBUG_SERIAL.println(
      "========================="
  );


  // ----------------------------------------------------------
  // Detectar motores
  // ----------------------------------------------------------

  bool rightOK =
      dxl.ping(
          MOTOR_RIGHT
      );

  bool leftOK =
      dxl.ping(
          MOTOR_LEFT
      );

  bool tiltOK =
      dxl.ping(
          MOTOR_TILT
      );


  if (rightOK)
  {
    DEBUG_SERIAL.println(
        "Motor DERECHO ID 2: OK"
    );
  }
  else
  {
    DEBUG_SERIAL.println(
        "Motor DERECHO ID 2: ERROR"
    );
  }


  if (leftOK)
  {
    DEBUG_SERIAL.println(
        "Motor IZQUIERDO ID 3: OK"
    );
  }
  else
  {
    DEBUG_SERIAL.println(
        "Motor IZQUIERDO ID 3: ERROR"
    );
  }


  if (tiltOK)
  {
    DEBUG_SERIAL.println(
        "Motor TILT ID 1: OK"
    );
  }
  else
  {
    DEBUG_SERIAL.println(
        "Motor TILT ID 1: ERROR"
    );
  }


  motorsReady =
      rightOK &&
      leftOK;

  tiltMotorReady =
      tiltOK;


  // ----------------------------------------------------------
  // Solo configurar si ambos motores están presentes
  // ----------------------------------------------------------

  if (motorsReady)
  {
    // Torque OFF antes de cambiar modo
    dxl.torqueOff(
        MOTOR_RIGHT
    );

    dxl.torqueOff(
        MOTOR_LEFT
    );


    // Velocity Mode
    dxl.setOperatingMode(
        MOTOR_RIGHT,
        OP_VELOCITY
    );

    dxl.setOperatingMode(
        MOTOR_LEFT,
        OP_VELOCITY
    );


    // Torque ON
    dxl.torqueOn(
        MOTOR_RIGHT
    );

    dxl.torqueOn(
        MOTOR_LEFT
    );


    // arrancar detenido
    stopRobot();


    DEBUG_SERIAL.println(
        "Motores preparados"
    );

  }

  else
  {
    DEBUG_SERIAL.println(
        "ERROR: motores no disponibles"
    );

    ROBOT_SERIAL.println(
        "ERROR,DXL"
    );
  }


  // ----------------------------------------------------------
  // Configurar tercer motor en Position Mode
  //
  // Se lee primero la posición actual y se usa como primer
  // objetivo para evitar un salto grande al habilitar torque.
  // ----------------------------------------------------------

  if (tiltMotorReady)
  {
    float initialTiltAngle =
        dxl.getPresentPosition(
            MOTOR_TILT,
            UNIT_DEGREE
        );

    dxl.torqueOff(
        MOTOR_TILT
    );

    dxl.setOperatingMode(
        MOTOR_TILT,
        OP_POSITION
    );

    if (
        initialTiltAngle >= TILT_MIN_ANGLE &&
        initialTiltAngle <= TILT_MAX_ANGLE
       )
    {
      dxl.setGoalPosition(
          MOTOR_TILT,
          initialTiltAngle,
          UNIT_DEGREE
      );
    }

    dxl.torqueOn(
        MOTOR_TILT
    );

    DEBUG_SERIAL.print(
        "Motor TILT preparado. Posicion inicial: "
    );

    DEBUG_SERIAL.print(
        initialTiltAngle,
        1
    );

    DEBUG_SERIAL.println(
        " deg"
    );
  }


  // Estado general hacia la AtomS3
  ROBOT_SERIAL.print(
      "READY;DRIVE="
  );

  ROBOT_SERIAL.print(
      motorsReady ? 1 : 0
  );

  ROBOT_SERIAL.print(
      ";TILT="
  );

  ROBOT_SERIAL.println(
      tiltMotorReady ? 1 : 0
  );


  lastCommandTime =
      millis();
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
  // ----------------------------------------------------------
  // Recibir datos desde AtomS3
  // ----------------------------------------------------------

  static String receivedLine;

  while (ROBOT_SERIAL.available())
  {
    char c =
        ROBOT_SERIAL.read();


    if (c == '\n')
    {
      receivedLine.trim();

      if (
          receivedLine.length() > 0
         )
      {
        processCommand(
            receivedLine
        );
      }

      receivedLine = "";
    }

    else if (c != '\r')
    {
      // Evitar desbordamiento accidental
      if (
          receivedLine.length()
          < 60
         )
      {
        receivedLine += c;
      }
    }
  }


  // ----------------------------------------------------------
  // WATCHDOG DE SEGURIDAD
  //
  // Si se pierde AtomS3, cable o Bluetooth:
  // STOP automático.
  // ----------------------------------------------------------

  if (
      robotIsMoving &&
      millis() - lastCommandTime >
      COMMAND_TIMEOUT
     )
  {
    stopRobot();

    DEBUG_SERIAL.println(
        "TIMEOUT -> STOP"
    );

    ROBOT_SERIAL.println(
        "TIMEOUT,STOP"
    );
  }
}