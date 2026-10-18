"""Keep the DCD interposition surface aligned with the pinned Pico TinyUSB driver."""
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_usb_startup_detach_delay_does_not_require_scheduler_or_alarm_wakeup():
    source = (ROOT / "firmware/src/app/usb/usb_task.c").read_text()
    startup = source.split("void pf_usb_start(void)", 1)[1]
    assert startup.index("tud_disconnect();") < startup.index("busy_wait_ms(USB_RECONNECT_DETACH_MS);")
    assert startup.index("busy_wait_ms(USB_RECONNECT_DETACH_MS);") < startup.index("tud_connect();")
    assert "sleep_ms(" not in startup
    assert "vTaskDelay(" not in startup


def test_dcd_gate_covers_every_pinned_driver_entry_and_shared_irq():
    source = ROOT / "firmware/third_party/pico-sdk/lib/tinyusb/src"
    driver = (source / "portable/raspberrypi/rp2040/dcd_rp2040.c").read_text()
    entries = set(re.findall(
        r"^(?:void|bool) (?:__tusb_irq_path_func\()?(dcd_\w+)\)?\(", driver, re.MULTILINE))
    assert len(entries) == 18
    guard = (ROOT / "firmware/src/integration/usb/dcd_guard.c").read_text()
    guarded = set(re.findall(r"PF_DCD_(?:BOOL|VOID)\((dcd_\w+),", guard))
    guarded.update(re.findall(r"\b__wrap_(dcd_\w+)\(", guard))
    assert guarded == entries | {"dcd_event_handler"}
    cmake = (ROOT / "firmware/CMakeLists.txt").read_text()
    wraps = cmake[cmake.index("foreach(symbol IN ITEMS"):].split("endforeach()", 1)[0]
    assert set(re.findall(r"\bdcd_\w+\b", wraps)) == entries | {"dcd_event_handler"}
    assert "tud_task_ext" in wraps
    assert "__wrap_tud_task_ext(" in guard
    assert "tud_task_event_ready" in wraps
    assert "__wrap_tud_task_event_ready(" in guard
    for operation in ("add_shared", "remove"):
        symbol = f"irq_{operation}_handler"
        assert symbol in wraps
        assert f"__wrap_{symbol}(" in guard
    assert "irq_add_shared_handler(USBCTRL_IRQ, dcd_rp2040_irq," in driver
    assert "irq_remove_handler(USBCTRL_IRQ, dcd_rp2040_irq)" in driver
    assert "CFG_TUSB_OS != OPT_OS_PICO" in guard
    osal = (source / "osal/osal_pico.h").read_text()
    for operation in ("send", "receive"):
        body = osal.split(f"bool osal_queue_{operation}(", 1)[1].split("\n}", 1)[0]
        assert "critical_section_enter_blocking(" in body
        assert "critical_section_exit(" in body
    config = (ROOT / "firmware/src/integration/usb/tusb_config.h").read_text()
    assert "#define CFG_TUD_TASK_QUEUE_SZ 16" in config
    usbd = (source / "device/usbd.c").read_text()
    assert "void dcd_event_handler(dcd_event_t const* event, bool in_isr)" in usbd
    assert "void tud_task_ext(uint32_t timeout_ms, bool in_isr)" in usbd
