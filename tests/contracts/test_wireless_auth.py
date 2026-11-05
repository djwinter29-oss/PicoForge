"""Compile the production wireless proof verifier and compare independent HMAC vectors."""
import ctypes
import hashlib
import hmac
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.contracts


def test_wireless_proof_is_bound_to_nonce_board_protocol_and_command(tmp_path):
    crypto = ROOT / "firmware/third_party/pico-sdk/lib/mbedtls"
    sdk = ROOT / "firmware/third_party/pico-sdk"
    output = tmp_path / "wireless_auth.so"
    pico = tmp_path / "pico"
    pico.mkdir()
    (pico / "rand.h").write_text("#include <stdint.h>\nuint64_t get_rand_64(void);\n")
    (pico / "unique_id.h").write_text(
        "#include <stdint.h>\ntypedef struct { uint8_t id[8]; } pico_unique_board_id_t;\n"
        "void pico_get_unique_board_id(pico_unique_board_id_t *);\n")
    support = tmp_path / "platform.c"
    support.write_text("""
#include <stdint.h>
#include "pico/unique_id.h"
uint64_t get_rand_64(void) { return 0; }
void pico_get_unique_board_id(pico_unique_board_id_t *uid) {
    for (unsigned i = 0; i < 8; ++i) uid->id[i] = (uint8_t)i;
}
""")
    subprocess.run([
        "gcc", "-shared", "-fPIC", "-Werror", "-DMBEDTLS_CONFIG_FILE=\"pf_mbedtls_config.h\"",
        "-I", str(tmp_path),
        "-I", str(ROOT / "firmware/src"),
        "-I", str(ROOT / "firmware/src/integration/wireless"),
        "-I", str(sdk / "src/rp2_common/pico_rand/include"),
        "-I", str(sdk / "src/rp2_common/pico_unique_id/include"),
        "-I", str(crypto / "include"), "-I", str(crypto / "library"),
        str(ROOT / "firmware/src/app/wireless/wireless_auth.c"), str(support),
        *(str(crypto / "library" / name) for name in ["md.c", "sha256.c", "platform_util.c"]),
        "-o", str(output),
    ], check=True, capture_output=True, text=True)
    library = ctypes.CDLL(str(output))
    set_key = library.pf_wireless_auth_set_key
    set_key.argtypes = [ctypes.c_char_p]
    verify = library.pf_wireless_auth_verify
    verify.argtypes = [ctypes.c_char_p] * 4
    verify.restype = ctypes.c_bool
    tcp_cli_proof = library.pf_wireless_auth_tcp_cli_proof
    tcp_cli_proof.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    tcp_cli_proof.restype = ctypes.c_bool
    available = library.pf_wireless_auth_available
    available.restype = ctypes.c_bool
    key, nonce, uid = bytes(range(32)), bytes(range(16)), bytes(range(8))
    command = bytes([1, 7, 4, 0, 1, 32, 0, 0, 0])
    domain = b"PFBLECTL1"
    proof = hmac.new(key, nonce + uid + domain + command, hashlib.sha256).digest()[:16]
    set_key(key)
    assert verify(nonce, domain, command, proof)
    session_vector = hmac.new(key, nonce + uid + domain + b"SESSION01", hashlib.sha256).digest()[:16]
    request_vector = bytes.fromhex("010702010000000000")
    command_vector = hmac.new(key, nonce + uid + domain + request_vector, hashlib.sha256).digest()[:16]
    assert session_vector.hex() == "7c583e03087e013dc444f219fa69cfe4"
    assert command_vector.hex() == "c090cfe4b5d641b4dd4f8d710c7dad6d"
    document = (ROOT / "docs/reference/bluetooth.md").read_text()
    assert session_vector.hex() in document and command_vector.hex() in document
    tcp_nonce = bytes(range(16, 32))
    tcp_proof = ctypes.create_string_buffer(16)
    assert tcp_cli_proof(tcp_nonce, tcp_proof)
    expected_tcp_proof = hmac.new(
        key, tcp_nonce + uid + b"PFTCPCLI1" + b"TCPCLI001", hashlib.sha256
    ).digest()[:16]
    assert tcp_proof.raw == expected_tcp_proof
    inputs = [nonce, domain, command, proof]
    for field, data in enumerate(inputs):
        for index in range(len(data)):
            changed = bytearray(data)
            changed[index] ^= 1
            altered = inputs.copy()
            altered[field] = bytes(changed)
            assert not verify(*altered)
    set_key(bytes(32))
    assert available()
    assert not verify(nonce, domain, command, proof)
    zero_proof = hmac.new(bytes(32), nonce + uid + domain + command, hashlib.sha256).digest()[:16]
    assert verify(nonce, domain, command, zero_proof)
    assert tcp_cli_proof(tcp_nonce, tcp_proof)
    assert tcp_proof.raw == hmac.new(
        bytes(32), tcp_nonce + uid + b"PFTCPCLI1" + b"TCPCLI001", hashlib.sha256
    ).digest()[:16]
    set_key(None)
    assert not available()
    assert not verify(nonce, domain, command, zero_proof)
    assert not tcp_cli_proof(tcp_nonce, tcp_proof)


def test_ble_application_auth_is_separate_from_nonbonding_smp():
    source = (ROOT / "firmware/src/app/wireless/bluetooth/bluetooth.c").read_text()
    assert "ATT_ERROR_INSUFFICIENT_AUTHENTICATION" not in source
    assert "#define BLE_ERROR_CONTROL_REJECTED 0x80" in source
    assert "#define BLE_ERROR_RATE_LIMITED 0x82" in source
    assert "#define BLE_ERROR_CHALLENGE_UNAVAILABLE 0x83" in source
    assert "sm_set_authentication_requirements(SM_AUTHREQ_NO_BONDING);" in source
