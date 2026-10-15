#!/usr/bin/env python3
"""Validate final ARM ELF MSC pointers only; no builds or host regressions.

Paths and SHA-256 identify artifacts, not their transport provenance.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

from msc_elf import check_artifacts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', nargs='+', type=Path, help='ELF artifact(s) with DWARF')
    args = parser.parse_args()
    try:
        check_artifacts(args.elf)
    except (OSError, ValueError, KeyError, TypeError, AttributeError,
            struct.error, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
