"""Keep scheduling unrestricted except for the SDK radio owner's initialization-core requirement."""
from pathlib import Path
import re
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize("ndebug", [False, True])
def test_config_assert_evaluates_once_and_fails_closed(tmp_path, ndebug):
    harness = tmp_path / "assert.c"
    harness.write_text('''
#include <stdlib.h>
#include "FreeRTOSConfig.h"
void hard_assertion_failure(void) { _Exit(73); }
int main(int argc, char **argv) {
    (void)argv;
    int evaluated = 0;
    configASSERT(++evaluated == 1);
    if (evaluated != 1) return 1;
    if (argc > 1) configASSERT(0);
    return 0;
}
''')
    binary = tmp_path / "assert"
    subprocess.run([
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", *(["-DNDEBUG"] if ndebug else []),
        "-I", str(ROOT / "firmware/src/integration/freertos"),
        "-I", str(ROOT / "firmware/third_party/pico-sdk/src/common/pico_base_headers/include"),
        str(harness), "-o", str(binary),
    ], check=True)
    assert subprocess.run([str(binary)]).returncode == 0
    failed = subprocess.run([str(binary), "fail"], capture_output=True)
    assert failed.returncode == (73 if ndebug else -6)


def test_two_core_configuration_has_one_authority_and_only_sdk_radio_affinity():
    config = (ROOT / "firmware/src/integration/freertos/FreeRTOSConfig.h").read_text()
    for key, value in (
        ("configNUMBER_OF_CORES", 2),
        ("configUSE_CORE_AFFINITY", 1),
        ("configIDLE_AFFINITY", 1),
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
        if path == ROOT / "firmware/src/app/wireless/wireless.c":
            source = path.read_text()
            start = source[source.index("void pf_wireless_start(void)"):]
            assert start.index("if (!wifi_enabled && !ble_enabled) return;") < start.index("vTaskCoreAffinitySet(")
            assert start.index("vTaskCoreAffinitySet(") < start.index("cyw43_arch_init(")
            assert source.count("vTaskCoreAffinitySet(") == 1
            assert "vTaskCoreAffinitySet(NULL, (UBaseType_t)1u << get_core_num());" in start
            continue
        assert not re.search(r"\b(?:vTaskCoreAffinitySet|xTaskCreateStaticAffinitySet|"
                             r"xTaskCreateAffinitySet|multicore_launch_core1)\s*\(", path.read_text()), path
