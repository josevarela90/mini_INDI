# ==========================================================
# VALIDATOR DEL ROBOT
# ==========================================================

# Estos límites son temporales.
# Se calibrarán con el robot físico.

HEAD_PAN_MIN = -90
HEAD_PAN_MAX = 90

HEAD_TILT_MIN = -45
HEAD_TILT_MAX = 45

BASE_MAX_SPEED = 0.70
BASE_MAX_DURATION_MS = 3000


def validar_accion(action):

    if not isinstance(action, dict):
        return False, "La accion debe ser un objeto"

    if action.get("type") != "action":
        return False, "type invalido"

    device = action.get("device")
    accion = action.get("action")

    # ======================================================
    # CABEZA HORIZONTAL
    # ======================================================

    if device == "head_pan":

        if accion in ("stop", "home"):
            return True, "OK"

        if accion != "move":
            return False, "Accion head_pan invalida"

        value = action.get("value")

        if not isinstance(
            value,
            (int, float)
        ):
            return False, "Angulo invalido"

        if not (
            HEAD_PAN_MIN <=
            value <=
            HEAD_PAN_MAX
        ):
            return False, "Angulo fuera del rango"

        return True, "OK"

    # ======================================================
    # CABEZA VERTICAL
    # ======================================================

    if device == "head_tilt":

        if accion in ("stop", "home"):
            return True, "OK"

        if accion != "move":
            return False, "Accion head_tilt invalida"

        value = action.get("value")

        if not isinstance(
            value,
            (int, float)
        ):
            return False, "Angulo invalido"

        if not (
            HEAD_TILT_MIN <=
            value <=
            HEAD_TILT_MAX
        ):
            return False, "Angulo fuera del rango"

        return True, "OK"

    # ======================================================
    # BASE CON ORUGAS
    # ======================================================

    if device == "base":

        acciones_validas = {
            "forward",
            "backward",
            "turn_left",
            "turn_right",
            "stop"
        }

        if accion not in acciones_validas:
            return False, "Movimiento invalido"

        if accion == "stop":
            return True, "OK"

        speed = action.get(
            "speed",
            0.35
        )

        duration_ms = action.get(
            "duration_ms",
            800
        )

        if not isinstance(
            speed,
            (int, float)
        ):
            return False, "Velocidad invalida"

        if not isinstance(
            duration_ms,
            (int, float)
        ):
            return False, "Duracion invalida"

        if (
            speed < 0 or
            speed > BASE_MAX_SPEED
        ):
            return False, "Velocidad fuera de rango"

        if (
            duration_ms < 50 or
            duration_ms >
            BASE_MAX_DURATION_MS
        ):
            return False, "Duracion fuera de rango"

        return True, "OK"

    return (
        False,
        f"Dispositivo no permitido: {device}"
    )
