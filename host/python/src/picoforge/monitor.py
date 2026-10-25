"""Reconnectable background HID monitor for the local dashboard."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import hashlib
import threading
import time
from typing import Any, Callable

from .device import PicoForgeHid, enumerate_devices
from .protocol import (
    build_settings_command_report,
    normalize_legacy_settings,
    validate_legacy_settings,
)


def _empty_snapshot() -> dict[str, object]:
    return {
        "connected": False,
        "error": None,
        "hardware": None,
        "board_configuration": None,
        "runtime": None,
        "board_status": None,
        "memory": None,
        "filesystem_format": None,
        "msc": None,
        "self_test": None,
        "settings": None,
        "settings_supported": False,
        "settings_message": None,
        "control_result": None,
        "command_outcome_uncertain": False,
        "report_freshness": {
            "3": {"supported": None, "available": False, "stale": False, "age_ms": None},
            "10": {"supported": None, "available": False, "stale": False, "age_ms": None},
            "11": {"supported": None, "available": False, "stale": False, "age_ms": None},
        },
        "telemetry_stale": False,
        "storage_stale": False,
        "self_test_stale": None,
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
        self._input_received_at: dict[int, float] = {}
        self._command_outcome_uncertain = False
        self._thread = threading.Thread(target=self._run, name="picoforge-hid-monitor", daemon=True)
        self._thread.start()

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            result = deepcopy(self._snapshot)
            hardware = result.get("hardware")
            capability = hardware.get("self_test_supported") if isinstance(hardware, dict) else None
            now = time.monotonic()
            received_at = getattr(self, "_input_received_at", {})
            intervals = {3: 0.750, 10: 1.500, 11: 1.500}
            freshness = {}
            for report_id, interval in intervals.items():
                received = received_at.get(report_id)
                supported = capability if report_id == 11 else None
                if report_id != 11 and result.get("connected") and isinstance(hardware, dict):
                    supported = hardware.get("supported", True)
                age_ms = max(0, int((now - received) * 1000)) if received is not None else None
                freshness[str(report_id)] = {
                    "supported": supported,
                    "available": received is not None,
                    "stale": received is not None and now - received >= interval,
                    "age_ms": age_ms,
                }
            result["report_freshness"] = freshness
            result["command_outcome_uncertain"] = getattr(self, "_command_outcome_uncertain", False)
            result["telemetry_stale"] = freshness["3"]["stale"]
            result["storage_stale"] = freshness["10"]["stale"]
            result["self_test_stale"] = (
                freshness["11"]["stale"] if freshness["11"]["supported"] is not False else None
            )
            if freshness["3"]["stale"]:
                result["runtime"] = None
                result["board_status"] = None
            if freshness["10"]["stale"]:
                result["memory"] = None
                result["filesystem_format"] = None
                result["msc"] = None
            if freshness["11"]["stale"]:
                result["self_test"] = None
            return result

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _send_control(
        self, method: Callable[..., dict[str, object]], *args: object, **kwargs: object
    ) -> dict[str, object]:
        if getattr(self, "_command_outcome_uncertain", False):
            raise RuntimeError("a prior command outcome is uncertain; no further writes are allowed")
        try:
            return method(*args, **kwargs)
        except Exception:
            client = self._active_client
            if client is not None and getattr(client, "_command_uncertain", False):
                self._command_outcome_uncertain = True
                with self._lock:
                    self._snapshot["command_outcome_uncertain"] = True
            raise

    def set_led(self, on: bool) -> dict[str, object]:
        if not isinstance(on, bool):
            raise ValueError("LED state must be boolean")
        hardware = self.snapshot().get("hardware")
        if not isinstance(hardware, dict) or hardware.get("led_available") is not True:
            raise RuntimeError("PicoForge HID is disconnected or the board LED is unavailable")
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            result = self._send_control(self._active_client.set_led, on)
            with self._lock:
                if self._snapshot.get("connected"):
                    self._snapshot["control_result"] = result
            if result.get("status") != "completed":
                raise RuntimeError(f"LED command {result['status']}: {result['error']}")
        with self._lock:
            current = self._snapshot.get("board_status")
            board_status = dict(current) if isinstance(current, dict) else {
                "temperature_valid": None,
                "temperature_centidegrees": None,
                "led_available": True,
            }
            board_status["led_on"] = on
            if self._snapshot.get("connected"):
                if isinstance(current, dict):
                    self._snapshot["board_status"] = board_status
                self._snapshot["control_result"] = result
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
            result = (self._send_control(self._active_client.set_msc, False, force=True) if force
                      else self._send_control(self._active_client.set_msc, enable))
            with self._lock:
                self._snapshot["control_result"] = result
            if result.get("status") != "completed":
                raise RuntimeError(f"MSC command {result['status']}: {result['error']}")
        with self._lock:
            previous = self._snapshot.get("msc") or {}
            status = {"enabled": previous.get("enabled", False),
                      "transition_pending": True, "request_rejected": False,
                      "force_return_supported": previous.get("force_return_supported", False)}
            self._snapshot["msc"] = status
            self._snapshot["control_result"] = result
        return status

    def reboot(self) -> dict[str, object]:
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            result = self._send_control(self._active_client.reboot)
            with self._lock:
                self._snapshot["control_result"] = result
            if result.get("status") != "completed":
                raise RuntimeError(f"reboot command {result['status']}: {result['error']}")
            return result

    def set_settings(self, settings: dict[str, object]) -> dict[str, object]:
        if not isinstance(settings, dict):
            raise ValueError("boot settings must be an object")
        with self._device_lock:
            snapshot = self.snapshot()
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            if snapshot.get("settings_supported") is not True:
                raise RuntimeError("Boot settings are unavailable on this firmware")
            hardware = snapshot.get("hardware")
            if not isinstance(hardware, dict):
                raise RuntimeError("Waiting for device identity")
            if "sdcard_profile" in settings:
                build_settings_command_report(settings)
                command_settings = settings
            else:
                validate_legacy_settings(settings)
                configuration = snapshot.get("board_configuration")
                current_profile = (
                    configuration.get("sdcard_profile") if isinstance(configuration, dict) else None
                )
                command_settings = normalize_legacy_settings(
                    settings, hardware["storage_transport"], current_profile
                )
            build_settings_command_report(command_settings)
            msc = snapshot.get("msc")
            if not isinstance(msc, dict):
                raise RuntimeError("Waiting for storage ownership status")
            if msc.get("enabled") or msc.get("transition_pending"):
                raise RuntimeError("Disable the USB drive before saving boot settings")
            if isinstance(snapshot.get("self_test"), dict) and snapshot["self_test"].get("active"):
                raise RuntimeError("Wait for the storage self-test to finish before saving boot settings")
            try:
                saved = self._send_control(self._active_client.set_settings, command_settings)
            except (OSError, RuntimeError, ValueError):
                result = self._active_client.last_command_result
                if result is not None:
                    with self._lock:
                        self._snapshot["control_result"] = result
                raise
            configuration = self._active_client.configuration
            with self._lock:
                self._snapshot.update(
                    settings=saved,
                    board_configuration=configuration,
                    settings_message="Settings saved. Waiting for the board to reboot.",
                    control_result=self._active_client.last_command_result,
                )
            return saved

    def _run(self) -> None:
        while not self._stop.is_set():
            client = None
            try:
                client = self._client_factory()
                hardware = client.read_info()
                if not hardware.get("supported", True):
                    with self._lock:
                        self._input_received_at = {}
                        self._snapshot = {
                            **_empty_snapshot(),
                            "connected": True,
                            "hardware": hardware,
                            "error": f"Unsupported HID product: {hardware['product_name']}",
                        }
                    continue
                try:
                    configuration = client.read_configuration()
                    settings = {
                        "system_clock_mhz": configuration["system_clock_mhz"],
                        "sdcard_profile": configuration["sdcard_profile"],
                    }
                    settings_message = None
                except (OSError, RuntimeError, ValueError) as error:
                    settings = None
                    configuration = None
                    settings_message = f"Boot settings unavailable: {error}"
                with self._device_lock:
                    self._active_client = client
                with self._lock:
                    self._input_received_at = {}
                    self._snapshot = {
                        **_empty_snapshot(), "connected": True, "hardware": hardware,
                        "board_configuration": configuration,
                        "settings": settings,
                        "settings_supported": bool(hardware.get("settings_supported") and settings is not None),
                        "settings_message": settings_message,
                    }
                while not self._stop.is_set():
                    with self._device_lock:
                        item = client.read_input_report(timeout_ms=250)
                    if item is not None and item["kind"] == "runtime":
                        runtime = item["data"]
                        board_status = _board_status_from_runtime(runtime)
                        with self._lock:
                            self._input_received_at[3] = time.monotonic()
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                runtime=runtime,
                                board_status=board_status,
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
                    elif item is not None and item["kind"] == "memory":
                        with self._lock:
                            self._input_received_at[10] = time.monotonic()
                            self._snapshot.update(
                                connected=True,
                                error=None,
                                memory=item["data"],
                                filesystem_format=item["data"]["filesystem_format"],
                                msc=item["data"]["msc"],
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
                    elif item is not None and item["kind"] == "self_test":
                        with self._lock:
                            self._input_received_at[11] = time.monotonic()
                            self._snapshot.update(
                                connected=True, error=None, self_test=item["data"],
                                updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                            )
            except (OSError, RuntimeError, ValueError) as error:
                with self._lock:
                    self._input_received_at = {}
                    self._snapshot = {**_empty_snapshot(), "error": str(error)}
            finally:
                if client is not None:
                    with self._device_lock:
                        if self._active_client is client:
                            self._active_client = None
                            self._input_received_at = {}
                        try:
                            client.close()
                        except OSError:
                            pass
            self._stop.wait(1.0)

    def set_self_test(self, action: str, size_mib: int | None = None) -> None:
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            result = self._send_control(self._active_client.set_self_test, action, size_mib)
            with self._lock:
                self._snapshot["control_result"] = result
            if result.get("status") not in ("completed", "pending"):
                raise RuntimeError(f"self-test command {result['status']}: {result['error']}")


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

    def set_settings(self, board_id: str | None, settings: dict[str, object]) -> dict[str, object]:
        return self._monitor(board_id).set_settings(settings)

    def set_msc(self, board_id: str | None, enabled: bool, *, force: bool = False) -> dict[str, object]:
        return self._monitor(board_id).set_msc(enabled, force=force)

    def reboot(self, board_id: str | None) -> dict[str, object]:
        return self._monitor(board_id).reboot()

    def set_self_test(self, board_id: str | None, action: str, size_mib: int | None) -> None:
        self._monitor(board_id).set_self_test(action, size_mib)

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            monitors = list(self._monitors.values())
        for monitor in monitors:
            monitor.close()