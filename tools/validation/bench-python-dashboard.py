#!/usr/bin/env python3
"""Measure Python dashboard hot paths; optional board mode is read-only.

Run with the dashboard environment, for example:
    host/python/.venv/bin/python tools/validation/bench-python-dashboard.py [--board [--serial SERIAL]]
"""

import argparse
from datetime import datetime, timezone
import math
import struct
import time
import timeit
from unittest.mock import Mock

from picoforge.dashboard.monitor import DeviceMonitor, _empty_snapshot
from picoforge.dashboard.server import create_app
from picoforge.hid.device import PicoForgeHid
from picoforge.reports import (
    parse_configuration_report, parse_identity_report, parse_memory_report, parse_self_test_report,
    parse_status_report, parse_wireless_status_report,
)

HARDWARE = parse_identity_report(bytes([1, 1, 2, 0]) + bytes.fromhex("0123456789abcdef")
                                 + bytes([1, 2, 3, 2, 2, 0, 0xb7]) + struct.pack("<II", 520192, 100000))
CONFIGURATION = parse_configuration_report(bytes([2, 1]) + struct.pack("<H", 200)
                                          + bytes([1, 255, 255, 255, 10, 11, 12, 13, 14, 15, 0x3a, 1, 25, 255]),
                                          HARDWARE)
WIRELESS = parse_wireless_status_report(bytes([12, 1, 2, 0, 1, 7, 192, 0, 2, 1, 3])
                                       + b"net" + bytes(29) + bytes(6))

SNAPSHOT = {
    **_empty_snapshot(), "connected": True,
    "hardware": HARDWARE, "board_configuration": CONFIGURATION, "settings_supported": True,
    "runtime": parse_status_report(bytes([3]) + struct.pack("<BBhQHHIBB", 1, 1, 3120, 123000, 1234, 5678,
                                                         200_000_000, 9, 0x0f), HARDWARE),
    "memory": parse_memory_report(bytes([10, 1, 7, 0x7f, 1, 1]) + struct.pack("<IBI", 25_000_000, 1, 4096)
                                  + bytes.fromhex("1b53444341524431211234567801aaff") + bytes([3, 0, 0, 0]), HARDWARE),
    "self_test": parse_self_test_report(bytes([11, 1, 1, 0, 0]) + bytes(28)),
    "wireless": {**WIRELESS, "wifi_enabled": True, "ble_enabled": True, "tcp_cli_enabled": True,
                 "stale": False, "remaining_seconds": None},
    "updated_at": datetime.now(timezone.utc).isoformat(),
}


class OneBoard:
    """Minimal dashboard registry serving one monitor."""

    def __init__(self, monitor):
        self._monitor = monitor

    def monitor(self, _board_id):
        return self._monitor

    def close(self):
        self._monitor.close()


def per_call_us(function, number):
    return timeit.timeit(function, number=number) / number * 1e6


def bench_host(seconds):
    monitor = DeviceMonitor(client_factory=Mock, autostart=False)
    monitor._snapshot = dict(SNAPSHOT)
    app = create_app(OneBoard(monitor))
    try:
        def refresh():
            monitor._input_received_at = dict.fromkeys(("runtime", "memory", "self_test"), time.monotonic())

        def snapshot():
            refresh()
            return monitor.snapshot()

        assert all(snapshot()[section] is not None for section in ("runtime", "memory", "self_test", "wireless"))
        print(f"snapshot() copy (including receipt refresh): {per_call_us(snapshot, 20000):.1f} us")
        # Keep the fixed benchmark payload fresh without changing production freshness thresholds.
        with app.test_client() as client:
            app.before_request(refresh)
            response = client.get("/api/status?board=a")
            assert response.status_code == 200 and all(
                response.json[section] is not None for section in ("runtime", "memory", "self_test", "wireless"))
            print(f"GET /api/status (Flask test client): "
                  f"{per_call_us(lambda: client.get('/api/status?board=a'), 2000):.0f} us")
    finally:
        app.extensions["picoforge_console"].close_all()
        monitor.close()

    idle = Mock(spec=PicoForgeHid)
    idle.read_info.return_value = HARDWARE
    idle.read_configuration.return_value = CONFIGURATION
    idle.read_wireless.return_value = SNAPSHOT["wireless"]
    idle.read_input_report.return_value = None
    polled = DeviceMonitor(client_factory=lambda: idle)
    try:
        wait_ready(polled)
        measured = cpu_ms_per_second(seconds)
        assert polled._thread.is_alive() and polled.snapshot()["connected"] and polled.snapshot()["error"] is None
        assert idle.read_input_report.called and idle.read_wireless.called
        print(f"idle poll loop (mocked HID): {measured:.2f} ms CPU/s")
    finally:
        polled.close()


def wait_ready(monitor):
    deadline = time.monotonic() + 5
    while not monitor.snapshot()["connected"] and time.monotonic() < deadline:
        time.sleep(0.01)
    snapshot = monitor.snapshot()
    if not snapshot["connected"] or snapshot["error"] is not None:
        raise RuntimeError(f"benchmark monitor did not become ready: {snapshot['error']}")
    return snapshot


def cpu_ms_per_second(seconds):
    start_cpu, start = time.process_time(), time.perf_counter()
    time.sleep(seconds)
    return (time.process_time() - start_cpu) / (time.perf_counter() - start) * 1000


def bench_board(serial, seconds):
    monitor = DeviceMonitor(serial)
    try:
        snapshot = wait_ready(monitor)
        measured = cpu_ms_per_second(seconds)
        assert monitor._thread.is_alive() and monitor.snapshot()["connected"] and monitor.snapshot()["error"] is None
        print(f"idle poll loop (board): {measured:.2f} ms CPU/s")
    finally:
        monitor.close()
    if snapshot["hardware"]["wireless_supported"]:
        with PicoForgeHid(serial) as device:
            device.read_info()
            device.read_configuration()
            print(f"read_wireless() (exclusive HID handle): {per_call_us(device.read_wireless, 50) / 1000:.2f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=5.0, help="idle CPU sample length")
    parser.add_argument("--board", action="store_true", help="also measure an attached board (read-only)")
    parser.add_argument("--serial", help="board serial number when several are attached")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        parser.error("--seconds must be positive and finite")
    bench_host(args.seconds)
    if args.board:
        bench_board(args.serial, args.seconds)


if __name__ == "__main__":
    main()
