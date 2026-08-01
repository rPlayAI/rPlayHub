#!/usr/bin/env bash
#
# Capture the RemotePairing control channel on the LAN, in cleartext.
#
# The device advertises _remotepairing._tcp and then waits: a TCP connect to it
# succeeds and the device says nothing, so the host speaks first. We therefore
# need the exact framing of that first message, and doc/REMOTEPAIRING-PROTOCOL.md
# has the message *set* (from a symbol dump) but not the bytes.
#
# Per that spec, the control channel is plain before authentication and only
# becomes a ChaCha20-Poly1305 stream afterwards -- so the handshake, and the
# whole pair-verify/pair-setup exchange, is readable on the wire. This is the
# same trick as doc/COREDEVICE-CAPTURE-METHOD.md: capture below the app's crypto.
#
# Unlike the CoreDevice captures this is NOT on a utun -- RemotePairing runs on
# the ordinary LAN interface, before any tunnel exists.
#
#   sudo ./scripts/capture-remotepairing.sh [seconds]
#
# While it runs: in Device Hub, connect to the iPhone OVER WIFI (unplug USB
# first, or the Mac will prefer the cable and nothing will cross the LAN).
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-40}"
OUT="build/remotepairing-$(date +%Y%m%d-%H%M%S).pcap"
mkdir -p build

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
[ "$(id -u)" = "0" ] || { echo "needs sudo: sudo $0 $DUR" >&2; exit 1; }

# The interface carrying the LAN, not a tunnel. Prefer the one with the default
# IPv4 route, which on a laptop is the active Wi-Fi.
IFACE="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')"
IFACE="${IFACE:-en0}"
say "capturing on $IFACE for ${DUR}s"
say "NOW: unplug USB, then connect the iPhone in Device Hub over Wi-Fi"

# Port 49152 is where this device advertised _remotepairing._tcp, but the port is
# assigned dynamically, so take the whole dynamic range rather than pin one port.
tcpdump -i "$IFACE" -w "$OUT" -s 0 -U \
    'tcp portrange 49000-65535 or udp port 5353' 2>build/rp-tcpdump.log &
TPID=$!
sleep "$DUR"
kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null

say "tcpdump:"; sed 's/^/     /' build/rp-tcpdump.log
chown "${SUDO_USER:-$(stat -f%Su /dev/console)}" "$OUT" 2>/dev/null
say "wrote $OUT"
echo
echo "Now:  python3 scripts/decode-remotepairing.py $OUT"
