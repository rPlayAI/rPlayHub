#!/bin/sh
# Capture Apple's own Device Hub "View Screen" CoreDevice session (cleartext, on the tunnel utun).
#
# ORDER MATTERS — startmediastream fires the instant View Screen opens:
#   1. In Xcode/Device Hub, CONNECT to the device (do NOT hit View Screen yet).
#      That brings up the CoreDevice tunnel utun.
#   2. Run this script — it finds the tunnel utun and starts tcpdump.
#   3. THEN click "View Screen" and let it mirror ~15s. Ctrl-C here to stop.
#   4. Decode:  python3 decode_devicehub.py devicehub-*.pcap
#
# Needs sudo (tcpdump). No TLS — the tunnel is cleartext.
set -e
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="devicehub-$STAMP.pcap"

# find the CoreDevice tunnel utun: ULA fd.. address and/or MTU 16000. Pass one explicitly when
# more than one tunnel is up (our own cdhost makes one too): capture-devicehub.sh utun9
IF="${1:-}"
[ -n "$IF" ] || for i in $(ifconfig -l | tr ' ' '\n' | grep '^utun'); do
  if ifconfig "$i" 2>/dev/null | grep -q "inet6 fd"; then IF="$i"; break; fi
  if ifconfig "$i" 2>/dev/null | grep -q "mtu 16000"; then IF="$i"; break; fi
done

if [ -z "$IF" ]; then
  echo "No CoreDevice tunnel utun found (ULA fd.. / mtu 16000)."
  echo "Connect Device Hub to the device FIRST (before View Screen), then re-run."
  echo "Current utuns:"; for i in $(ifconfig -l | tr ' ' '\n' | grep '^utun'); do
    echo "  $i mtu=$(ifconfig $i | sed -n 's/.*mtu \([0-9]*\).*/\1/p' | head -1) $(ifconfig $i | awk '/inet6/{print $2}' | head -1)"; done
  exit 1
fi

echo "capturing $IF -> $OUT   (now click View Screen; Ctrl-C to stop)"
exec sudo tcpdump -i "$IF" -w "$OUT" -s 0
