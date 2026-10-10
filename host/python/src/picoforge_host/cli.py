"""CLI for PicoForge HID status and its local dashboard."""

from __future__ import annotations

import argparse
import json
import sys

from .device import PicoForgeHid


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="picoforge-host")
    commands = parser.add_subparsers(dest="command", required=True)
    status = commands.add_parser("status", help="read hardware and runtime status")
    web = commands.add_parser("web", help="serve the local status dashboard")
    for command in (status, web):
        selection = command.add_mutually_exclusive_group()
        selection.add_argument("--serial", help="select a board by USB serial number")
        selection.add_argument("--device-path", help="select a HID path")
    web.add_argument("--port", type=int, default=8765)
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
            info = device.read_info()
            runtime = device.read_status(timeout_ms=1500)
            print(json.dumps({"connected": True, "hardware": info, "runtime": runtime}, sort_keys=True))
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1