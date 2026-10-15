#!/usr/bin/env python3
"""Run data-bearing MSC callbacks through the production SD facade and diskio."""
import os
from importlib.machinery import SourceFileLoader
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
shims = SourceFileLoader('serialization', str(ROOT / 'tools/validation/check-sd-serialization.py')).load_module().SHIMS


def main():
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        for name, body in shims.items():
            p = d / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(body)
        includes = [d, ROOT / 'tools/validation/host/stubs', ROOT / 'firmware/src',
                    ROOT / 'firmware/src/drivers/sd_card',
                    ROOT / 'firmware/src/integration/fatfs', ROOT / 'firmware/third_party/fatfs/source']
        source = (ROOT / 'tools/validation/host/regression_msc_batch.c').read_text()
        # Keep relative production includes valid when placing the variant in tmp.
        for relative in ('usb/msc.c', 'integration/fatfs/diskio.c'):
            source = source.replace('../../../firmware/src/' + relative,
                                    str(ROOT / 'firmware/src' / relative))
        for transport in (0, 1):
            variant = source
            if transport == 1:
                variant = variant.replace('sdio_pio/sdio_pio.h', 'spi/sd_spi.h')
                variant = variant.replace('pf_sdio_pio_', 'pf_sd_spi_')
                variant = variant.replace('PF_SD_CARD_TRANSPORT_SDIO_PIO', 'PF_SD_CARD_TRANSPORT_SPI')
            (d / 'test.c').write_text(variant)
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_XOPEN_SOURCE=700',
                            f'-DPF_CONFIG_SD_CARD_TRANSPORT={transport}',
                            '-Wall', '-Wextra', '-Werror', '-pthread',
                            *['-I' + str(p) for p in includes], str(d / 'test.c'),
                            str(ROOT / 'firmware/src/drivers/sd_card/sd_card.c'),
                            '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True, timeout=15)
            print('Data-bearing facade transport:', 'SPI' if transport else 'SDIO')


if __name__ == '__main__':
    main()
