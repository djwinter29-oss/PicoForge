#!/usr/bin/env python3
"""Compile complete production translation units with host hardware seams."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
# Keep the callback-boundary model honest about the pinned TinyUSB ordering.
# This inspects source; it does not execute the USB BOT state machine.
tinyusb = (ROOT / 'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c').read_text()
for operation in ('read', 'write'):
    callback = tinyusb.index(f'= tud_msc_{operation}10_cb(')
    failed = tinyusb.index('if (nbytes < 0)', callback)
    overwrite = tinyusb.index('set_sense_medium_not_present(p_cbw->lun);', failed)
    finish = tinyusb.index('fail_scsi_op(', failed)
    assert callback < failed < overwrite < finish
request = tinyusb[tinyusb.index('case SCSI_CMD_REQUEST_SENSE:'):]
assert request.index('tu_memcpy_s(buffer') < request.index('resplen = tud_msc_request_sense_cb(') < request.index('tud_msc_set_sense(lun, 0, 0, 0)')
print('Pinned TinyUSB source ordering checked (callback-boundary tests, not BOT integration)')
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
