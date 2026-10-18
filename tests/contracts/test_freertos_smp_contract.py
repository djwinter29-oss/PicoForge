"""Keep application scheduling unrestricted while retaining SDK flash-safe execution support."""
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_two_core_configuration_has_one_authority_and_no_application_affinity():
    config = (ROOT / "firmware/src/integration/freertos/FreeRTOSConfig.h").read_text()
    for key, value in (
        ("configNUMBER_OF_CORES", 2),
        ("configUSE_CORE_AFFINITY", 1),
        ("configRUN_MULTIPLE_PRIORITIES", 1),
        ("configUSE_PASSIVE_IDLE_HOOK", 0),
        ("INCLUDE_vTaskDelete", 1),
        ("INCLUDE_vTaskPrioritySet", 1),
    ):
        assert re.search(rf"^#define {key} {value}$", config, re.MULTILINE)
    cmake = (ROOT / "firmware/CMakeLists.txt").read_text()
    assert "configNUMBER_OF_CORES=" not in cmake
    kernel = (ROOT / "firmware/third_party/FreeRTOS-Kernel/include/FreeRTOS.h").read_text()
    assert re.search(r"#define configTASK_DEFAULT_CORE_AFFINITY\s+tskNO_AFFINITY", kernel)
    for path in (ROOT / "firmware/src").rglob("*.c"):
        assert not re.search(r"\b(?:vTaskCoreAffinitySet|xTaskCreateStaticAffinitySet|"
                             r"xTaskCreateAffinitySet|multicore_launch_core1)\s*\(", path.read_text()), path
