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
reset = tinyusb[tinyusb.index('bool mscd_control_xfer_cb('):tinyusb.index('case MSC_REQ_GET_MAX_LUN:')]
assert reset.index('stage != CONTROL_STAGE_SETUP') < reset.index('case MSC_REQ_RESET:')
assert reset.index('TU_VERIFY(request->wValue == 0 && request->wLength == 0)') < reset.index('proc_bot_reset(p_msc)') < reset.index('tud_control_status(')
usbd = (ROOT / 'firmware/third_party/pico-sdk/lib/tinyusb/src/device/usbd.c').read_text()
assert '.reset            = mscd_reset' in usbd
assert '.control_xfer_cb  = mscd_control_xfer_cb' in usbd
queue = usbd[usbd.index('static inline bool queue_event('):usbd.index('// Prototypes')]
assert queue.index('osal_queue_send(') < queue.index('tud_event_hook_cb(')
handled = usbd[usbd.index('if (!osal_queue_receive('):]
assert handled.index('case DCD_EVENT_BUS_RESET:') < handled.index('usbd_reset(event.rhport)')
cmake = (ROOT / 'firmware/CMakeLists.txt').read_text()
for symbol in ('mscd_reset', 'mscd_control_xfer_cb'):
    assert f'"LINKER:--wrap={symbol}"' in cmake
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
        if name == 'msc':
            args += [str(ROOT / 'tools/validation/host/msc_reset_model.c'),
                     str(ROOT / 'tools/validation/host/msc_reset_queue.c'),
                     '-Wl,--wrap=mscd_reset', '-Wl,--wrap=mscd_control_xfer_cb']
        if name == 'spi_write':
            args += [str(ROOT / 'firmware/src/drivers/sd_card/sd_card_protocol.c'),
                     str(ROOT / 'firmware/src/drivers/sd_card/spi/sd_spi_protocol.c')]
        subprocess.run(args + ['-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
