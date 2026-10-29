import time

import pytest

NEG = 0x7F


def nrc(rsp):
    assert rsp is not None, "no response"
    assert rsp[0] == NEG, f"expected NRC, got {rsp.hex(' ')}"
    return rsp[2]


# --- 0x10 DiagnosticSessionControl ---

def test_default_session(client):
    rsp = client.request([0x10, 0x01])
    # P2 = 50 ms, P2* = 500 * 10 ms
    assert rsp == bytes([0x50, 0x01, 0x00, 0x32, 0x01, 0xF4])


def test_extended_session(client):
    assert client.request([0x10, 0x03])[:2] == bytes([0x50, 0x03])
    assert client.request([0x22, 0xF1, 0x86]) == bytes([0x62, 0xF1, 0x86, 0x03])


def test_programming_needs_extended_first(client):
    assert nrc(client.request([0x10, 0x02])) == 0x22
    client.request([0x10, 0x03])
    assert client.request([0x10, 0x02])[:2] == bytes([0x50, 0x02])


def test_session_bad_subfunction(client):
    assert nrc(client.request([0x10, 0x05])) == 0x12


def test_session_bad_length(client):
    assert nrc(client.request([0x10])) == 0x13
    assert nrc(client.request([0x10, 0x03, 0x00])) == 0x13


def test_session_suppress_positive_response(client):
    client.stack.send(bytes([0x10, 0x83]))
    assert client.wait_response(0.3) is None
    # still switched, just quietly
    assert client.request([0x22, 0xF1, 0x86])[-1] == 0x03


# --- 0x3E TesterPresent ---

def test_tester_present(client):
    assert client.request([0x3E, 0x00]) == bytes([0x7E, 0x00])


def test_tester_present_suppressed(client):
    client.stack.send(bytes([0x3E, 0x80]))
    assert client.wait_response(0.3) is None


def test_tester_present_bad_subfunction(client):
    assert nrc(client.request([0x3E, 0x01])) == 0x12


def test_tester_present_bad_length(client):
    assert nrc(client.request([0x3E])) == 0x13


# --- 0x22 ReadDataByIdentifier ---

def test_read_vin_multiframe(client):
    rsp = client.request([0x22, 0xF1, 0x90])
    assert rsp[:3] == bytes([0x62, 0xF1, 0x90])
    assert rsp[3:] == b"TESTVIN0000000042"
    assert len(rsp) > 7     # so it really went FF + CFs


def test_read_sw_version(client):
    assert client.request([0x22, 0xF1, 0x95]) == b"\x62\xF1\x95" + b"0.3.1"


def test_read_ecu_serial(client):
    assert client.request([0x22, 0xF1, 0x8C]) == b"\x62\xF1\x8C" + b"TJP-ECU-000123"


def test_read_battery_voltage(client):
    rsp = client.request([0x22, 0x01, 0x01])
    assert rsp[:3] == bytes([0x62, 0x01, 0x01])
    volts = int.from_bytes(rsp[3:5], "big") / 100
    assert 12.40 <= volts <= 12.80


def test_read_multiple_dids(client):
    # 11 byte request, so this also tests the server's multi-frame rx
    rsp = client.request([0x22, 0xF1, 0x90, 0xF1, 0x95, 0xF1, 0x8C, 0xF1, 0x86, 0x01, 0x01])
    assert rsp.startswith(b"\x62\xF1\x90TESTVIN0000000042\xF1\x950.3.1\xF1\x8CTJP-ECU-000123")
    assert rsp[-7:-2] == bytes([0xF1, 0x86, 0x01, 0x01, 0x01])


def test_read_unknown_did(client):
    assert nrc(client.request([0x22, 0x12, 0x34])) == 0x31


def test_read_mix_known_unknown(client):
    # unknown ones are skipped, only NRC when nothing matched
    assert client.request([0x22, 0x12, 0x34, 0xF1, 0x95]) == b"\x62\xF1\x950.3.1"


@pytest.mark.parametrize("req", [[0x22], [0x22, 0xF1], [0x22, 0xF1, 0x90, 0xF1]])
def test_read_bad_length(client, req):
    assert nrc(client.request(req)) == 0x13


# --- 0x11 ECUReset ---

def test_reset_not_in_default_session(client):
    assert nrc(client.request([0x11, 0x01])) == 0x7F


def test_hard_reset(client):
    client.request([0x10, 0x03])
    before = client.status_frames(0.25)[-1].data[4]
    assert client.request([0x11, 0x01]) == bytes([0x51, 0x01])
    # back in default after reset
    assert client.request([0x22, 0xF1, 0x86])[-1] == 0x01
    client.flush_status()
    after = client.status_frames(0.25)[-1].data[4]
    assert after == (before + 1) & 0xFF


def test_soft_reset(client):
    client.request([0x10, 0x03])
    assert client.request([0x11, 0x03]) == bytes([0x51, 0x03])


def test_reset_unsupported_type(client):
    client.request([0x10, 0x03])
    assert nrc(client.request([0x11, 0x02])) == 0x12


# --- misc ---

@pytest.mark.parametrize("sid", [0x27, 0x2E, 0x31, 0x19], ids=lambda s: f"0x{s:02X}")
def test_unknown_service(client, sid):
    assert nrc(client.request([sid, 0x01])) == 0x11


def test_functional_tester_present(client):
    client.send_functional([0x3E, 0x00])
    assert client.wait_response(0.5) == bytes([0x7E, 0x00])


def test_functional_nrc_suppressed(client):
    # 0x11 serviceNotSupported must not be sent for functional requests
    client.send_functional([0x27, 0x01])
    assert client.wait_response(0.3) is None
    time.sleep(0.05)
