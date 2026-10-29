# uds-server-socketcan

A small UDS (ISO 14229) diagnostic server in C, running over ISO-TP (ISO 15765-2) on Linux SocketCAN. Plus a Python tester and a pytest suite.

It acts like a very simple ECU: you can switch sessions, read a few DIDs (VIN, SW version, serial, battery voltage), reset it, and keep the session alive with TesterPresent. It also broadcasts a status frame every 100 ms, which is described in a DBC file and decoded with cantools.

## Why I built this

At work I've used diagnostic tools a lot, mostly from the tester side. You send `22 F1 90`, you get a VIN back, done. I wanted to write the ECU side myself to really understand what happens underneath: how a 20 byte response gets split into CAN frames, what flow control is actually doing, and why the ECU drops you back to the default session if you stop sending TesterPresent.

I also didn't want it to depend on hardware, so the whole thing runs on one Linux box.

## Features

- UDS services: 0x10, 0x11, 0x22, 0x3E (table below)
- Negative responses: 0x11, 0x12, 0x13, 0x14, 0x22, 0x31, 0x7F
- suppressPosRspMsgIndicationBit (0x80) for 0x10 / 0x11 / 0x3E
- S3 session timer (default 5 s). Non-default session falls back to default if the tester goes quiet
- Functional addressing on 0x7DF (single frame only), with the NRC suppression rules from the spec
- My own ISO-TP layer in C: single frame, first frame, consecutive frames, flow control (CTS / WAIT / OVFLW), block size, STmin, N_Bs and N_Cr timeouts, padding to 8 bytes
- Periodic 0x3A0 status frame (battery voltage, session, alive counter, reset count) with a DBC
- Python tester (`tools/uds_client.py`) that uses the `can-isotp` package, so my C ISO-TP is checked against a different implementation and not just against itself
- 50 pytest tests, CI on GitHub Actions (gcc + clang, ASan/UBSan run)

### Kernel ISO-TP or my own?

**My own.** Linux has a kernel ISO-TP socket (`CAN_ISOTP`, mainline since 5.10), and that's what I'd use in a product. But the container I developed in has no CAN support in the kernel at all (`socket(PF_CAN)` fails with `Address family not supported by protocol`, no `vcan`, no `can_isotp` module). So I wrote a minimal ISO-TP on top of raw CAN frames. Honestly it was the more useful exercise anyway.

## Architecture

```
  tester (python)                         uds_server (C)
 +------------------+                  +---------------------------+
 | uds_client.py    |                  |  uds.c     services, NRC, |
 | pytest           |                  |            S3 timer, DIDs |
 +------------------+                  +---------------------------+
 | can-isotp        |                  |  isotp.c   SF/FF/CF/FC    |
 +------------------+                  +---------------------------+
 | python-can       |                  |  can_if.c  transport      |
 |  socketcan / udp |                  |   socketcan  |  udp       |
 +--------+---------+                  +-------+-------------+-----+
          |                                    |             |
          +------ vcan0 / can0 (SocketCAN) ----+             |
          |                                                  |
          +------ UDP 127.0.0.1 (one can_frame per packet) --+

  0x7E0  tester -> ECU   (physical request)
  0x7DF  tester -> all   (functional request)
  0x7E8  ECU -> tester   (response)
  0x3A0  ECU -> all      (status, every 100 ms)
```

`main.c` is one loop: poll the socket with a 10 ms timeout, feed frames into the ISO-TP state machine, hand a complete request to the UDS layer, send the response, then do the timers (N_Cr, S3, status frame).

The UDP transport sends the exact same 16 byte `struct can_frame` you'd get from a raw CAN socket. Everything above `can_if.c` doesn't know which one it's on.

```
.
├── src/            main.c, can_if.c, isotp.c, uds.c, timeutil.c
├── include/
├── dbc/            ecu_status.dbc
├── tools/          uds_client.py, udpcan.py (python-can bus for the UDP transport)
├── tests/          pytest suite
├── scripts/        setup_vcan.sh
├── Makefile, CMakeLists.txt
└── .github/workflows/ci.yml
```

## Build

Needs gcc (or clang) and make. Plain C99, `-Wall -Wextra -Wpedantic`, builds with no warnings.

```sh
make
# or
cmake -S . -B build-cmake && cmake --build build-cmake
```

Python side:

```sh
pip install -r requirements.txt     # python-can, cantools, can-isotp, pytest
```

## Run

### Option 1: vcan (normal Linux machine)

```sh
./scripts/setup_vcan.sh                  # modprobe vcan + ip link add vcan0
./build/uds_server -i vcan0 -v           # terminal 1
candump vcan0                            # terminal 2, optional
./tools/uds_client.py -c vcan0 session 03
./tools/uds_client.py -c vcan0 read F190 F195
```

On Ubuntu, if `modprobe vcan` fails you probably need `linux-modules-extra-$(uname -r)`.

### Option 2: UDP loopback (docker, WSL, anything without CAN)

```sh
./build/uds_server -u 47000:47001 -v     # listen on 47000, send to 47001
./tools/uds_client.py --udp session 03   # client defaults to 47001:47000
./tools/uds_client.py --udp read F190
./tools/uds_client.py --udp monitor -t 0.5
```

Server options:

```
  -i IF     SocketCAN interface (vcan0, can0, ...)
  -u L:R    UDP loopback bus, listen on L, send to R
  -s MS     S3 session timeout in ms (default 5000)
  -p MS     status frame period in ms, 0 = off (default 100)
  -b BS     block size we send in our flow control (default 0)
  -m STMIN  STmin we send in our flow control, raw byte (default 0)
  -v        print every frame (candump style)
```

## Example output

Real output from my machine. I don't have `candump` here (no CAN in this container), so the frame log is the server's own `-v` trace. Same columns as candump, plus R/T for direction.

Client:

```
$ ./tools/uds_client.py --udp session 03
-> 10 03
<- 50 03 00 32 01 F4
positive: 50 03 00 32 01 F4
$ ./tools/uds_client.py --udp read F190
-> 22 F1 90
<- 62 F1 90 54 45 53 54 56 49 4E 30 30 30 30 30 30 30 30 34 32
positive:
  F190 VIN              TESTVIN0000000042
$ ./tools/uds_client.py --udp read 0101
-> 22 01 01
<- 62 01 01 04 F8
positive:
  0101 battery voltage  12.72 V
$ ./tools/uds_client.py --udp raw 2EF19000
-> 2E F1 90 00
<- 7F 2E 11
NRC 0x11 serviceNotSupported (service 0x2E)
$ ./tools/uds_client.py --udp reset 01
-> 11 01
<- 51 01
positive: 51 01
$ ./tools/uds_client.py --udp reset 01
-> 11 01
<- 7F 11 7F
NRC 0x7F serviceNotSupportedInActiveSession (service 0x11)
```

The second reset fails because the first one put the ECU back in the default session, and I don't allow resets from default.

Server trace for the same run (`-v -p 0 -s 2000`, status frame turned off so it's readable):

```
uds_server up on udp47000 (req 0x7E0 rsp 0x7E8 func 0x7DF), S3=2000 ms
  udp47000 R 7E0   [8]  02 10 03 CC CC CC CC CC
uds: session default -> extended
  udp47000 T 7E8   [8]  06 50 03 00 32 01 F4 CC
  udp47000 R 7E0   [8]  03 22 F1 90 CC CC CC CC
  udp47000 T 7E8   [8]  10 14 62 F1 90 54 45 53
  udp47000 R 7E0   [8]  30 08 00 CC CC CC CC CC
  udp47000 T 7E8   [8]  21 54 56 49 4E 30 30 30
  udp47000 T 7E8   [8]  22 30 30 30 30 30 34 32
  udp47000 R 7E0   [8]  03 22 01 01 CC CC CC CC
  udp47000 T 7E8   [8]  05 62 01 01 04 F8 CC CC
  udp47000 R 7E0   [8]  04 2E F1 90 00 CC CC CC
  udp47000 T 7E8   [8]  03 7F 2E 11 CC CC CC CC
  udp47000 R 7E0   [8]  02 11 01 CC CC CC CC CC
  udp47000 T 7E8   [8]  02 51 01 CC CC CC CC CC
uds: hard reset
  udp47000 R 7E0   [8]  02 11 01 CC CC CC CC CC
  udp47000 T 7E8   [8]  03 7F 11 7F CC CC CC CC
  udp47000 R 7E0   [8]  02 10 03 CC CC CC CC CC
uds: session default -> extended
  udp47000 T 7E8   [8]  06 50 03 00 32 01 F4 CC
uds: S3 timeout, back to default session
```

How to read the VIN part: `10 14` is a first frame with length 0x014 = 20 bytes (`62 F1 90` + 17 VIN chars). The tester answers with flow control `30 08 00` = continue to send, block size 8, STmin 0. Then `21`, `22` are the consecutive frames with sequence numbers 1 and 2. The last line is the S3 timer kicking in 2 s after the last request.

Status frame decoded with the DBC:

```
$ ./tools/uds_client.py --udp monitor -t 0.5
3A0  EE 04 03 06 00 00 00 00  batt=12.62V session=Extended alive=6 resets=0
3A0  EF 04 03 07 00 00 00 00  batt=12.63V session=Extended alive=7 resets=0
3A0  F0 04 03 08 00 00 00 00  batt=12.64V session=Extended alive=8 resets=0
3A0  F1 04 03 09 00 00 00 00  batt=12.65V session=Extended alive=9 resets=0
3A0  F2 04 03 0A 00 00 00 00  batt=12.66V session=Extended alive=10 resets=0
```

## Tests

```sh
make test                              # UDP bus, default
UDS_TRANSPORT=socketcan UDS_CHANNEL=vcan0 python3 -m pytest -v tests
make asan                              # rebuild with ASan/UBSan and run the tests
```

The tests start their own `uds_server` (S3 set to 1 s so the timeout tests are quick). Last run:

```
tests/test_isotp_raw.py::test_single_frame_padding PASSED                [  2%]
tests/test_isotp_raw.py::test_server_sends_fc_on_first_frame PASSED      [  4%]
tests/test_isotp_raw.py::test_server_honors_block_size PASSED            [  6%]
tests/test_isotp_raw.py::test_server_handles_fc_wait PASSED              [  8%]
tests/test_isotp_raw.py::test_server_aborts_on_overflow PASSED           [ 10%]
tests/test_isotp_raw.py::test_server_respects_stmin PASSED               [ 12%]
...
tests/test_services.py::test_reset_not_in_default_session PASSED         [ 66%]
tests/test_services.py::test_hard_reset PASSED                           [ 68%]
...
tests/test_session_timeout.py::test_s3_timeout_drops_to_default PASSED   [ 86%]
tests/test_session_timeout.py::test_tester_present_keeps_session PASSED  [ 88%]
tests/test_session_timeout.py::test_any_request_restarts_s3 PASSED       [ 90%]
tests/test_session_timeout.py::test_default_session_has_no_timeout PASSED [ 92%]
tests/test_status_frame.py::test_period_about_100ms PASSED               [ 94%]
tests/test_status_frame.py::test_alive_counter_increments PASSED         [ 96%]
tests/test_status_frame.py::test_decode_with_dbc PASSED                  [ 98%]
tests/test_status_frame.py::test_battery_matches_did PASSED              [100%]

============================= 50 passed in 19.12s ==============================
```

What's covered:

- `test_services.py`: every positive response and every NRC in the table below except 0x14 (none of my DIDs are big enough to hit it), suppress bit, functional requests
- `test_session_timeout.py`: S3 timeout, TesterPresent keeping the session, any request restarting S3
- `test_status_frame.py`: 100 ms period, alive counter, DBC decode, broadcast value matching the DID
- `test_isotp_raw.py`: hand-built frames to poke the ISO-TP layer: block size, FC.WAIT, FC.OVFLW, STmin timing, no FC at all (N_Bs), wrong sequence number, stray CF

## Supported services

| SID  | Service                  | Sub-functions             | Notes |
|------|--------------------------|---------------------------|-------|
| 0x10 | DiagnosticSessionControl | 01 default, 02 programming, 03 extended | Response has P2 = 50 ms, P2* = 5000 ms. Programming only from extended |
| 0x11 | ECUReset                 | 01 hard, 03 soft          | Not allowed in default session. Reset happens after the response goes out |
| 0x22 | ReadDataByIdentifier     | -                         | Several DIDs per request. Unknown DIDs skipped, NRC 0x31 only if none matched |
| 0x3E | TesterPresent            | 00                        | Usually sent as `3E 80` (no response) |

DIDs:

| DID    | What                 | Format |
|--------|----------------------|--------|
| 0xF190 | VIN                  | 17 ASCII chars (`TESTVIN0000000042`, fake) |
| 0xF195 | SW version           | ASCII, 5 bytes |
| 0xF18C | ECU serial number    | ASCII, 14 bytes |
| 0xF186 | Active session       | 1 byte |
| 0x0101 | Battery voltage      | uint16 big endian, 0.01 V/bit |

Negative responses:

| NRC  | Name                                   | When |
|------|----------------------------------------|------|
| 0x11 | serviceNotSupported                    | Any SID not in the table above |
| 0x12 | subFunctionNotSupported                | e.g. `10 05`, `11 02`, `3E 01` |
| 0x13 | incorrectMessageLengthOrInvalidFormat  | Wrong length, e.g. `10` or `22 F1` |
| 0x14 | responseTooLong                        | Response wouldn't fit in 4095 bytes |
| 0x22 | conditionsNotCorrect                   | `10 02` from default session |
| 0x31 | requestOutOfRange                      | Unknown DID |
| 0x7F | serviceNotSupportedInActiveSession     | `11 xx` in default session |

For functional requests (0x7DF) the server stays quiet on 0x11, 0x12, 0x31, 0x7E and 0x7F, like the spec says. Otherwise every ECU on the bus would answer "not supported" to everything.

## What I learned / bugs I hit

- **No CAN in my dev container.** `modprobe vcan` doesn't work, there's no `/lib/modules`, and even `socket(PF_CAN, ...)` fails with errno 97. That's why the UDP transport exists. Keeping it at the "send a `struct can_frame`" level meant the ISO-TP and UDS code stayed exactly the same.
- **Status frame went silent for a whole second.** `isotp_send()` blocks while it waits for the tester's flow control. If the tester never sends one, it sits there for N_Bs (1 s), and during that time the 0x3A0 frame stopped. I only noticed when I measured the gap between status frames: 1001 ms instead of 100. Fixed it with an `on_idle` callback that runs while waiting for FC. There's a regression test for it now (`test_status_frame_keeps_going_while_waiting_for_fc`), and it fails on the old code.
- **RDBI responses don't carry a length per DID.** The response is just `62 DID data DID data ...`. My first client decoder treated string DIDs as "the rest of the message", which works for one DID and falls apart as soon as you read two. A real tester knows the lengths from ODX/CDD. Mine has a small table that has to match `uds.c`.
- **argparse ate my subcommand.** I had `--udp [PORTS]` with `nargs="?"`, so `--udp session 03` took `session` as the port string. Split it into `--udp` and `--ports`.
- **`make CFLAGS=-Werror` broke the build** with `can_if.h: No such file or directory`. A variable set on the make command line overrides the Makefile, `+=` included, so `-Iinclude` disappeared. Moved the required flags into `BASE_CFLAGS`.
- **"Address already in use" on the UDP port.** I was starting the server with `cd dir && ./build/uds_server ... &`. That backgrounds a subshell, so `$!` was the subshell's pid, `kill $!` didn't stop the server, and an old server kept holding port 47000. Not a code bug, but it cost me some time.
- Small things that are easy to get wrong: P2* in the 0x50 response is in **10 ms units** (so 5000 ms goes out as `01 F4`), not 1 ms. STmin `0x00-0x7F` is milliseconds but `0xF1-0xF9` is 100-900 **micro**seconds. The CF sequence number wraps 15 -> 0, not back to 1. You can see python can-isotp asking for block size 8 in the trace (`30 08 00`), which was a nice way to check my block size handling against something I didn't write.

## Limitations

- I developed and tested on the **UDP transport**. The SocketCAN path (`-i vcan0`) builds and uses the same code above the transport, but I couldn't run it in my container. The CI workflow tries to bring up vcan0 on the GitHub runner and runs the full suite on it if that works.
- My own ISO-TP, not the kernel's `CAN_ISOTP`. Classic CAN only (8 bytes), no CAN FD, normal 11-bit addressing, max 4095 byte messages (no 32-bit FF length).
- Single threaded. While a multi-frame response is going out, new requests are ignored.
- ECUReset is simulated. It just clears the session and bumps a counter.
- No 0x78 responsePending. Everything answers right away.
- DIDs are hardcoded. Nothing is writable yet.
- Two session rules are my choice and not universal: programming session only from extended (some ECUs use 0x7E instead of 0x22 here), and no ECUReset from default.
- The UDP "bus" is point to point. No arbitration, no third node listening.

## Next steps

- SecurityAccess (0x27): seed/key, attempt counter, delay timer. Then lock 0x2E and 0x11 behind it
- WriteDataByIdentifier (0x2E), backed by a small file as fake NVM
- ReadDTCInformation (0x19) / ClearDTC (0x14)
- Option to use the kernel `CAN_ISOTP` socket instead of my ISO-TP
- DoIP (ISO 13400) transport so the same UDS layer works over Ethernet
- Run it on real hardware (a board with a CAN transceiver, talking to a USB-CAN adapter on a laptop)

## License

MIT, see [LICENSE](LICENSE).
