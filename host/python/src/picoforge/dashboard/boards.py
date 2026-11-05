"""Board discovery and per-board monitor ownership."""

from __future__ import annotations

import hashlib
import logging
import queue
import threading
import time
from typing import Callable

from ..hid.device import enumerate_devices
from .backend import DashboardBackend
from .monitor import DeviceMonitor, _empty_snapshot
from .models import (
    BoardStatus,
    DashboardBoard,
    DashboardBoardsResponse,
    DashboardSnapshot,
    BootSettings,
    ControlResult,
    MscStatus,
    WirelessCancelResponse,
    WirelessDashboardResponse,
    WirelessSaveResponse,
    WirelessScanResponse,
    WirelessTestResponse,
)

_SERIAL_BOARD_GRACE_SECONDS = 30.0
_PATH_BOARD_GRACE_SECONDS = 5.0
_MAX_MANAGED_MONITORS = 16
_RETIREMENT_ATTEMPTS = 3
_RETIREMENT_RETRY_DELAYS_SECONDS = (0.05, 0.25)
_LOGGER = logging.getLogger(__name__)


class DashboardBoards(DashboardBackend):
    """Share one lazily started HID monitor per board across dashboard requests."""

    def __init__(self, serial_number: str | None = None, device_path: str | None = None,
                 discover: Callable[[], list[dict[str, str | None]]] = enumerate_devices,
                 monitor_factory: Callable[[str | None, str | None], DeviceMonitor] = DeviceMonitor,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._serial_number = serial_number
        self._device_path = device_path
        self._discover = discover
        self._monitor_factory = monitor_factory
        self._clock = clock
        self._lock = threading.RLock()
        self._known: dict[str, DashboardBoard] = {}
        self._last_seen: dict[str, float] = {}
        self._monitors: dict[str, DeviceMonitor] = {}
        self._retiring: dict[str, DeviceMonitor] = {}
        self._retirement_failures: dict[str, str] = {}
        self._retirement_queue: queue.Queue[tuple[str, DeviceMonitor] | None] = queue.Queue(
            maxsize=_MAX_MANAGED_MONITORS
        )
        self._closed = False
        self._retirement_thread: threading.Thread | None = None

    def boards(self) -> DashboardBoardsResponse:
        with self._lock:
            if self._closed:
                raise RuntimeError("dashboard is shutting down")
            now = self._clock()
            boards: list[DashboardBoard] = []
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
                self._last_seen[board_id] = now
                boards.append(board)

            expired_ids = [
                board_id for board_id, board in self._known.items()
                if board_id not in seen
                and now - self._last_seen[board_id] >= (
                    _SERIAL_BOARD_GRACE_SECONDS if board["serial_number"] else _PATH_BOARD_GRACE_SECONDS
                )
                and not (self._monitors.get(board_id)
                         and self._monitors[board_id].snapshot().get("command_outcome_uncertain"))
            ]
            for board_id in expired_ids:
                self._known.pop(board_id)
                self._last_seen.pop(board_id)
                monitor = self._monitors.pop(board_id, None)
                if monitor is not None:
                    self._retiring[board_id] = monitor
                    self._retirement_queue.put((board_id, monitor))
                    if self._retirement_thread is None:
                        self._retirement_thread = threading.Thread(
                            target=self._retire_monitors,
                            name="picoforge-monitor-retirement",
                            daemon=True,
                        )
                        self._retirement_thread.start()

            preferred = next((board["id"] for board in boards
                              if (self._serial_number and board["serial_number"] == self._serial_number)
                              or (self._device_path and board["device_path"] == self._device_path)), None)
            default = preferred or (
                None if self._serial_number or self._device_path or not boards else boards[0]["id"]
            )
            return {"boards": boards, "default_board_id": default}

    def _retire_monitors(self) -> None:
        while True:
            item = self._retirement_queue.get()
            try:
                if item is None:
                    return
                board_id, monitor = item
                self._retire_monitor(board_id, monitor)
            finally:
                self._retirement_queue.task_done()

    def _retire_monitor(self, board_id: str, monitor: DeviceMonitor) -> None:
        for attempt in range(_RETIREMENT_ATTEMPTS):
            try:
                monitor.close()
            except Exception as error:
                _LOGGER.exception("Failed to close expired monitor for board %s", board_id)
                if attempt < len(_RETIREMENT_RETRY_DELAYS_SECONDS):
                    time.sleep(_RETIREMENT_RETRY_DELAYS_SECONDS[attempt])
                    continue
                with self._lock:
                    if self._retiring.get(board_id) is monitor:
                        self._retirement_failures[board_id] = str(error)
                return
            with self._lock:
                if self._retiring.get(board_id) is monitor:
                    self._retiring.pop(board_id)
                    self._retirement_failures.pop(board_id, None)
            return

    def _monitor(self, board_id: str | None) -> DeviceMonitor:
        with self._lock:
            if self._closed:
                raise RuntimeError("dashboard is shutting down")
            if not board_id or board_id not in self._known:
                raise RuntimeError("unknown board; rescan and select a listed board")
            if board_id in self._retirement_failures:
                raise RuntimeError(
                    "board monitor shutdown failed; restart the dashboard before reconnecting"
                )
            if board_id in self._retiring:
                raise RuntimeError("board monitor is shutting down; retry after the next rescan")
            if board_id not in self._monitors:
                if len(self._monitors) + len(self._retiring) >= _MAX_MANAGED_MONITORS:
                    raise RuntimeError("monitor capacity reached; wait for cleanup or restart the dashboard")
                board = self._known[board_id]
                serial = board["serial_number"]
                path = None if serial else board["device_path"]
                self._monitors[board_id] = self._monitor_factory(serial, path)
            return self._monitors[board_id]

    def snapshot(self, board_id: str | None = None) -> DashboardSnapshot:
        return _empty_snapshot() if board_id is None else self._monitor(board_id).snapshot()

    def set_led(self, board_id: str | None, on: bool) -> BoardStatus:
        return self._monitor(board_id).set_led(on)

    def read_wireless(self, board_id: str | None) -> WirelessDashboardResponse:
        return self._monitor(board_id).read_wireless()

    def tcp_cli_auth_proof(self, board_id: str | None, nonce: bytes, uid: bytes) -> bytes:
        return self._monitor(board_id).tcp_cli_auth_proof(nonce, uid)

    def scan_wireless(self, board_id: str | None) -> WirelessScanResponse:
        return self._monitor(board_id).scan_wireless()

    def test_wireless(self, board_id: str | None, ssid: str, password: str = "") -> WirelessTestResponse:
        return self._monitor(board_id).test_wireless(ssid, password)

    def cancel_wireless_test(self, board_id: str | None) -> WirelessCancelResponse:
        return self._monitor(board_id).cancel_wireless_test()

    def set_wireless(self, board_id: str | None, settings: dict[str, object]) -> WirelessSaveResponse:
        return self._monitor(board_id).set_wireless(settings)

    def set_settings(self, board_id: str | None, settings: dict[str, object], *,
                     force: bool = False) -> BootSettings:
        return self._monitor(board_id).set_settings(settings, force=force)

    def set_msc(self, board_id: str | None, enabled: bool, *, force: bool = False) -> MscStatus:
        return self._monitor(board_id).set_msc(enabled, force=force)

    def reboot(self, board_id: str | None) -> ControlResult:
        return self._monitor(board_id).reboot()

    def set_self_test(self, board_id: str | None, action: str, size_mib: int | None) -> None:
        self._monitor(board_id).set_self_test(action, size_mib)

    def close(self) -> None:
        with self._lock:
            if not self._closed:
                self._closed = True
                if self._retirement_thread is not None:
                    self._retirement_queue.put(None)
            retirement_thread = self._retirement_thread
        if retirement_thread is not None:
            retirement_thread.join()
        with self._lock:
            monitors = list({id(monitor): monitor for monitor in
                             (*self._monitors.values(), *self._retiring.values())}.values())
        failures: list[Exception] = []
        for monitor in monitors:
            try:
                monitor.close()
            except Exception as error:
                _LOGGER.exception("Failed to close dashboard monitor during shutdown")
                failures.append(error)
        if failures:
            raise RuntimeError(f"Failed to close {len(failures)} dashboard monitor(s)") from failures[0]
