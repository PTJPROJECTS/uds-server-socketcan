"""
python-can bus that talks to uds_server's UDP loopback transport.

Each datagram is one struct can_frame (16 bytes), same as a raw CAN socket:
    u32 can_id | u8 dlc | 3 pad | 8 data
Only used when there is no vcan (containers, WSL1, some CI runners).
"""
import socket
import struct
import time

import can

_FMT = "<IB3x8s"


class UdpCanBus(can.BusABC):
    def __init__(self, channel="47001:47000", **kwargs):
        local, remote = (int(p) for p in str(channel).split(":"))
        self._remote = ("127.0.0.1", remote)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("127.0.0.1", local))
        self.channel_info = f"udp {local}->{remote}"
        super().__init__(channel=channel, **kwargs)

    def send(self, msg, timeout=None):
        data = bytes(msg.data).ljust(8, b"\x00")
        frame = struct.pack(_FMT, msg.arbitration_id, msg.dlc, data)
        try:
            self._sock.sendto(frame, self._remote)
        except ConnectionRefusedError:
            pass

    def _recv_internal(self, timeout):
        self._sock.settimeout(timeout)
        try:
            raw = self._sock.recv(16)
        except (socket.timeout, BlockingIOError):
            return None, False
        except ConnectionRefusedError:
            return None, False
        if len(raw) != 16:
            return None, False
        can_id, dlc, data = struct.unpack(_FMT, raw)
        msg = can.Message(
            timestamp=time.time(),
            arbitration_id=can_id & 0x7FF,
            is_extended_id=False,
            dlc=dlc,
            data=data[:dlc],
            channel=self.channel_info,
        )
        return msg, False

    def shutdown(self):
        super().shutdown()
        self._sock.close()
