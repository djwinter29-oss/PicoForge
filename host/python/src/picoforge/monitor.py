"""Reconnectable background HID monitor for the local dashboard."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import threading
import time
from typing import Any, Callable

from .device import PicoForgeHid, enumerate_devices
from .protocol import build_settings_command_report


def _empty_snapshot() -> dict[str, object]:
    return {
        "connected": False,
        "error": None,
        "hardware": None,
        "runtime": None,
        "board_status": None,
        "memory": None,
        "filesystem_format": None,
        "msc": None,
        "self_test": None,
        "settings": None,
        "settings_supported": False,
        "settings_message": None,
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
            current = self._snapshot.get("board_status")
            if not isinstance(current, dict):
                raise RuntimeError("PicoForge HID is disconnected")
            board_status = dict(current)
            board_status["led_on"] = on
            self._snapshot["board_status"] = board_status
        return board_status

    def set_msc(self, enable: bool, *, force: bool = False) -> dict[str, object]:
        if not isinstance(enable, bool):
            raise ValueError("MSC enable state must be boolean")
        if not isinstance(force, bool) or (force and enable):
            raise ValueError("Force return requires disabling MSC")
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            snapshot = self.snapshot()
            msc = snapshot.get("msc") or {}
            if force and (not msc.get("force_return_supported") or not msc.get("enabled")
                          or msc.get("transition_pending")):
                raise RuntimeError("Force return requires enabled MSC and firmware with force-return support")
            if force:
                self._active_client.set_msc(False, force=True)
            else:
                self._active_client.set_msc(enable)
        with self._lock:
            previous = self._snapshot.get("msc") or {}
            status = {"enabled": previous.get("enabled", False),
                      "transition_pending": True, "request_rejected": False,
                      "force_return_supported": previous.get("force_return_supported", False)}
            self._snapshot["msc"] = status
        return status

    def set_settings(self, settings: dict[str, object]) -> dict[str, int]:
        build_settings_command_report(settings)
        with self._device_lock:
            snapshot = self.snapshot()
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            if snapshot.get("settings_supported") is not True:
                raise RuntimeError("Boot settings are unavailable on this firmware")
            msc = snapshot.get("msc")
            if not isinstance(msc, dict):
                raise RuntimeError("Waiting for storage ownership status")
            if msc.get("enabled") or msc.get("transition_pending"):
                raise RuntimeError("Disable the USB drive before saving boot settings")
            if isinstance(snapshot.get("self_test"), dict) and snapshot["self_test"].get("active"):
                raise RuntimeError("Wait for the storage self-test to finish before saving boot settings")
            saved = self._active_client.set_settings(settings)
            with self._lock:
                self._snapshot.update(settings=saved, settings_message="Saved. Rebooting now.")
            return saved

    def _run(self) -> None:
        while not self._stop.is_set():
            client = None
            try:
                client = self._client_factory()
                hardware = client.read_info()
                try:
                    settings = client.read_settings()
                    settings_message = None
                except (OSError, RuntimeError, ValueError) as error:
                    settings = None
                    settings_message = f"Boot settings unavailable: {error}"
                with self._device_lock:
                    self._active_client = client
                with self._lock:
                    self._snapshot = {
                        **_empty_snapshot(), "connected": True, "hardware": hardware,
                        "settings": settings, "settings_supported": settings is not None,
                        "settings_message": settings_message,
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
                    elif item is not None and item["kind"] == "filesystem":
                        with self._lock:
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                filesystem_format=item["data"]["format"],
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


class DashboardBoards:
    """Share one lazily started HID monitor per board across dashboard requests."""

    def __init__(self, serial_number: str | None = None, device_path: str | None = None,
                 discover: Callable[[], list[dict[str, str | None]]] = enumerate_devices,
                 monitor_factory: Callable[[str | None, str | None], DeviceMonitor] = DeviceMonitor) -> None:
        self._serial_number = serial_number
        self._device_path = device_path
        self._discover = discover
        self._monitor_factory = monitor_factory
        self._lock = threading.RLock()
        self._known: dict[str, dict[str, str | None]] = {}
        self._monitors: dict[str, DeviceMonitor] = {}
        self._closed = False

    def boards(self) -> dict[str, object]:
        with self._lock:
            if self._closed:
                raise RuntimeError("dashboard is shutting down")
            boards = []
            seen: set[str] = set()
            for device in self._discover():
                serial = device.get("serial_number") or None
                path = device.get("device_path") or ""
                identity = f"serial:{serial}" if serial else f"path:{path}"
                board_id = hashlib.sha256(identity.encode()).hexdigest()
                if board_id in seen:
                    raise RuntimeError("duplicate PicoForge board identity; board serials must be unique")
                seen.add(board_id)
                board = {
                    "id": board_id,
                    "product_name": device.get("product_name") or "PicoForge",
                    "serial_number": serial,
                    "device_path": path,
                }
                self._known[board_id] = board
                boards.append(board)

            preferred = next((board["id"] for board in boards
                              if (self._serial_number and board["serial_number"] == self._serial_number)
                              or (self._device_path and board["device_path"] == self._device_path)), None)
            default = preferred or (
                None if self._serial_number or self._device_path or not boards else boards[0]["id"]
            )
            return {"boards": boards, "default_board_id": default}

    def _monitor(self, board_id: str | None) -> DeviceMonitor:
        with self._lock:
            if self._closed:
                raise RuntimeError("dashboard is shutting down")
            if not board_id or board_id not in self._known:
                raise RuntimeError("unknown board; rescan and select a listed board")
            if board_id not in self._monitors:
                board = self._known[board_id]
                serial = board["serial_number"]
                path = None if serial else board["device_path"]
                self._monitors[board_id] = self._monitor_factory(serial, path)
            return self._monitors[board_id]

    def snapshot(self, board_id: str | None = None) -> dict[str, object]:
        return _empty_snapshot() if board_id is None else self._monitor(board_id).snapshot()

    def set_led(self, board_id: str | None, on: bool) -> dict[str, object]:
        return self._monitor(board_id).set_led(on)

    def set_settings(self, board_id: str | None, settings: dict[str, object]) -> dict[str, int]:
        return self._monitor(board_id).set_settings(settings)

    def set_msc(self, board_id: str | None, enabled: bool, *, force: bool = False) -> dict[str, object]:
        return self._monitor(board_id).set_msc(enabled, force=force)

    def set_self_test(self, board_id: str | None, action: str, size_mib: int) -> None:
        self._monitor(board_id).set_self_test(action, size_mib)

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            monitors = list(self._monitors.values())
        for monitor in monitors:
            monitor.close()