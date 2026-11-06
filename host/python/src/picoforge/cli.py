# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""CLI for PicoForge HID status and its local dashboard."""

from __future__ import annotations

import argparse
import json
import sys
from math import ceil
from time import monotonic

from .hid.device import PicoForgeHid
from .reports.models import MemoryStatus, RuntimeStatus, SelfTestStatus

_STATUS_REPORT_TIMEOUT_SECONDS = 5.0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="picoforge")
    commands = parser.add_subparsers(dest="command", required=True)
    status = commands.add_parser("status", help="read hardware and runtime status")
    web = commands.add_parser("web", help="serve the local status dashboard")
    reboot = commands.add_parser("reboot", help="reboot the selected board")
    reboot.add_argument("--confirm", action="store_true", required=True,
                        help="confirm that the board should reboot")
    for command in (status, web, reboot):
        selection = command.add_mutually_exclusive_group()
        selection.add_argument("--serial", help="select a board by USB serial number")
        selection.add_argument("--device-path", help="select a HID path")
    web.add_argument("--port", type=int, default=5000)
    return parser.parse_args(argv)


def _read_status_reports(
    device: PicoForgeHid, self_test_expected: bool
) -> tuple[RuntimeStatus | None, MemoryStatus | None, SelfTestStatus | None]:
    runtime = None
    memory = None
    self_test = None
    deadline = monotonic() + _STATUS_REPORT_TIMEOUT_SECONDS
    while True:
        remaining = deadline - monotonic()
        if remaining <= 0:
            break
        item = device.read_input_report(timeout_ms=max(1, ceil(remaining * 1000)))
        if item is None:
            break
        if item["kind"] == "runtime":
            runtime = item["data"]
        elif item["kind"] == "memory":
            memory = item["data"]
        elif item["kind"] == "self_test":
            self_test = item["data"]
        if runtime is not None and memory is not None and (
            self_test is not None or not self_test_expected
        ):
            break
    return runtime, memory, self_test


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "web":
            if not 1 <= args.port <= 65535:
                raise ValueError("port must be between 1 and 65535")
            from .dashboard.server import run_server

            run_server(args.port, args.serial, args.device_path)
            return 0

        with PicoForgeHid(args.serial, args.device_path) as device:
            if args.command == "reboot":
                device.read_info()
                result = device.reboot()
                print(json.dumps({"control_result": result}, sort_keys=True))
                return 0 if result["status"] == "completed" else 1
            info = device.read_info()
            if not info.get("supported", True):
                print(json.dumps({
                    "connected": True,
                    "hardware": info,
                    "settings": None,
                    "board_configuration": None,
                    "runtime": None,
                    "memory": None,
                    "msc": None,
                    "self_test": None,
                }, sort_keys=True))
                return 1
            configuration = device.read_configuration()
            settings = {
                key: configuration[key]
                for key in ("system_clock_mhz", "sdcard_profile", "radios", "tcp_cli_enabled")
            }
            runtime, memory, self_test = _read_status_reports(device, info["self_test_supported"])
            print(json.dumps({
                "connected": True,
                "hardware": info,
                "settings": settings,
                "board_configuration": configuration,
                "runtime": runtime,
                "memory": memory,
                "msc": memory["msc"] if memory is not None else None,
                "self_test": self_test,
            }, sort_keys=True))
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())