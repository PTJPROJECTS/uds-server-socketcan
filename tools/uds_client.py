#!/usr/bin/env python3
"""
Tiny UDS tester for uds_server.

  ./tools/uds_client.py --udp session 03
  ./tools/uds_client.py --udp read F190 F195
  ./tools/uds_client.py -c vcan0 raw 22F18C
  ./tools/uds_client.py --udp monitor -t 0.5   # decode 0x3A0 with the DBC

ISO-TP on this side is the can-isotp package, not my C code, so the two
implementations check each other.
"""
import argparse
import os
import sys
import time

import can
import cantools
import isotp

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from udpcan import UdpCanBus  # noqa: E402

REQ_ID = 0x7E0
RSP_ID = 0x7E8
FUNC_ID = 0x7DF
STATUS_ID = 0x3A0

DBC_PATH = os.path.join(os.path.dirname(__file__), "..", "dbc", "ecu_status.dbc")

NRC_NAMES = {
    0x10: "generalReject",
    0x11: "serviceNotSupported",
    0x12: "subFunctionNotSupported",
    0x13: "incorrectMessageLengthOrInvalidFormat",
    0x14: "responseTooLong",
    0x22: "conditionsNotCorrect",
    0x31: "requestOutOfRange",
    0x78: "requestCorrectlyReceived-ResponsePending",
    0x7E: "subFunctionNotSupportedInActiveSession",
    0x7F: "serviceNotSupportedInActiveSession",
}

DID_NAMES = {
    0xF190: "VIN",
    0xF195: "SW version",
    0xF18C: "ECU serial",
    0xF186: "active session",
    0x0101: "battery voltage",
}


def make_bus(channel=None, udp=None):
    if udp:
        return UdpCanBus(udp)
    return can.Bus(interface="socketcan", channel=channel)


class UdsClient:
    def __init__(self, bus, blocksize=8, stmin=0):
        self.bus = bus
        self.status = can.BufferedReader()
        addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                             txid=REQ_ID, rxid=RSP_ID)
        params = {
            "blocksize": blocksize,
            "stmin": stmin,
            "tx_padding": 0xCC,
            "rx_flowcontrol_timeout": 1000,
            "rx_consecutive_frame_timeout": 1000,
        }
        self.notifier = can.Notifier(bus, [self.status])
        self.stack = isotp.NotifierBasedCanStack(bus, self.notifier,
                                                 address=addr, params=params)
        self.stack.start()

    def close(self):
        self.stack.stop()
        self.notifier.stop()

    def request(self, data, timeout=2.0):
        """Send a request, return the response bytes (or None)."""
        self.stack.send(bytes(data))
        deadline = time.time() + timeout
        while time.time() < deadline:
            rsp = self.stack.recv(block=True, timeout=0.05)
            if rsp is None:
                continue
            # 0x78 = still working on it, keep waiting
            if len(rsp) >= 3 and rsp[0] == 0x7F and rsp[2] == 0x78:
                deadline = time.time() + 5.0
                continue
            return bytes(rsp)
        return None

    def send_functional(self, data):
        """Single frame on 0x7DF, raw (can-isotp is set up for physical)."""
        data = bytes(data)
        assert len(data) <= 7
        payload = bytes([len(data)]) + data
        payload = payload.ljust(8, b"\xCC")
        self.bus.send(can.Message(arbitration_id=FUNC_ID, data=payload,
                                  is_extended_id=False))

    def wait_response(self, timeout=0.5):
        return self.stack.recv(block=True, timeout=timeout)

    def status_frames(self, duration):
        """Collect 0x3A0 frames for `duration` seconds."""
        out = []
        end = time.time() + duration
        while time.time() < end:
            m = self.status.get_message(timeout=0.05)
            if m is not None and m.arbitration_id == STATUS_ID:
                out.append(m)
        return out

    def flush_status(self):
        while self.status.get_message(timeout=0) is not None:
            pass


def describe(rsp):
    if rsp is None:
        return "no response (timeout)"
    if rsp[0] == 0x7F:
        name = NRC_NAMES.get(rsp[2], "?")
        return f"NRC 0x{rsp[2]:02X} {name} (service 0x{rsp[1]:02X})"
    if rsp[0] == 0x62:
        return describe_rdbi(rsp)
    return "positive: " + rsp.hex(" ").upper()


def describe_rdbi(rsp):
    # The 0x62 response has no length per DID, so the tester has to know
    # them up front (a real tester gets this from ODX/CDD). These must match
    # src/uds.c. First version had None = "rest of the frame" for the strings
    # and it fell apart as soon as you read two of them in one request.
    lengths = {0xF190: 17, 0xF195: 5, 0xF18C: 14, 0xF186: 1, 0x0101: 2}
    i = 1
    lines = []
    while i + 2 <= len(rsp):
        did = (rsp[i] << 8) | rsp[i + 1]
        i += 2
        n = lengths.get(did, len(rsp) - i)
        val = rsp[i:i + n]
        i += n
        if did == 0x0101:
            shown = f"{int.from_bytes(val, 'big') / 100:.2f} V"
        elif did == 0xF186:
            shown = f"0x{val[0]:02X}"
        else:
            shown = val.decode(errors="replace")
        lines.append(f"  {did:04X} {DID_NAMES.get(did, '?'):<16} {shown}")
    return "positive:\n" + "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="small UDS client for uds_server")
    ap.add_argument("-c", "--channel", default="vcan0", help="SocketCAN iface")
    # note: this used to be --udp [PORTS] with nargs="?", but then argparse
    # ate the subcommand ("--udp session 03" -> ports="session"). Two flags now.
    ap.add_argument("--udp", action="store_true", help="use the UDP loopback bus")
    ap.add_argument("--ports", default="47001:47000",
                    help="UDP LOCAL:REMOTE ports (default 47001:47000)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("session"); p.add_argument("type", help="01, 02 or 03")
    p = sub.add_parser("reset"); p.add_argument("type", nargs="?", default="01")
    p = sub.add_parser("read"); p.add_argument("dids", nargs="+")
    sub.add_parser("tp")
    p = sub.add_parser("raw"); p.add_argument("hex")
    p = sub.add_parser("monitor"); p.add_argument("-t", "--time", type=float, default=1.0)
    args = ap.parse_args()

    bus = make_bus(args.channel, args.ports if args.udp else None)
    cli = UdsClient(bus)
    try:
        if args.cmd == "monitor":
            db = cantools.database.load_file(DBC_PATH)
            for m in cli.status_frames(args.time):
                sig = db.decode_message(m.arbitration_id, m.data)
                print(f"{m.arbitration_id:03X}  {m.data.hex(' ').upper()}  "
                      f"batt={sig['BatteryVoltage']:.2f}V session={sig['ActiveSession']} "
                      f"alive={sig['AliveCounter']} resets={sig['ResetCount']}")
            return 0

        if args.cmd == "session":
            req = bytes([0x10, int(args.type, 16)])
        elif args.cmd == "reset":
            req = bytes([0x11, int(args.type, 16)])
        elif args.cmd == "read":
            req = bytes([0x22]) + b"".join(int(d, 16).to_bytes(2, "big") for d in args.dids)
        elif args.cmd == "tp":
            req = bytes([0x3E, 0x00])
        else:
            req = bytes.fromhex(args.hex)

        print("->", req.hex(" ").upper())
        rsp = cli.request(req)
        if rsp is not None:
            print("<-", rsp.hex(" ").upper())
        print(describe(rsp))
        return 0 if rsp is not None and rsp[0] != 0x7F else 2
    finally:
        cli.close()
        bus.shutdown()


if __name__ == "__main__":
    sys.exit(main())
