"""
Frame level ISO-TP checks. No can-isotp here, I build the frames by hand
so I can do things a well behaved stack wouldn't (wrong SN, no FC, ...).
"""
import time

import can
import pytest

from conftest import open_bus

REQ, RSP = 0x7E0, 0x7E8
PAD = 0xCC

# 22 F190 F195 F18C F186 0101 -> 50 byte response: FF + 7 CF
BIG_REQ = [0x22, 0xF1, 0x90, 0xF1, 0x95, 0xF1, 0x8C, 0xF1, 0x86, 0x01, 0x01]
BIG_RSP_LEN = 50


@pytest.fixture(scope="module")
def bus(server):
    b = open_bus(server)
    yield b
    b.shutdown()


@pytest.fixture(autouse=True)
def drain(bus):
    # let any half finished transfer from the previous test time out
    while bus.recv(0.05) is not None:
        pass
    yield


def send(bus, data):
    data = list(data) + [PAD] * (8 - len(data))
    bus.send(can.Message(arbitration_id=REQ, data=data, is_extended_id=False))


def recv_rsp(bus, timeout=0.5):
    """next frame on 0x7E8, skipping the 0x3A0 broadcasts"""
    end = time.time() + timeout
    while time.time() < end:
        m = bus.recv(max(0.0, end - time.time()))
        if m is not None and m.arbitration_id == RSP:
            return m
    return None


def send_sf(bus, payload):
    send(bus, [len(payload)] + list(payload))


def send_big_request(bus):
    """send BIG_REQ as FF + CF, honoring the server's FC"""
    send(bus, [0x10, len(BIG_REQ)] + BIG_REQ[:6])
    fc = recv_rsp(bus)
    assert fc is not None and fc.data[0] == 0x30
    send(bus, [0x21] + BIG_REQ[6:])
    return fc


def test_single_frame_padding(bus):
    send_sf(bus, [0x3E, 0x00])
    m = recv_rsp(bus)
    assert m.dlc == 8
    assert list(m.data) == [0x02, 0x7E, 0x00] + [PAD] * 5


def test_server_sends_fc_on_first_frame(bus):
    fc = send_big_request(bus)
    # CTS, BS=0 (no more FCs), STmin=0
    assert list(fc.data[:3]) == [0x30, 0x00, 0x00]
    ff = recv_rsp(bus)
    assert ff.data[0] >> 4 == 1
    assert ((ff.data[0] & 0x0F) << 8 | ff.data[1]) == BIG_RSP_LEN
    send(bus, [0x30, 0x00, 0x00])
    cfs = [recv_rsp(bus) for _ in range(7)]
    assert all(c is not None for c in cfs)
    assert [c.data[0] for c in cfs] == [0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27]


def test_server_honors_block_size(bus):
    send_big_request(bus)
    assert recv_rsp(bus).data[0] == 0x10        # FF

    send(bus, [0x30, 0x02, 0x00])               # BS=2
    got = [recv_rsp(bus).data[0], recv_rsp(bus).data[0]]
    assert got == [0x21, 0x22]
    # must stop and wait for the next FC
    assert recv_rsp(bus, 0.2) is None

    send(bus, [0x30, 0x03, 0x00])               # BS=3
    got = [recv_rsp(bus).data[0] for _ in range(3)]
    assert got == [0x23, 0x24, 0x25]
    assert recv_rsp(bus, 0.2) is None

    send(bus, [0x30, 0x00, 0x00])               # rest in one go
    got = [recv_rsp(bus).data[0] for _ in range(2)]
    assert got == [0x26, 0x27]


def test_server_handles_fc_wait(bus):
    send_big_request(bus)
    recv_rsp(bus)                               # FF
    for _ in range(3):
        send(bus, [0x31, 0x00, 0x00])           # FC.WAIT
        assert recv_rsp(bus, 0.1) is None
    send(bus, [0x30, 0x00, 0x00])
    assert recv_rsp(bus).data[0] == 0x21


def test_server_aborts_on_overflow(bus):
    send_big_request(bus)
    recv_rsp(bus)
    send(bus, [0x32, 0x00, 0x00])               # FC.OVFLW
    assert recv_rsp(bus, 0.3) is None
    # and is still alive
    send_sf(bus, [0x3E, 0x00])
    assert list(recv_rsp(bus).data[:3]) == [0x02, 0x7E, 0x00]


def test_server_respects_stmin(bus):
    send_big_request(bus)
    recv_rsp(bus)
    send(bus, [0x30, 0x00, 20])                 # 20 ms between CFs
    stamps = [recv_rsp(bus).timestamp for _ in range(7)]
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    assert min(gaps) >= 0.018


def test_n_bs_timeout_then_recovers(bus):
    send_big_request(bus)
    recv_rsp(bus)
    # no FC from us -> server should give up after N_Bs (1 s)
    time.sleep(1.2)
    send_sf(bus, [0x3E, 0x00])
    assert list(recv_rsp(bus).data[:3]) == [0x02, 0x7E, 0x00]


def test_wrong_sequence_number_dropped(bus):
    send(bus, [0x10, len(BIG_REQ)] + BIG_REQ[:6])
    assert recv_rsp(bus).data[0] == 0x30
    send(bus, [0x22] + BIG_REQ[6:])             # SN 2 instead of 1
    assert recv_rsp(bus, 0.3) is None
    send_sf(bus, [0x3E, 0x00])
    assert list(recv_rsp(bus).data[:3]) == [0x02, 0x7E, 0x00]


def test_stray_consecutive_frame_ignored(bus):
    send(bus, [0x21, 1, 2, 3, 4, 5, 6, 7])
    assert recv_rsp(bus, 0.2) is None


def test_sf_with_zero_length_ignored(bus):
    send(bus, [0x00, 0x3E, 0x00])
    assert recv_rsp(bus, 0.2) is None


# --- server rx side with a block size set (-b) ---

@pytest.fixture(scope="module")
def bs1_bus():
    import subprocess
    from conftest import SERVER, TRANSPORT, _free_udp_port
    from uds_client import make_bus

    if TRANSPORT != "udp":
        pytest.skip("needs a second server, only done on the udp bus")
    sp, cp = _free_udp_port(), _free_udp_port()
    proc = subprocess.Popen([SERVER, "-u", f"{sp}:{cp}", "-b", "1", "-m", "5", "-p", "0"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    b = make_bus(udp=f"{cp}:{sp}")
    yield b
    b.shutdown()
    proc.terminate()
    proc.wait(timeout=3)


def test_server_rx_block_size_1(bs1_bus):
    # 22 + 8 DIDs = 17 bytes -> FF(6) + CF(7) + CF(4)
    # with BS=1 the server must send a FC after the first CF too
    req = [0x22] + [0xF1, 0x95, 0xF1, 0x86, 0x01, 0x01, 0xF1, 0x8C] * 2
    send(bs1_bus, [0x10, len(req)] + req[:6])
    fc = recv_rsp(bs1_bus)
    assert list(fc.data[:3]) == [0x30, 0x01, 0x05]
    send(bs1_bus, [0x21] + req[6:13])
    fc = recv_rsp(bs1_bus)
    assert list(fc.data[:3]) == [0x30, 0x01, 0x05]
    send(bs1_bus, [0x22] + req[13:])
    ff = recv_rsp(bs1_bus)
    assert ff.data[0] >> 4 == 1 and ff.data[2] == 0x62
