#!/usr/bin/env python3
"""Measure Python dashboard hot paths; optional board mode is read-only.

Run with the dashboard environment, for example:
    host/python/.venv/bin/python tools/validation/bench-python-dashboard.py [--board [--serial SERIAL]]
"""

import argparse
import math
import time
import timeit
from unittest.mock import Mock

from picoforge.dashboard.monitor import DeviceMonitor, _empty_snapshot
from picoforge.dashboard.server import create_app

SNAPSHOT = {
    **_empty_snapshot(), "connected": True,
    "hardware": {"product_name": "PicoForge", "wireless_supported": True, "self_test_supported": True},
    "runtime": {"led_on": False, "temperature_c": 31.2, "uptime_s": 123, "system_clock_mhz": 200},
    "memory": {"msc": {"enabled": False, "transition_pending": False, "request_rejected": False},
               "heap_used": 1, "heap_free": 2},
    "wireless": {"ssid": "net", "wifi_state": "connected", "ipv4": "192.0.2.1",
                 "networks": [{"ssid": f"net{i}", "rssi": -50} for i in range(20)]},
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
    monitor._input_received_at = dict.fromkeys((3, 10, 11), math.inf)
    print(f"snapshot() copy: {per_call_us(monitor.snapshot, 20000):.1f} us")
    client = create_app(OneBoard(monitor)).test_client()
    print(f"GET /api/status (Flask test client): {per_call_us(lambda: client.get('/api/status?board=a'), 2000):.0f} us")

    idle = Mock()
    idle.read_info.return_value = {"supported": True}
    idle.read_input_report.return_value = None
    polled = DeviceMonitor(client_factory=lambda: idle)
    print(f"idle poll loop (mocked HID): {cpu_ms_per_second(seconds):.2f} ms CPU/s")
    polled.close()


def cpu_ms_per_second(seconds):
    start_cpu, start = time.process_time(), time.perf_counter()
    time.sleep(seconds)
    return (time.process_time() - start_cpu) / (time.perf_counter() - start) * 1000


def bench_board(serial, seconds):
    monitor = DeviceMonitor(serial)
    try:
        deadline = time.monotonic() + 5
        while not monitor.snapshot()["connected"] and time.monotonic() < deadline:
            time.sleep(0.1)
        snapshot = monitor.snapshot()
        if not snapshot["connected"]:
            raise SystemExit(f"board did not connect: {snapshot['error']}")
        time.sleep(1)
        print(f"idle poll loop (board): {cpu_ms_per_second(seconds):.2f} ms CPU/s")
        if snapshot["hardware"].get("wireless_supported"):
            print(f"read_wireless() under the device lock: {per_call_us(monitor.read_wireless, 50) / 1000:.2f} ms")
    finally:
        monitor.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=float, default=5.0, help="idle CPU sample length")
    parser.add_argument("--board", action="store_true", help="also measure an attached board (read-only)")
    parser.add_argument("--serial", help="board serial number when several are attached")
    args = parser.parse_args()
    bench_host(args.seconds)
    if args.board:
        bench_board(args.serial, args.seconds)


if __name__ == "__main__":
    main()
