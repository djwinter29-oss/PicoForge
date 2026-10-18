#!/usr/bin/env python3
"""Test pinned TinyUSB source ordering and the MSC linker-wrapper contract."""
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts

def check_contract(root):
    # Keep the callback-boundary model honest about the pinned TinyUSB ordering.
    # This inspects source; it does not execute the USB BOT state machine.
    tinyusb = (root / 'firmware/third_party/pico-sdk/lib/tinyusb/src/class/msc/msc_device.c').read_text()
    for operation in ('read', 'write'):
        callback = tinyusb.index(f'= tud_msc_{operation}10_cb(')
        failed = tinyusb.index('if (nbytes < 0)', callback)
        overwrite = tinyusb.index('set_sense_medium_not_present(p_cbw->lun);', failed)
        finish = tinyusb.index('fail_scsi_op(', failed)
        assert callback < failed < overwrite < finish
    # No-data vendor SCSI commands receive total_len=0, not endpoint capacity.
    command = tinyusb[tinyusb.index('int32_t resplen = proc_builtin_scsi('):]
    assert command.index('(resplen < 0) && (p_msc->sense_key == 0)') < command.index('resplen = tud_msc_scsi_cb(')
    assert '(uint16_t)p_msc->total_len' in command[:command.index('if (resplen < 0) {')]
    for opcode, callback_name in (
        ('START_STOP_UNIT', 'start_stop'),
        ('PREVENT_ALLOW_MEDIUM_REMOVAL', 'prevent_allow_medium_removal'),
    ):
        builtin = tinyusb[tinyusb.index(f'case SCSI_CMD_{opcode}:'):]
        assert builtin.index(f'tud_msc_{callback_name}_cb(') < builtin.index('if (p_msc->sense_key == 0)') < builtin.index('set_sense_medium_not_present(lun);')
    completed = tinyusb[tinyusb.index('case MSC_STAGE_STATUS_SENT:'):]
    assert completed.index('xferred_bytes == sizeof(msc_csw_t)') < completed.index('tud_msc_write10_complete_cb(p_cbw->lun)')
    assert 'tu_min32(CFG_TUD_MSC_EP_BUFSIZE, p_cbw->total_bytes - p_msc->xferred_len)' in tinyusb
    write = tinyusb[tinyusb.index('= tud_msc_write10_cb('):]
    assert 'xferred_bytes' in write[:write.index('if (nbytes < 0)')]
    config = (root / 'firmware/src/integration/usb/tusb_config.h').read_text()
    assert '#define CFG_TUD_MSC_EP_BUFSIZE 512' in config
    request = tinyusb[tinyusb.index('case SCSI_CMD_REQUEST_SENSE:'):]
    assert request.index('tu_memcpy_s(buffer') < request.index('resplen = tud_msc_request_sense_cb(') < request.index('tud_msc_set_sense(lun, 0, 0, 0)')
    reset = tinyusb[tinyusb.index('bool mscd_control_xfer_cb('):tinyusb.index('case MSC_REQ_GET_MAX_LUN:')]
    assert reset.index('stage != CONTROL_STAGE_SETUP') < reset.index('case MSC_REQ_RESET:')
    assert reset.index('TU_VERIFY(request->wValue == 0 && request->wLength == 0)') < reset.index('proc_bot_reset(p_msc)') < reset.index('tud_control_status(')
    usbd = (root / 'firmware/third_party/pico-sdk/lib/tinyusb/src/device/usbd.c').read_text()
    assert '.reset            = mscd_reset' in usbd
    assert '.control_xfer_cb  = mscd_control_xfer_cb' in usbd
    queue = usbd[usbd.index('static inline bool queue_event('):usbd.index('// Prototypes')]
    assert queue.index('osal_queue_send(') < queue.index('tud_event_hook_cb(')
    handled = usbd[usbd.index('if (!osal_queue_receive('):]
    assert handled.index('case DCD_EVENT_BUS_RESET:') < handled.index('usbd_reset(event.rhport)')
    cmake = (root / 'firmware/CMakeLists.txt').read_text()
    for symbol in ('mscd_reset', 'mscd_control_xfer_cb'):
        assert f'"LINKER:--wrap={symbol}"' in cmake
    print('Pinned TinyUSB source ordering checked (callback-boundary tests, not BOT integration)')


def test_tinyusb_msc_contract():
    check_contract(ROOT)
