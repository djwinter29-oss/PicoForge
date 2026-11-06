"""Reconnectable background HID monitor for the local dashboard."""

from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
from datetime import datetime, timezone
import logging
import threading
import time
from typing import Any, Callable, Generator, ParamSpec, TypeVar, cast

from ..hid.device import PicoForgeHid, WirelessTestCancelled
from ..reports.models import (
    BoardConfiguration,
    BootSettings,
    ControlResult,
    InputReport,
    MscStatus,
    WirelessNetwork,
    WirelessSaveResult,
    WirelessStatus,
    WirelessTestResult,
)
from ..reports import ProtocolError
from .models import (
    ControlResponse,
    DashboardSnapshot,
    MscResponse,
    ResyncResponse,
    SettingsResponse,
    WirelessSaveResponse,
)

_LOGGER = logging.getLogger(__name__)
_MSC_RECONNECT_TIMEOUT_SECONDS = 5.0
_INPUT_POLL_INTERVAL_SECONDS = 0.05
_IDLE_INPUT_POLL_INTERVAL_SECONDS = 0.5
_IDLE_WIRELESS_POLL_INTERVAL_SECONDS = 5.0
_VIEWER_IDLE_TIMEOUT_SECONDS = 5.0
_BUSY_WAIT_SECONDS = 2.0
_P = ParamSpec("_P")
_T = TypeVar("_T")
# Input report kind -> (maximum age before the section is withheld, stale flag); matches .NET.
_REPORTS = {
    "runtime": (0.750, "telemetry_stale"),
    "memory": (1.500, "storage_stale"),
    "self_test": (1.500, "self_test_stale"),
}


class CommandOutcomeUncertain(RuntimeError):
    """A control command was delivered but its final outcome was not confirmed."""


class BoardConflict(RuntimeError):
    """An expected board lifecycle conflict; the caller must wait or change board state."""


def _empty_snapshot() -> DashboardSnapshot:
    return {
        "connected": False,
        "reconnecting": False,
        "error": None,
        "hardware": None,
        "board_configuration": None,
        "runtime": None,
        "memory": None,
        "self_test": None,
        "settings_supported": False,
        "settings_message": None,
        "control_result": None,
        "command_outcome_uncertain": False,
        "command_resync_supported": True,
        "reboot_pending": False,
        "telemetry_stale": True,
        "storage_stale": True,
        "self_test_stale": True,
        "updated_at": None,
        "wireless_phase": None,
        "wireless": None,
        "wireless_error": None,
    }


def _msc(snapshot: DashboardSnapshot) -> MscStatus | None:
    memory = snapshot.get("memory")
    return memory.get("msc") if isinstance(memory, dict) else None


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
        *,
        autostart: bool = True,
    ) -> None:
        self._client_factory = client_factory or (lambda: PicoForgeHid(serial_number, device_path))
        self._lock = threading.RLock()
        self._device_lock = threading.RLock()
        self._active_client: PicoForgeHid | None = None
        self._close_error: Exception | None = None
        self._failed_client: PicoForgeHid | None = None
        self._stop = threading.Event()
        self._snapshot = _empty_snapshot()
        self._input_received_at: dict[str, float] = {}
        self._last_status_request_at = time.monotonic() - _VIEWER_IDLE_TIMEOUT_SECONDS
        self._command_outcome_uncertain = False
        self._uncertain_command: tuple[int, int] | None = None
        self._uncertain_wireless_operation: tuple[str, int] | None = None
        self._msc_reconnect_deadline = 0.0
        self._msc_reconnect_enabled: bool | None = None
        self._wireless_test_cancel: threading.Event | None = None
        self._thread = threading.Thread(target=self._run, name="picoforge-hid-monitor", daemon=True)
        if autostart:
            self._thread.start()

    def snapshot(self) -> DashboardSnapshot:
        # ponytail: a deep copy (~50 us for a full snapshot) keeps route code off shared state; copy only changed
        # sections if one status request exceeds 1 ms or more than ~10 tabs poll at once.
        with self._lock:
            result = cast(DashboardSnapshot, deepcopy(self._snapshot))
            received_at = self._input_received_at.copy()
            result["command_outcome_uncertain"] = self._command_outcome_uncertain
        now = time.monotonic()
        for kind, (max_age, flag) in _REPORTS.items():
            stale = kind not in received_at or now - received_at[kind] >= max_age
            result[flag] = stale  # type: ignore[literal-required]
            if stale:
                result[kind] = None  # type: ignore[literal-required]
        return result

    def status_snapshot(self) -> DashboardSnapshot:
        """Record a browser poll before returning the same freshness-filtered snapshot."""
        with self._lock:
            self._last_status_request_at = time.monotonic()
        return self.snapshot()

    def _reset_snapshot(self, **fields: Any) -> None:
        """Replace the snapshot with an empty one plus fields and forget every input report age."""
        with self._lock:
            self._input_received_at = {}
            self._snapshot = cast(DashboardSnapshot, {**_empty_snapshot(), **fields})

    def close(self) -> None:
        self._stop.set()
        if self._thread.ident is not None and threading.current_thread() is not self._thread:
            self._thread.join()
        if self._close_error is not None:
            raise RuntimeError("HID cleanup failed; restart the dashboard before reconnecting") from self._close_error

    def try_retire(self) -> bool:
        """Claim idle, resolved command ownership without waiting; prevent subsequent commands from starting."""
        if not self._device_lock.acquire(blocking=False):
            return False
        try:
            if self._command_outcome_uncertain:
                return False
            self._stop.set()
            return True
        finally:
            self._device_lock.release()

    def _client(self) -> PicoForgeHid:
        if self._stop.is_set():
            raise BoardConflict("device monitor is shutting down")
        if self._active_client is None:
            raise ConnectionError("PicoForge HID is not connected")
        return self._active_client

    @contextmanager
    def _device(self) -> Generator[None, None, None]:
        """Own the board for one API command, or refuse quickly while a long Wi-Fi operation holds it."""
        # ponytail: a 2 s wait turns a 15 s scan or 35 s test into a quick 409 instead of a hung request; queue
        # commands per board if users need them to run after the operation instead of retrying.
        if not self._device_lock.acquire(timeout=_BUSY_WAIT_SECONDS):
            raise BoardConflict("Board is busy with another command; retry when it finishes")
        try:
            if self._stop.is_set():
                raise BoardConflict("device monitor is shutting down")
            yield
        finally:
            self._device_lock.release()

    def _send_control(self, method: Callable[_P, _T], *args: _P.args, **kwargs: _P.kwargs) -> _T:
        """Delegate transaction state to HID; preserve unresolved references across client replacement."""
        if self._command_outcome_uncertain:
            raise RuntimeError("a prior command outcome is uncertain; no further writes are allowed")
        client = self._active_client
        previous_result = client.last_command_result if client is not None else None
        try:
            result = method(*args, **kwargs)
        except Exception as error:
            self._publish_command_result(client, previous_result)
            if client is not None and client.command_uncertain is True:
                self._record_uncertain_command(client)
                raise CommandOutcomeUncertain(
                    "Command outcome is uncertain; re-sync with the board before retrying"
                ) from error
            raise
        self._publish_command_result(client, previous_result, result)
        return result

    def _record_uncertain_command(self, client: PicoForgeHid) -> None:
        command = getattr(client, "uncertain_command", None)
        operation = getattr(client, "uncertain_wireless_operation", None)
        with self._lock:
            self._command_outcome_uncertain = True
            self._uncertain_command = command if (
                isinstance(command, tuple) and len(command) == 2
                and all(type(value) is int for value in command)
            ) else None
            self._uncertain_wireless_operation = operation if (
                isinstance(operation, tuple) and len(operation) == 2
                and isinstance(operation[0], str) and type(operation[1]) is int
            ) else None

    def _publish_command_result(
        self, client: PicoForgeHid | None, previous: object, returned: object = None,
    ) -> None:
        current = client.last_command_result if client is not None else None
        result = current if current is not previous else returned
        if not isinstance(result, dict) or "status" not in result:
            return
        with self._lock:
            if self._snapshot.get("connected"):
                self._snapshot["control_result"] = cast(ControlResult, result)

    def set_led(self, on: bool) -> ControlResponse:
        with self._device():
            client = self._client()
            hardware = self.snapshot().get("hardware")
            if not isinstance(hardware, dict) or hardware.get("led_available") is not True:
                raise RuntimeError("The board LED is unavailable")
            result = self._send_control(client.set_led, on)
            PicoForgeHid._require_completed(result, "LED")
        with self._lock:
            runtime = self._snapshot.get("runtime")
            if isinstance(runtime, dict):
                runtime["led_on"] = on
        return {"control_result": result}

    def set_msc(self, enable: bool, *, force: bool = False) -> MscResponse:
        if force and enable:
            raise RuntimeError("Force return requires disabling MSC")
        with self._device():
            client = self._client()
            msc = _msc(self.snapshot()) or {}
            if force and (not msc.get("enabled") or msc.get("transition_pending")):
                raise RuntimeError("Force return requires enabled MSC")
            result = self._send_control(client.set_msc, enable, force=force)
            PicoForgeHid._require_completed(result, "MSC")
            with self._lock:
                memory = self._snapshot.get("memory")
                previous = _msc(cast(DashboardSnapshot, self._snapshot)) or {}
                status: MscStatus = {"enabled": previous.get("enabled", False),
                                     "transition_pending": True, "request_rejected": False}
                if isinstance(memory, dict):
                    memory["msc"] = status
                changing_owner = previous.get("enabled", False) != enable
                self._msc_reconnect_deadline = (
                    time.monotonic() + _MSC_RECONNECT_TIMEOUT_SECONDS if changing_owner else 0.0
                )
                self._msc_reconnect_enabled = enable if changing_owner else None
        return {"msc": status, "control_result": result}

    def reboot(self) -> ControlResponse:
        with self._device():
            result = self._send_control(self._client().reboot)
            PicoForgeHid._require_completed(result, "reboot")
            return {"control_result": result}

    def set_self_test(self, action: str, size_mib: int | None = None) -> ControlResponse:
        with self._device():
            result = self._send_control(self._client().set_self_test, action, size_mib)
            PicoForgeHid._require_completed(result, "self-test")
            return {"control_result": result}

    def set_settings(self, settings: dict[str, object], *, force: bool = False) -> SettingsResponse:
        with self._device():
            snapshot = self.snapshot()
            client = self._client()
            if snapshot.get("settings_supported") is not True:
                raise RuntimeError("Boot settings are unavailable on this firmware")
            if not isinstance(snapshot.get("hardware"), dict):
                raise RuntimeError("Waiting for device identity")
            msc = _msc(snapshot)
            if not isinstance(msc, dict):
                raise RuntimeError("Waiting for storage ownership status")
            if force and not msc.get("enabled"):
                raise RuntimeError("Forced save requires USB ownership")
            if msc.get("transition_pending") or (msc.get("enabled") and not force):
                raise RuntimeError("Disable the USB drive before saving boot settings")
            self_test = snapshot.get("self_test")
            if isinstance(self_test, dict) and self_test.get("active"):
                raise RuntimeError("Wait for the storage self-test to finish before saving boot settings")
            saved = self._send_control(client.set_settings, settings, force=force)
            self._publish_saved_settings(saved)
            return {"settings": saved, "control_result": client.last_command_result}

    def _publish_saved_settings(self, saved: BootSettings) -> None:
        with self._lock:
            configuration = self._snapshot.get("board_configuration")
            if configuration is None:
                self._snapshot["board_configuration"] = None
            else:
                self._snapshot["board_configuration"] = cast(
                    BoardConfiguration, {**configuration, **saved},
                )
            self._snapshot["settings_message"] = "Settings saved. Waiting for the board to reboot."
            self._snapshot["reboot_pending"] = True

    def tcp_cli_auth_proof(self, nonce: bytes, uid: bytes) -> bytes:
        with self._device():
            return self._send_control(self._client().tcp_cli_auth_proof, nonce, uid)

    def resync_command(self) -> ResyncResponse:
        """Clear uncertainty only when the board reports the exact pending operation as final."""
        with self._device():
            if not self._command_outcome_uncertain:
                return {"uncertain": False, "control_result": None, "wireless": None}
            client = self._client()
            try:
                outcome = client.reconcile_outcome(
                    self._uncertain_command, self._uncertain_wireless_operation,
                )
            except (OSError, ProtocolError) as error:
                raise CommandOutcomeUncertain(
                    "Could not confirm the command result; uncertainty remains"
                ) from error
            if outcome is None:
                raise CommandOutcomeUncertain(
                    "No final result matches the pending command; uncertainty remains"
                )
            control_result, wireless = outcome
            with self._lock:
                self._command_outcome_uncertain = False
                self._uncertain_command = None
                self._uncertain_wireless_operation = None
                if self._snapshot.get("connected"):
                    if control_result is not None:
                        self._snapshot["control_result"] = control_result
                    if wireless is not None:
                        self._snapshot["wireless"] = wireless
                        self._snapshot["wireless_error"] = None
            return {"uncertain": False, "control_result": control_result, "wireless": wireless}

    def _require_wireless_idle(self) -> PicoForgeHid:
        """Return the client when wireless staging cannot race storage, self-test or another command."""
        snapshot = self.snapshot()
        client = self._client()
        hardware = snapshot.get("hardware")
        if (not snapshot.get("connected")
                or not isinstance(hardware, dict) or not hardware.get("wireless_supported")):
            raise RuntimeError("Wireless provisioning is unavailable on this firmware")
        if snapshot.get("command_outcome_uncertain"):
            raise RuntimeError("A prior command outcome is uncertain; no further writes are allowed")
        msc = _msc(snapshot)
        self_test = snapshot.get("self_test")
        # The device lock serializes host commands and firmware answers "busy" itself; a stored busy result is history.
        if (snapshot.get("storage_stale") is not False
                or not isinstance(msc, dict) or msc.get("enabled") or msc.get("transition_pending")
                or snapshot.get("reboot_pending")
                or hardware.get("self_test_supported") and snapshot.get("self_test_stale") is not False
                or isinstance(self_test, dict) and self_test.get("active")):
            raise RuntimeError("Wireless scan and connection test require idle firmware-owned storage")
        return client

    def scan_wireless(self) -> dict[str, list[WirelessNetwork]]:
        with self._device():
            client = self._require_wireless_idle()
            return {"networks": self._send_control(client.scan_wireless, on_poll=self._drain_input_reports)}

    def test_wireless(self, ssid: str | bytes, password: str = "") -> dict[str, WirelessTestResult]:
        with self._device():
            client = self._require_wireless_idle()
            stop = threading.Event()
            with self._lock:
                self._wireless_test_cancel = stop
            cancel_sent = False
            try:
                return {"test": self._send_control(client.test_wireless, ssid, password,
                                                   on_poll=self._drain_input_reports,
                                                   on_progress=self._set_wireless_phase, cancel=stop)}
            except WirelessTestCancelled:
                cancel_sent = True
                raise
            finally:
                with self._lock:
                    self._wireless_test_cancel = None
                self._set_wireless_phase(None)
                # A cancel that arrived after the client's last check must still clear the test credentials.
                if stop.is_set() and not cancel_sent and not self._command_outcome_uncertain:
                    try:
                        self._send_control(client.cancel_wireless_test)
                    except (OSError, RuntimeError, ValueError) as error:
                        _LOGGER.exception("Late Wi-Fi test cancellation failed; board command state is uncertain")
                        self._record_uncertain_command(client)
                        raise CommandOutcomeUncertain(
                            "Late Wi-Fi test cancellation failed; command outcome is uncertain"
                        ) from error

    def _set_wireless_phase(self, phase: str | None) -> None:
        with self._lock:
            self._snapshot["wireless_phase"] = phase

    def _drain_input_reports(self) -> None:
        """Apply a bounded batch of queued input reports while the caller owns the HID lock."""
        client = self._active_client
        if client is None:
            raise ConnectionError("PicoForge HID is not connected")
        for _ in range(16):
            try:
                item = client.read_input_report(timeout_ms=0)
            except ProtocolError as error:
                _LOGGER.warning("Discarding malformed HID input report: %s", error)
                continue
            if item is None:
                break
            self._apply_input_report(item)

    def _apply_input_report(self, item: InputReport) -> None:
        with self._lock:
            self._input_received_at[item["kind"]] = time.monotonic()
            if item["kind"] == "runtime":
                self._snapshot["runtime"] = item["data"]
            elif item["kind"] == "memory":
                data = item["data"]
                if (not data["msc"]["transition_pending"]
                        and data["msc"]["enabled"] == self._msc_reconnect_enabled):
                    self._msc_reconnect_deadline = 0.0
                    self._msc_reconnect_enabled = None
                self._snapshot["memory"] = data
            else:
                self._snapshot["self_test"] = item["data"]
            self._snapshot["connected"] = True
            self._snapshot["error"] = None
            self._snapshot["updated_at"] = datetime.now(timezone.utc).isoformat(timespec="seconds")

    def cancel_wireless_test(self) -> dict[str, bool]:
        # A running test owns the board for up to 35 s; it sends the cancel at its next 0.5 s poll instead.
        with self._lock:
            if self._wireless_test_cancel is not None:
                self._wireless_test_cancel.set()
                return {"cancelled": True}
        with self._device():
            client = self._require_wireless_idle()
            self._send_control(client.cancel_wireless_test)
            return {"cancelled": True}

    def set_wireless(self, settings: dict[str, object]) -> WirelessSaveResponse:
        with self._device():
            client = self._require_wireless_idle()
            result = cast(WirelessSaveResult, self._send_control(client.set_wireless, settings))
            saved = result["settings"]
            self._publish_saved_settings(saved)
            wireless = cast(WirelessStatus, {key: value for key, value in result.items() if key != "settings"})
            return {"wireless": wireless, "settings": saved, "reboot_pending": True}

    def _run(self) -> None:
        while not self._stop.is_set():
            client = None
            try:
                client = self._client_factory()
                hardware = client.read_info()
                if hardware["supported"] is False:
                    self._reset_snapshot(connected=True, hardware=hardware,
                                         error=f"Unsupported HID product: {hardware['product_name']}")
                    continue
                try:
                    configuration = client.read_configuration()
                    settings_message = None
                except (OSError, RuntimeError, ValueError) as error:
                    configuration = None
                    settings_message = f"Boot settings unavailable: {error}"
                with self._device_lock:
                    self._active_client = client
                self._reset_snapshot(
                    connected=True, hardware=hardware, board_configuration=configuration,
                    settings_supported=hardware["settings_supported"] and configuration is not None,
                    settings_message=settings_message,
                )
                wireless_poll_at = 0.0
                viewer_active_last = False
                while not self._stop.is_set():
                    with self._device_lock:
                        self._drain_input_reports()
                        now = time.monotonic()
                        with self._lock:
                            viewer_active = now - self._last_status_request_at < _VIEWER_IDLE_TIMEOUT_SECONDS
                        if viewer_active and not viewer_active_last:
                            wireless_poll_at = now
                        viewer_active_last = viewer_active
                        poll_interval = (
                            _INPUT_POLL_INTERVAL_SECONDS if viewer_active else _IDLE_INPUT_POLL_INTERVAL_SECONDS
                        )
                        if hardware["wireless_supported"] and now >= wireless_poll_at:
                            wireless_interval = (
                                (0.5 if client.wireless_active else 2.0)
                                if viewer_active else _IDLE_WIRELESS_POLL_INTERVAL_SECONDS
                            )
                            wireless_poll_at = now + wireless_interval
                            wireless: WirelessStatus | None
                            wireless_error: str | None
                            try:
                                wireless, wireless_error = client.read_wireless(), None
                            except (OSError, ValueError) as error:
                                wireless, wireless_error = None, str(error)
                            with self._lock:
                                self._snapshot["wireless_error"] = wireless_error
                                if wireless is not None:
                                    self._snapshot["wireless"] = wireless
                                else:
                                    previous_wireless = self._snapshot["wireless"]
                                    if previous_wireless is not None:
                                        previous_wireless["stale"] = True
                    self._stop.wait(poll_interval)
            except Exception as error:
                message = str(error)
                if not isinstance(error, (OSError, RuntimeError, ValueError)):
                    # A malformed report or code bug must not silently end the thread behind a "connected" snapshot.
                    _LOGGER.exception("HID monitor failed unexpectedly")
                    message = f"Internal monitor error: {error!r}"
                with self._lock:
                    reconnecting = (
                        not self._command_outcome_uncertain
                        and time.monotonic() < self._msc_reconnect_deadline
                        and isinstance(error, OSError)
                    )
                    if reconnecting:
                        _LOGGER.info("USB-drive mode change: waiting for HID reconnection: %s", error)
                    self._reset_snapshot(reconnecting=reconnecting, error=None if reconnecting else message)
            finally:
                if client is not None:
                    with self._device_lock:
                        if self._active_client is client:
                            self._active_client = None
                            with self._lock:
                                self._input_received_at = {}
                        try:
                            client.close()
                        except Exception as error:
                            _LOGGER.exception("Failed to close HID connection; stopping monitor to prevent overlap")
                            self._close_error = error
                            self._failed_client = client
                            self._stop.set()
                            self._reset_snapshot(error="HID cleanup failed; restart the dashboard")
                # Every exit path, including the unsupported-product `continue`, must back off before reopening.
                self._stop.wait(1.0)
