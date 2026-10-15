#!/usr/bin/env python3
"""Compile complete production translation units with host hardware seams."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    for name in ('spi_write', 'msc'):
        test = ROOT / f'tools/validation/host/regression_{name}.c'
        if not test.exists():
            continue
        exe = Path(tmp) / name
        args = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'tools/validation/host/stubs'),
                '-I' + str(ROOT / 'firmware/src'),
                '-I' + str(ROOT / 'firmware/src/drivers/sd_card'), str(test)]
        if name == 'spi_write':
            args += [str(ROOT / 'firmware/src/drivers/sd_card/sd_card_protocol.c'),
                     str(ROOT / 'firmware/src/drivers/sd_card/spi/sd_spi_protocol.c')]
        subprocess.run(args + ['-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
