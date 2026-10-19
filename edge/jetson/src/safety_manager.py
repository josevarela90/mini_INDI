class SafetyManager:
    def __init__(self):
        self.emergency = False

    def activate_emergency(self):
        self.emergency = True

    def deactivate_emergency(self):
        self.emergency = False

    def command_allowed(self, command: str):
        command = command.strip().lower()

        if not self.emergency:
            return True, "normal"

        blocked = {
            "alarm_off",
            "light_off",
            "emergency_off",
        }

        if command in blocked:
            return (
                False,
                "Accion bloqueada durante una emergencia"
            )

        return True, "emergency"


safety = SafetyManager()
