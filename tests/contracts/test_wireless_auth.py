"""Compile the production BLE proof verifier against the pinned SDK crypto and compare independent HMAC vectors."""
import ctypes
import hashlib
import hmac
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_ble_proof_is_bound_to_nonce_board_and_every_command_byte(tmp_path):
    crypto = ROOT / "firmware/third_party/pico-sdk/lib/mbedtls"
    output = tmp_path / "wireless_auth.so"
    subprocess.run([
        "gcc", "-shared", "-fPIC", "-Werror", "-DMBEDTLS_CONFIG_FILE=\"pf_mbedtls_config.h\"",
        "-I", str(ROOT / "firmware/src/integration/wireless"),
        "-I", str(crypto / "include"), "-I", str(crypto / "library"),
        str(ROOT / "firmware/src/app/wireless/bluetooth/bluetooth_auth.c"),
        *(str(crypto / "library" / name) for name in ["md.c", "sha256.c", "platform_util.c"]),
        "-o", str(output),
    ], check=True, capture_output=True, text=True)
    verify = ctypes.CDLL(str(output)).pf_wireless_auth_verify
    verify.argtypes = [ctypes.c_char_p] * 5
    verify.restype = ctypes.c_bool
    key, nonce, uid = bytes(range(32)), bytes(range(16)), bytes(range(8))
    command = bytes([1, 7, 4, 0, 1, 32, 0, 0, 0])
    proof = hmac.new(key, nonce + uid + command, hashlib.sha256).digest()[:16]
    assert verify(key, nonce, uid, command, proof)
    inputs = [key, nonce, uid, command, proof]
    for field, data in enumerate(inputs):
        for index in range(len(data)):
            changed = bytearray(data)
            changed[index] ^= 1
            altered = inputs.copy()
            altered[field] = bytes(changed)
            assert not verify(*altered)


def test_ble_advertising_payload_outlives_start():
    """BTstack stores the advertising-data pointer; a stack buffer advertises garbage after start returns."""
    source = (ROOT / "firmware/src/app/wireless/bluetooth/bluetooth.c").read_text()
    start = source.split("bool pf_bluetooth_start(void)")[1]
    assert "static const uint8_t advertising[]" in start
    assert "gap_advertisements_set_data(sizeof(advertising), (uint8_t *)advertising)" in start
    assert "static uint8_t scan_response[2u + 10u + 2u * PICO_UNIQUE_BOARD_ID_SIZE_BYTES];" in source
    assert 'memcpy(scan_response + 2u, "PicoForge-", 10u);' in start
    assert "gap_scan_response_set_data(sizeof(scan_response), scan_response);" in start


def test_ble_without_control_key_is_diagnostics_only():
    source = (ROOT / "firmware/src/app/wireless/bluetooth/bluetooth.c").read_text()
    prepare = source.split("bool pf_bluetooth_prepare(void)")[1].split("uint8_t pf_bluetooth_state(void)")[0]
    authenticate = source.split("static bool authenticate(")[1].split("static int write_callback(")[0]
    proof_write = source.split("if (handle == BLE_HANDLE(0021))")[1].split("if (handle != BLE_HANDLE(0014))")[0]
    assert "control_key_available = key_bits != 0u;" in prepare
    assert "return (pf_boot_config_radios() & PF_WIRELESS_BLE) != 0u;" in prepare
    assert "!control_key_available" in authenticate
    assert "!control_key_available" in proof_write


def test_ble_rejected_proof_does_not_request_os_pairing():
    source = (ROOT / "firmware/src/app/wireless/bluetooth/bluetooth.c").read_text()
    assert "ATT_ERROR_INSUFFICIENT_AUTHENTICATION" not in source
    assert "#define BLE_ERROR_CONTROL_REJECTED 0x80" in source
    assert source.count("return BLE_ERROR_CONTROL_REJECTED;") == 2
