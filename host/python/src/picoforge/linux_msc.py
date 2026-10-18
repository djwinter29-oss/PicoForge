"""Safely release only the USB disk belonging to the connected PicoForge board."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys

_BLOCKS = Path("/sys/class/block")


def _run(arguments: list[str]) -> str:
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=30, check=False)
    except FileNotFoundError as error:
        raise RuntimeError(
            f"Automatic USB eject requires {arguments[0]}; install it or safely eject manually."
        ) from error
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("Linux USB eject timed out; storage remains USB-owned. Check for open files.") from error
    if result.returncode != 0:
        reason = result.stderr.strip() or result.stdout.strip() or f"exit status {result.returncode}"
        raise RuntimeError(f"Linux USB eject failed: {reason}. Close open files or safely eject manually.")
    return result.stdout


def _board_disk(serial_number: str) -> Path:
    matches = []
    for block in _BLOCKS.iterdir():
        if (block / "partition").exists():
            continue
        for parent in block.resolve().parents:
            if not (parent / "idVendor").exists():
                continue
            if ((parent / "idVendor").read_text().strip().lower() == "cafe"
                    and (parent / "idProduct").read_text().strip().lower() in {"4011", "4012"}
                    and (parent / "serial").read_text().strip() == serial_number):
                matches.append(Path("/dev") / block.name)
            break
    if len(matches) != 1:
        raise RuntimeError(
            f"Expected exactly one PicoForge USB disk for serial {serial_number}, found {len(matches)}. "
            "Safely eject manually before disabling."
        )
    return matches[0]


def eject_msc(serial_number: str) -> None:
    """Unmount the selected board's filesystems, then send UDisks' safe SCSI eject.

    No sudo, forced unmount, power-off, formatting, or whole-machine sync is used.
    Authentication is noninteractive: permission failures must be presented to the user.
    """
    if not sys.platform.startswith("linux"):
        raise RuntimeError("Automatic USB eject is Linux-only; safely eject in the host OS before disabling.")
    if not serial_number:
        raise RuntimeError("Cannot safely identify the USB disk without the connected board's serial number.")
    disk = _board_disk(serial_number)
    try:
        devices = json.loads(_run(["lsblk", "--json", "--paths", "--output", "NAME,MOUNTPOINTS", str(disk)]))
    except json.JSONDecodeError as error:
        raise RuntimeError("Linux returned invalid USB mount information; storage remains USB-owned.") from error

    def unmount(device: dict) -> None:
        for child in device.get("children", []):
            unmount(child)
        node = Path(device["name"])
        # Accept only the selected whole disk or its direct sysfs partitions.
        if node != disk and (
            node.parent != Path("/dev")
            or not (_BLOCKS / node.name / "partition").exists()
            or (_BLOCKS / node.name).resolve().parent.name != disk.name
        ):
            raise RuntimeError("Unexpected USB block-device topology; safely eject manually.")
        if any(device.get("mountpoints") or []):
            _run(["udisksctl", "unmount", "--block-device", str(node), "--no-user-interaction"])

    for device in devices["blockdevices"]:
        unmount(device)
    info = _run(["udisksctl", "info", "--block-device", str(disk)])
    drive = re.search(r"^\s*Drive:\s*'(/org/freedesktop/UDisks2/drives/[A-Za-z0-9_]+)'\s*$", info, re.MULTILINE)
    if drive is None:
        raise RuntimeError("Linux did not identify the USB drive for safe eject; storage remains USB-owned.")
    _run([
        "gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2",
        "--object-path", drive[1], "--method", "org.freedesktop.UDisks2.Drive.Eject",
        "{'auth.no_user_interaction': <true>}",
    ])
