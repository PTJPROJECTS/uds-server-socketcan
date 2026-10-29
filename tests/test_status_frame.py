import cantools
import pytest

from uds_client import DBC_PATH, STATUS_ID


@pytest.fixture(scope="module")
def db():
    return cantools.database.load_file(DBC_PATH)


def test_period_about_100ms(client):
    frames = client.status_frames(1.05)
    # timing on a loaded CI box is not great, allow some slack
    assert 8 <= len(frames) <= 12
    gaps = [b.timestamp - a.timestamp for a, b in zip(frames, frames[1:])]
    assert 0.07 < sum(gaps) / len(gaps) < 0.13


def test_alive_counter_increments(client, db):
    frames = client.status_frames(0.6)
    counters = [db.decode_message(STATUS_ID, f.data)["AliveCounter"] for f in frames]
    for a, b in zip(counters, counters[1:]):
        assert b == (a + 1) % 16


def test_decode_with_dbc(client, db):
    client.request([0x10, 0x03])
    client.flush_status()
    frame = client.status_frames(0.25)[-1]
    sig = db.decode_message(STATUS_ID, frame.data)
    assert sig["ActiveSession"] == "Extended"
    assert 12.40 <= sig["BatteryVoltage"] <= 12.80


def test_battery_matches_did(client, db):
    # same source for the DID and the broadcast, should be within a few mV
    rsp = client.request([0x22, 0x01, 0x01])
    did_v = int.from_bytes(rsp[3:5], "big") / 100
    client.flush_status()
    frame = client.status_frames(0.15)[-1]
    assert abs(db.decode_message(STATUS_ID, frame.data)["BatteryVoltage"] - did_v) <= 0.05
