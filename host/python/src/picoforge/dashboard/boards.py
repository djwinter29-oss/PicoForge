"""Board discovery and per-board monitor ownership."""

from __future__ import annotations

import hashlib
import logging
import threading
import time
from typing import Callable

from ..hid.device import enumerate_devices
from .monitor import BoardConflict, DeviceMonitor
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
        self._discovery_lock = threading.Lock()
        self._close_lock = threading.Lock()
        self._known: dict[str, DashboardBoard] = {}
        self._last_seen: dict[str, float] = {}
        self._monitors: dict[str, DeviceMonitor] = {}
        self._retiring: dict[str, DeviceMonitor] = {}
        self._retirement_threads: dict[str, threading.Thread] = {}
        self._retirement_errors: dict[str, Exception] = {}
        self._ambiguous: set[str] = set()
        self._closed = False

    def boards(self) -> DashboardBoardsResponse:
        with self._discovery_lock:
            with self._lock:
                if self._closed:
                    raise BoardConflict("dashboard is shutting down")
            devices = self._discover()
            return self._publish_boards(devices)

    def _publish_boards(self, devices: list[dict[str, str | None]]) -> DashboardBoardsResponse:
        """Publish one complete enumeration without holding the registry lock during HID discovery."""
        with self._lock:
            if self._closed:
                raise BoardConflict("dashboard is shutting down")
            now = self._clock()
            boards: list[DashboardBoard] = []
            seen: set[str] = set()
            ambiguous: set[str] = set()
            for device in devices:
                serial = device.get("serial_number") or None
                path = device.get("device_path") or ""
                identity = f"serial:{serial}" if serial else f"path:{path}"
                board_id = hashlib.sha256(identity.encode("utf-8", errors="surrogateescape")).hexdigest()
                if board_id in seen:
                    ambiguous.add(board_id)
                seen.add(board_id)
                board: DashboardBoard = {
                    "id": board_id,
                    "product_name": device.get("product_name") or "PicoForge",
                    "serial_number": serial,
                    "device_path": path,
                }
                boards.append(board)

            for board_id in ambiguous - self._ambiguous:
                _LOGGER.warning("Quarantining duplicate PicoForge board identity %s", board_id)
            changed = self._ambiguous ^ ambiguous
            self._ambiguous = ambiguous
            for board_id, monitor in self._monitors.items():
                if board_id in changed:
                    monitor.set_identity_ambiguous(board_id in ambiguous)
            boards = [board for board in boards if board["id"] not in ambiguous]
            for board in boards:
                self._known[board["id"]] = board
                self._last_seen[board["id"]] = now

            expired_ids = [
                board_id for board_id, board in self._known.items()
                if board_id not in seen
                and now - self._last_seen[board_id] >= (
                    _SERIAL_BOARD_GRACE_SECONDS if board["serial_number"] else _PATH_BOARD_GRACE_SECONDS
                )
            ]
            for board_id in expired_ids:
                monitor = self._monitors.get(board_id)
                if monitor is not None and not monitor.try_retire():
                    continue
                self._known.pop(board_id)
                self._last_seen.pop(board_id)
                monitor = self._monitors.pop(board_id, None)
                if monitor is not None:
                    self._retiring[board_id] = monitor
                    # ponytail: at most 16 retirement threads, no retry; failed cleanup refuses reconnect until
                    # restart. Add supervised retirement only if recurring failures need in-process recovery.
                    worker = threading.Thread(target=self._retire_monitor, args=(board_id, monitor),
                                              name="picoforge-monitor-retirement", daemon=True)
                    self._retirement_threads[board_id] = worker
                    worker.start()

            preferred = next((board["id"] for board in boards
                              if (self._serial_number and board["serial_number"] == self._serial_number)
                              or (self._device_path and board["device_path"] == self._device_path)), None)
            default = preferred or (
                None if self._serial_number or self._device_path or not boards else boards[0]["id"]
            )
            result: DashboardBoardsResponse = {"boards": boards, "default_board_id": default}
            if ambiguous:
                result["warnings"] = ["Duplicate PicoForge board identity; disconnect boards with duplicate serials"]
            return result

    def _retire_monitor(self, board_id: str, monitor: DeviceMonitor) -> None:
        try:
            monitor.close()
        except Exception as error:
            _LOGGER.exception("Failed to close expired monitor for board %s", board_id)
            with self._lock:
                self._retirement_errors[board_id] = error
            return
        with self._lock:
            if self._retiring.get(board_id) is monitor:
                self._retiring.pop(board_id)
                self._retirement_threads.pop(board_id)

    def monitor(self, board_id: str | None) -> DeviceMonitor:
        """Return the shared monitor for a listed board, starting it on first use."""
        with self._lock:
            if self._closed:
                raise BoardConflict("dashboard is shutting down")
            if board_id in self._ambiguous:
                raise BoardConflict("Duplicate PicoForge board identity; disconnect boards with duplicate serials")
            if not board_id or board_id not in self._known:
                raise LookupError("unknown board; rescan and select a listed board")
            if board_id in self._retiring:
                raise BoardConflict("board monitor is shutting down; retry after the next rescan")
            if board_id not in self._monitors:
                if len(self._monitors) + len(self._retiring) >= _MAX_MANAGED_MONITORS:
                    raise BoardConflict("monitor capacity reached; wait for cleanup or restart the dashboard")
                board = self._known[board_id]
                serial = board["serial_number"]
                path = None if serial else board["device_path"]
                self._monitors[board_id] = self._monitor_factory(serial, path)
            return self._monitors[board_id]

    def close(self) -> None:
        with self._close_lock:
            with self._lock:
                self._closed = True
                monitors = list(self._monitors.items())
                retirements = list(self._retirement_threads.values())
            failures: list[Exception] = []
            for board_id, monitor in monitors:
                try:
                    monitor.close()
                except Exception as error:
                    _LOGGER.exception("Failed to close dashboard monitor during shutdown")
                    failures.append(error)
                else:
                    with self._lock:
                        self._monitors.pop(board_id)
            for worker in retirements:
                worker.join()
            with self._lock:
                failures.extend(self._retirement_errors.values())
            if failures:
                raise RuntimeError(f"Failed to close {len(failures)} dashboard monitor(s)") from failures[0]
