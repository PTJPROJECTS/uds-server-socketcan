#!/bin/sh
# Bring up a virtual CAN interface for local testing.
#   ./scripts/setup_vcan.sh          -> vcan0
#   ./scripts/setup_vcan.sh vcan1
#   ./scripts/setup_vcan.sh vcan0 down
set -e

IFACE=${1:-vcan0}
ACTION=${2:-up}

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

if [ "$ACTION" = "down" ]; then
    $SUDO ip link del "$IFACE" 2>/dev/null || true
    echo "$IFACE removed"
    exit 0
fi

if ! $SUDO modprobe vcan 2>/dev/null; then
    # docker / WSL kernels often don't ship it. maybe it's built in?
    if ! grep -q vcan /proc/modules 2>/dev/null && [ ! -d /sys/module/vcan ]; then
        echo "can't load vcan (no module for $(uname -r))."
        echo "on ubuntu try: sudo apt install linux-modules-extra-\$(uname -r)"
        echo "or skip vcan and use the UDP bus: ./build/uds_server -u 47000:47001"
        exit 1
    fi
fi
# can_isotp isn't needed (own isotp in src/), load it only if it's there
$SUDO modprobe can_isotp 2>/dev/null || true

if ! ip link show "$IFACE" >/dev/null 2>&1; then
    $SUDO ip link add dev "$IFACE" type vcan
fi
$SUDO ip link set up "$IFACE"
ip -details link show "$IFACE"
