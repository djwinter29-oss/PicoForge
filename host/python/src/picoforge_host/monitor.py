"""Reconnectable background HID monitor for the local dashboard."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import threading
import time
from typing import Callable

from .device import PicoForgeHid


def _empty_snapshot() -> dict[str, object]:
    return {
        "connected": False,
        "error": None,
        "hardware": None,
        "runtime": None,
        "updated_at": None,
    }


class DeviceMonitor:
    """Keep one HID handle open and publish an immutable status snapshot."""

    def __init__(
        self,
        serial_number: str | None = None,
        device_path: str | None = None,
        client_factory: Callable[[], PicoForgeHid] | None = None,
    ) -> None:
        self._client_factory = client_factory or (lambda: PicoForgeHid(serial_number, device_path))
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._snapshot = _empty_snapshot()
        self._thread = threading.Thread(target=self._run, name="picoforge-hid-monitor", daemon=True)
        self._thread.start()

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            return deepcopy(self._snapshot)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _run(self) -> None:
        while not self._stop.is_set():
            client = None
            try:
                client = self._client_factory()
                hardware = client.read_info()
                with self._lock:
                    self._snapshot = {
                        "connected": True,
                        "error": None,
                        "hardware": hardware,
                        "runtime": None,
                        "updated_at": None,
                    }
                while not self._stop.is_set():
                    runtime = client.read_status(timeout_ms=250)
                    if runtime is not None:
                        with self._lock:
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                runtime=runtime,
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
            except (OSError, RuntimeError, ValueError) as error:
                with self._lock:
                    self._snapshot = {**_empty_snapshot(), "error": str(error)}
            finally:
                if client is not None:
                    try:
                        client.close()
                    except OSError:
                        pass
            self._stop.wait(1.0)