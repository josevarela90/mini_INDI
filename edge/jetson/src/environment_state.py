from dataclasses import dataclass, asdict
from threading import Lock
from typing import Optional
import time


@dataclass
class EnvironmentState:
    node: str = "ATOM_ENV"
    temperature: Optional[float] = None
    humidity: Optional[float] = None
    emergency: bool = False
    last_update: float = 0.0


class EnvironmentManager:
    def __init__(self):
        self._lock = Lock()
        self._state = EnvironmentState()

    def update(
        self,
        temperature=None,
        humidity=None,
        emergency=False,
        node="ATOM_ENV",
    ):
        with self._lock:

            self._state.node = node

            if temperature is not None:
                self._state.temperature = float(
                    temperature
                )

            if humidity is not None:
                self._state.humidity = float(
                    humidity
                )

            self._state.emergency = bool(
                emergency
            )

            self._state.last_update = (
                time.time()
            )

    def set_emergency(
        self,
        active
    ):
        with self._lock:

            self._state.emergency = bool(
                active
            )

            self._state.last_update = (
                time.time()
            )

    def snapshot(self):
        with self._lock:

            return asdict(
                self._state
            )


environment = EnvironmentManager()
