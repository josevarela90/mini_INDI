from enum import Enum


class SystemMode(str, Enum):
    NORMAL = "normal"
    EMERGENCY = "emergency"


class ModeManager:
    def __init__(self):
        self.mode = SystemMode.NORMAL

    def emergency_on(self):
        self.mode = SystemMode.EMERGENCY

    def emergency_off(self):
        self.mode = SystemMode.NORMAL

    def is_emergency(self):
        return self.mode == SystemMode.EMERGENCY

    def current(self):
        return self.mode.value


mode_manager = ModeManager()
