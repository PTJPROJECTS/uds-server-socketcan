import time

import cantools
import pytest

from conftest import S3_MS
from uds_client import DBC_PATH


@pytest.fixture(scope="module")
def db():
    return cantools.database.load_file(DBC_PATH)


def active_session(client):
    return client.request([0x22, 0xF1, 0x86])[-1]


def test_s3_timeout_drops_to_default(client, db):
    client.request([0x10, 0x03])
    assert active_session(client) == 0x03

    time.sleep(S3_MS / 1000 + 0.3)
    client.flush_status()
    frames = client.status_frames(0.25)
    sig = db.decode_message(frames[-1].arbitration_id, frames[-1].data)
    assert sig["ActiveSession"] == "Default"
    assert active_session(client) == 0x01


def test_tester_present_keeps_session(client):
    client.request([0x10, 0x03])
    end = time.time() + 2.5 * S3_MS / 1000
    while time.time() < end:
        client.stack.send(bytes([0x3E, 0x80]))
        time.sleep(S3_MS / 1000 / 3)
    assert active_session(client) == 0x03


def test_any_request_restarts_s3(client):
    client.request([0x10, 0x03])
    for _ in range(4):
        time.sleep(S3_MS / 1000 * 0.6)
        client.request([0x22, 0x01, 0x01])
    assert active_session(client) == 0x03


def test_default_session_has_no_timeout(client):
    time.sleep(S3_MS / 1000 + 0.2)
    assert active_session(client) == 0x01
