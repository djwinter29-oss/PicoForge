"""CLI for PicoForge HID status and its local dashboard."""

from __future__ import annotations

import argparse
import json
import sys

from .device import PicoForgeHid


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


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "web":
            if not 1 <= args.port <= 65535:
                raise ValueError("port must be between 1 and 65535")
            from .web import run_server

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
                "system_clock_mhz": configuration["system_clock_mhz"],
                "sdcard_profile": configuration["sdcard_profile"],
            }
            runtime = None
            memory = None
            self_test = None
            self_test_expected = info["self_test_supported"]
            for _ in range(16):
                item = device.read_input_report(timeout_ms=1500)
                if item is None:
                    continue
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