import os
import socket
import subprocess
import sys
import time

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from uds_client import UdsClient, make_bus  # noqa: E402

SERVER = os.path.join(ROOT, "build", "uds_server")

# UDS_TRANSPORT=udp (default) or socketcan. For socketcan set UDS_CHANNEL too.
TRANSPORT = os.environ.get("UDS_TRANSPORT", "udp")
CHANNEL = os.environ.get("UDS_CHANNEL", "vcan0")

# short S3 so the timeout test doesn't take forever
S3_MS = 1000


def _free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@pytest.fixture(scope="session")
def server():
    if not os.path.exists(SERVER):
        subprocess.check_call(["make", "-C", ROOT])

    if TRANSPORT == "socketcan":
        args = ["-i", CHANNEL]
        ports = None
    else:
        srv_port, cli_port = _free_udp_port(), _free_udp_port()
        args = ["-u", f"{srv_port}:{cli_port}"]
        ports = f"{cli_port}:{srv_port}"

    log = open(os.path.join(ROOT, "build", "server_test.log"), "w")
    proc = subprocess.Popen([SERVER, *args, "-s", str(S3_MS), "-v"],
                            stdout=log, stderr=subprocess.STDOUT)
    time.sleep(0.3)
    assert proc.poll() is None, "uds_server died on startup, see build/server_test.log"

    yield {"ports": ports, "proc": proc}

    proc.terminate()
    proc.wait(timeout=3)
    log.close()


def open_bus(server):
    if TRANSPORT == "socketcan":
        return make_bus(channel=CHANNEL)
    return make_bus(udp=server["ports"])


@pytest.fixture(scope="module")
def client(server):
    bus = open_bus(server)
    cli = UdsClient(bus)
    yield cli
    cli.close()
    bus.shutdown()


@pytest.fixture(autouse=True)
def default_session(request):
    """Every test starts in the default session."""
    if "client" in request.fixturenames:
        cli = request.getfixturevalue("client")
        cli.request([0x10, 0x01])
        cli.flush_status()
    yield
