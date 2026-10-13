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
        "board_status": None,
        "memory": None,
        "msc": None,
        "self_test": None,
        "updated_at": None,
    }


def _board_status_from_runtime(runtime: dict[str, object]) -> dict[str, object]:
    return {
        "temperature_valid": runtime["temperature_valid"],
        "temperature_centidegrees": runtime["temperature_centidegrees"],
        "led_available": runtime["led_available"],
        "led_on": runtime["led_on"],
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
        self._device_lock = threading.RLock()
        self._active_client: PicoForgeHid | None = None
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

    def set_led(self, on: bool) -> dict[str, object]:
        if not isinstance(on, bool):
            raise ValueError("LED state must be boolean")
        board_status = self.snapshot().get("board_status")
        if not isinstance(board_status, dict) or board_status.get("led_available") is not True:
            raise RuntimeError("PicoForge HID is disconnected or the board LED is unavailable")
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            self._active_client.set_led(on)
        with self._lock:
            board_status = dict(self._snapshot["board_status"])
            board_status["led_on"] = on
            self._snapshot["board_status"] = board_status
        return board_status

    def set_msc(self, enable: bool) -> dict[str, object]:
        if not isinstance(enable, bool):
            raise ValueError("MSC enable state must be boolean")
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            self._active_client.set_msc(enable)
        with self._lock:
            previous = self._snapshot.get("msc") or {}
            status = {"enabled": previous.get("enabled", False),
                      "transition_pending": True, "request_rejected": False}
            self._snapshot["msc"] = status
        return status

    def _run(self) -> None:
        while not self._stop.is_set():
            client = None
            try:
                client = self._client_factory()
                hardware = client.read_info()
                with self._device_lock:
                    self._active_client = client
                with self._lock:
                    self._snapshot = {
                        "connected": True,
                        "error": None,
                        "hardware": hardware,
                        "runtime": None,
                        "board_status": None,
                        "memory": None,
                        "msc": None,
                        "self_test": None,
                        "updated_at": None,
                    }
                while not self._stop.is_set():
                    with self._device_lock:
                        item = client.read_input_report(timeout_ms=250)
                    if item is not None and item["kind"] == "runtime":
                        runtime = item["data"]
                        board_status = _board_status_from_runtime(runtime)
                        with self._lock:
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                runtime=runtime,
                                board_status=board_status,
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
                    elif item is not None and item["kind"] == "memory":
                        with self._lock:
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                memory=item["data"],
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
                    elif item is not None and item["kind"] == "msc":
                        with self._lock:
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                msc=item["data"],
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
                    elif item is not None and item["kind"] == "self_test":
                        with self._lock:
                            self._snapshot.update(
                                connected=True, error=None, self_test=item["data"],
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
            except (OSError, RuntimeError, ValueError) as error:
                with self._lock:
                    self._snapshot = {**_empty_snapshot(), "error": str(error)}
            finally:
                if client is not None:
                    with self._device_lock:
                        if self._active_client is client:
                            self._active_client = None
                        try:
                            client.close()
                        except OSError:
                            pass
            self._stop.wait(1.0)

    def set_self_test(self, action: str, size_mib: int = 32) -> None:
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            self._active_client.set_self_test(action, size_mib)