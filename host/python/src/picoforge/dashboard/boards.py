"""Board discovery and per-board monitor ownership."""

from __future__ import annotations

import hashlib
import logging
import threading
import time
from typing import Callable

from ..hid.device import enumerate_devices
from .monitor import DeviceMonitor
from .models import DashboardBoard, DashboardBoardsResponse

_SERIAL_BOARD_GRACE_SECONDS = 30.0
_PATH_BOARD_GRACE_SECONDS = 5.0
_MAX_MANAGED_MONITORS = 16
_LOGGER = logging.getLogger(__name__)


class DashboardBoards:
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
        self._closed = False

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
                    # ponytail: one daemon thread per expired board, no retry. A monitor whose close() fails
                    # stays in _retiring, so that board is refused until restart; add retries if this is seen.
                    threading.Thread(target=self._retire_monitor, args=(board_id, monitor),
                                     name="picoforge-monitor-retirement", daemon=True).start()

            preferred = next((board["id"] for board in boards
                              if (self._serial_number and board["serial_number"] == self._serial_number)
                              or (self._device_path and board["device_path"] == self._device_path)), None)
            default = preferred or (
                None if self._serial_number or self._device_path or not boards else boards[0]["id"]
            )
            return {"boards": boards, "default_board_id": default}

    def _retire_monitor(self, board_id: str, monitor: DeviceMonitor) -> None:
        try:
            monitor.close()
        except Exception:
            _LOGGER.exception("Failed to close expired monitor for board %s", board_id)
            return
        with self._lock:
            if self._retiring.get(board_id) is monitor:
                self._retiring.pop(board_id)

    def monitor(self, board_id: str | None) -> DeviceMonitor:
        """Return the shared monitor for a listed board, starting it on first use."""
        with self._lock:
            if self._closed:
                raise RuntimeError("dashboard is shutting down")
            if not board_id or board_id not in self._known:
                raise LookupError("unknown board; rescan and select a listed board")
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

    def close(self) -> None:
        with self._lock:
            self._closed = True
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
