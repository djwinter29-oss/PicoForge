"""Reconnectable background HID monitor for the local dashboard."""

from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timezone
import logging
import threading
import time
from typing import Any, Callable

from ..hid.device import PicoForgeHid
from ..hid.models import (
    BootSettings,
    ControlResult,
    InputReport,
    MscStatus,
    RuntimeStatus,
)
from ..reports import validate_wireless_configuration
from ..reports.common import _validate_settings
from .models import (
    BoardStatus,
    DashboardSnapshot,
    ReportFreshness,
    WirelessCancelResponse,
    WirelessDashboardResponse,
    WirelessSaveResponse,
    WirelessScanResponse,
    WirelessTestResponse,
)
from .freshness import calculate_report_freshness
from .policy import require_wireless_idle

_LOGGER = logging.getLogger(__name__)
_MSC_RECONNECT_TIMEOUT_SECONDS = 5.0
_INPUT_POLL_INTERVAL_SECONDS = 0.05


def _empty_snapshot() -> DashboardSnapshot:
    return {
        "connected": False,
        "reconnecting": False,
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
        "reboot_pending": False,
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


def _board_status_from_runtime(runtime: RuntimeStatus) -> BoardStatus:
    return {
        "temperature_valid": runtime["temperature_valid"],
        "temperature_centidegrees": runtime["temperature_centidegrees"],
        "led_available": runtime["led_available"],
        "led_on": runtime["led_on"],
    }


class DeviceMonitor:
    """Keep one HID handle open and publish an immutable status snapshot.

    The worker owns connection and report state; command methods publish command results
    and transitions under the same locks.
    """

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
        self._msc_reconnect_deadline = 0.0
        self._msc_reconnect_enabled: bool | None = None
        self._thread = threading.Thread(target=self._run, name="picoforge-hid-monitor", daemon=True)
        self._thread.start()

    def snapshot(self) -> DashboardSnapshot:
        with self._lock:
            result = deepcopy(self._snapshot)
            received_at = getattr(self, "_input_received_at", {}).copy()
            command_uncertain = getattr(self, "_command_outcome_uncertain", False)
        hardware = result.get("hardware")
        freshness = calculate_report_freshness(
            received_at,
            time.monotonic(),
            bool(result.get("connected")),
            hardware.get("supported", True) if isinstance(hardware, dict) else None,
            hardware.get("self_test_supported") if isinstance(hardware, dict) else None,
        )
        result["report_freshness"] = freshness
        result["command_outcome_uncertain"] = command_uncertain
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
        if threading.current_thread() is not self._thread:
            self._thread.join()

    def _send_control(
        self, method: Callable[..., Any], *args: object, **kwargs: object
    ) -> Any:
        stop = getattr(self, "_stop", None)
        if stop is not None and stop.is_set():
            raise RuntimeError("device monitor is shutting down")
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

    def set_led(self, on: bool) -> BoardStatus:
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

    def set_msc(self, enable: bool, *, force: bool = False) -> MscStatus:
        if not isinstance(enable, bool):
            raise ValueError("MSC enable state must be boolean")
        if not isinstance(force, bool) or (force and enable):
            raise ValueError("Force return requires disabling MSC")
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            snapshot = self.snapshot()
            msc = snapshot.get("msc") or {}
            if force and (not msc.get("enabled") or msc.get("transition_pending")):
                raise RuntimeError("Force return requires enabled MSC")
            result = (self._send_control(self._active_client.set_msc, False, force=True) if force
                      else self._send_control(self._active_client.set_msc, enable))
            with self._lock:
                self._snapshot["control_result"] = result
            if result.get("status") != "completed":
                raise RuntimeError(f"MSC command {result['status']}: {result['error']}")
            with self._lock:
                previous = self._snapshot.get("msc") or {}
                status = {"enabled": previous.get("enabled", False),
                          "transition_pending": True, "request_rejected": False}
                self._snapshot["msc"] = status
                self._snapshot["control_result"] = result
                changing_owner = previous.get("enabled", False) != enable
                self._msc_reconnect_deadline = (
                    time.monotonic() + _MSC_RECONNECT_TIMEOUT_SECONDS if changing_owner else 0.0
                )
                self._msc_reconnect_enabled = enable if changing_owner else None
        return status

    def reboot(self) -> ControlResult:
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            result = self._send_control(self._active_client.reboot)
            with self._lock:
                self._snapshot["control_result"] = result
            if result.get("status") != "completed":
                raise RuntimeError(f"reboot command {result['status']}: {result['error']}")
            return result

    def set_settings(self, settings: dict[str, object], *, force: bool = False) -> BootSettings:
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
            _validate_settings(settings)
            command_settings = settings
            msc = snapshot.get("msc")
            if not isinstance(msc, dict):
                raise RuntimeError("Waiting for storage ownership status")
            if type(force) is not bool:
                raise ValueError("Forced save confirmation must be boolean")
            if force and not msc.get("enabled"):
                raise RuntimeError("Forced save requires USB ownership")
            if msc.get("transition_pending") or (msc.get("enabled") and not force):
                raise RuntimeError("Disable the USB drive before saving boot settings")
            if isinstance(snapshot.get("self_test"), dict) and snapshot["self_test"].get("active"):
                raise RuntimeError("Wait for the storage self-test to finish before saving boot settings")
            try:
                saved = (self._send_control(self._active_client.set_settings, command_settings, force=True) if force
                         else self._send_control(self._active_client.set_settings, command_settings))
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
                    reboot_pending=True,
                    control_result=self._active_client.last_command_result,
                )
            return saved

    def read_wireless(self) -> WirelessDashboardResponse:
        with self._device_lock:
            stop = getattr(self, "_stop", None)
            if stop is not None and stop.is_set():
                raise RuntimeError("device monitor is shutting down")
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            return {"wireless": self._active_client.read_wireless(),
                    "reboot_pending": self.snapshot().get("reboot_pending", False)}

    def tcp_cli_auth_proof(self, nonce: bytes, uid: bytes) -> bytes:
        with self._device_lock:
            if self._active_client is None:
                raise RuntimeError("PicoForge HID is not connected")
            return self._send_control(self._active_client.tcp_cli_auth_proof, nonce, uid)

    def scan_wireless(self) -> WirelessScanResponse:
        with self._device_lock:
            snapshot = self.snapshot()
            require_wireless_idle(
                snapshot,
                client_connected=self._active_client is not None,
                command_outcome_uncertain=self._command_outcome_uncertain,
            )
            return {"networks": self._send_control(self._active_client.scan_wireless,
                                                   on_poll=self._poll_wireless_inputs)}

    def test_wireless(self, ssid: str, password: str = "") -> WirelessTestResponse:
        with self._device_lock:
            snapshot = self.snapshot()
            require_wireless_idle(
                snapshot,
                client_connected=self._active_client is not None,
                command_outcome_uncertain=self._command_outcome_uncertain,
            )
            try:
                return {"test": self._send_control(self._active_client.test_wireless, ssid, password,
                                                   on_poll=self._poll_wireless_inputs,
                                                   on_progress=self._set_wireless_phase)}
            finally:
                self._set_wireless_phase(None)

    def _set_wireless_phase(self, phase: str | None) -> None:
        with self._lock:
            self._snapshot["wireless_phase"] = phase

    def _poll_wireless_inputs(self) -> None:
        """Drain a bounded input batch while the wireless command owns the HID lock."""
        client = self._active_client
        if client is None:
            raise RuntimeError("PicoForge HID is not connected")
        for _ in range(16):
            item = client.read_input_report(timeout_ms=0)
            if item is None:
                break
            self._apply_input_report(item)

    def _apply_input_report(self, item: InputReport | None) -> None:
        if item is None:
            return
        kind = item["kind"]
        data = item["data"]
        with self._lock:
            if kind == "runtime":
                self._input_received_at[3] = time.monotonic()
                self._snapshot.update(
                    connected=True, error=None, runtime=data,
                    board_status=_board_status_from_runtime(data),
                    updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                )
            elif kind == "memory":
                if (not data["msc"]["transition_pending"]
                        and data["msc"]["enabled"] == getattr(self, "_msc_reconnect_enabled", None)):
                    self._msc_reconnect_deadline = 0.0
                    self._msc_reconnect_enabled = None
                self._input_received_at[10] = time.monotonic()
                self._snapshot.update(
                    connected=True, error=None, memory=data, filesystem_format=data["filesystem_format"],
                    msc=data["msc"], updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                )
            elif kind == "self_test":
                self._input_received_at[11] = time.monotonic()
                self._snapshot.update(
                    connected=True, error=None, self_test=data,
                    updated_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                )

    def cancel_wireless_test(self) -> WirelessCancelResponse:
        with self._device_lock:
            snapshot = self.snapshot()
            require_wireless_idle(
                snapshot,
                client_connected=self._active_client is not None,
                command_outcome_uncertain=self._command_outcome_uncertain,
            )
            self._send_control(self._active_client.cancel_wireless_test)
            return {"cancelled": True}

    def set_wireless(self, settings: dict[str, object]) -> WirelessSaveResponse:
        validate_wireless_configuration(settings)
        with self._device_lock:
            snapshot = self.snapshot()
            require_wireless_idle(
                snapshot,
                client_connected=self._active_client is not None,
                command_outcome_uncertain=self._command_outcome_uncertain,
            )
            result = dict(self._send_control(self._active_client.set_wireless, settings))
            saved = result.pop("settings")
            with self._lock:
                self._snapshot.update(settings=saved, board_configuration=self._active_client.configuration,
                                      reboot_pending=True,
                                      settings_message="Settings saved. Waiting for the board to reboot.")
            return {"wireless": result, "settings": saved, "reboot_pending": True}

    def _run(self) -> None:
        wireless_poll_at = 0.0
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
                        "radios": configuration["radios"],
                        "tcp_cli_enabled": configuration["tcp_cli_enabled"],
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
                        self._poll_wireless_inputs()
                        if hardware.get("wireless_supported"):
                            interval = 0.5 if getattr(client, "_wireless_active", False) else 2.0
                            if time.monotonic() >= wireless_poll_at:
                                wireless_poll_at = time.monotonic() + interval
                                try:
                                    wireless = client.read_wireless()
                                    with self._lock:
                                        self._snapshot["wireless"] = wireless
                                        self._snapshot["wireless_error"] = None
                                except (OSError, ValueError) as error:
                                    with self._lock:
                                        self._snapshot["wireless_error"] = str(error)
                            with self._lock:
                                self._snapshot["wireless_freshness"] = {
                                    "supported": True, **client.wireless_freshness(),
                                }
                    # ponytail: 50 ms caps report latency within the 1 s dashboard refresh; shorten if
                    # measured latency becomes noticeable.
                    self._stop.wait(_INPUT_POLL_INTERVAL_SECONDS)
            except (OSError, RuntimeError, ValueError) as error:
                with self._lock:
                    reconnecting = (
                        not getattr(self, "_command_outcome_uncertain", False)
                        and time.monotonic() < getattr(self, "_msc_reconnect_deadline", 0.0)
                        and (isinstance(error, OSError) or (
                            isinstance(error, RuntimeError)
                            and str(error).startswith("PicoForge HID interface not found;")
                        ))
                    )
                    if reconnecting:
                        _LOGGER.info("USB-drive mode change: waiting for HID reconnection: %s", error)
                    self._input_received_at = {}
                    self._snapshot = {
                        **_empty_snapshot(), "reconnecting": reconnecting,
                        "error": None if reconnecting else str(error),
                    }
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
