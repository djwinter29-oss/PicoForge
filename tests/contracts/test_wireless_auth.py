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
        str(ROOT / "firmware/src/app/wireless/wireless_auth.c"),
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
