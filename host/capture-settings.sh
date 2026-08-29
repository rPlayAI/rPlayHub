#!/bin/sh
# Capture Device Hub's Settings-tab traffic (Appearance, Reduce Motion, Text Size, ...).
#
# WHY THIS IS NOT A USBMUX CAPTURE. Checked live against Device Hub with a device connected:
# DeviceHub.app holds ZERO connections to /var/run/usbmuxd. Every device connection it has is
# TCP over IPv6 to the CoreDevice tunnel's ULA address (fd94:..::1) on the mtu-16000 utun.
# usbmux is used only by remotepairingd to bring that tunnel up; no service traffic rides it.
# So the tunnel utun is the whole story, and tcpdump on it sees everything in cleartext.
#
# ORDER MATTERS — the tab reads its current values the moment it is first opened:
#   1. In Device Hub, CONNECT to the device and leave it on the Info tab (NOT Settings).
#   2. Run this script. It finds the tunnel utun and starts tcpdump.
#   3. THEN click the Settings (sliders) icon — that captures the READ side.
#   4. Then change ONE thing, slowly, and note what and when: e.g. toggle Reduce Motion on,
#      wait 3s, toggle it off. One control at a time is what makes the pcap readable —
#      a burst of changes is very hard to attribute afterwards.
#   5. Ctrl-C here to stop.
#   6. Decode:  python3 host/decode_devicehub.py settings-*.pcap
#
# Needs sudo (tcpdump). No TLS — the tunnel is cleartext.
set -e
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="settings-$STAMP.pcap"

# The CoreDevice tunnel: ULA fd.. address and MTU 16000. Our own cdhost makes one too, so pass
# the interface explicitly when both are up: capture-settings.sh utun10
IF="${1:-}"
if [ -z "$IF" ]; then
  for i in $(ifconfig -l | tr ' ' '\n' | grep '^utun'); do
    if ifconfig "$i" 2>/dev/null | grep -q "inet6 fd"; then IF="$i"; break; fi
  done
fi

if [ -z "$IF" ]; then
  echo "No CoreDevice tunnel utun found (needs an inet6 fd.. address)."
  echo "Connect Device Hub to the device first, then re-run."
  echo "Current utuns:"
  for i in $(ifconfig -l | tr ' ' '\n' | grep '^utun'); do
    echo "  $i mtu=$(ifconfig "$i" | sed -n 's/.*mtu \([0-9]*\).*/\1/p' | head -1) $(ifconfig "$i" | awk '/inet6/{print $2}' | head -1)"
  done
  exit 1
fi

echo "capturing $IF -> $OUT"
echo "now: click Settings, then change ONE control at a time. Ctrl-C to stop."
exec sudo tcpdump -i "$IF" -w "$OUT" -s 0
