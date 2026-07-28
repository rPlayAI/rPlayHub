#!/usr/bin/env bash
#
# Capture Apple's OWN CoreDevice traffic, so we can read the real startmediastream offer.
#
#   sudo ./scripts/capture-apple-session.sh [seconds]        default 60
#
# Why this works: remoted builds a utun to the device and speaks RSD/RemoteXPC over it — HTTP/2
# with XPC dictionaries, in CLEARTEXT inside the tunnel. So a tcpdump on that interface while
# Device Hub mirrors a screen records exactly what Apple sends, including the media-stream offer
# whose parameters we have only guessed at (bitrate tiers, feature strings, PLI/FIR enablement,
# keyframe cadence).
#
# Procedure:
#   1. Close Device Hub.
#   2. Run this.
#   3. When it says GO: open Device Hub, select the device, click View Screen, let it mirror.
#   4. Decode with:  python3 scripts/decode-remotexpc.py <pcap>
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
DUR="${1:-60}"
OUT="$ROOT/logs/apple-session-$(date +%H%M%S)"
mkdir -p "$OUT"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
[[ $EUID -eq 0 ]] || { echo "run with sudo (packet capture is privileged)" >&2; exit 1; }

# Apple's tunnels look exactly like ours: a ULA fd..::2 on a utun at MTU 16000. Ours is not
# running during this capture, so any such interface is remoted's.
BEFORE=$(ifconfig | awk '/^utun/{i=$1} /inet6 fd/{print i}' | tr '\n' ' ')
say "utun interfaces with a tunnel address right now: ${BEFORE:-none}"
say "GO — open Device Hub, select the device, click View Screen. Capturing ${DUR}s."

# Capture every utun; remoted may create a fresh one when Device Hub connects.
PIDS=()
for i in $(seq 0 12); do
    if ifconfig "utun$i" >/dev/null 2>&1; then
        tcpdump -i "utun$i" -s 0 -w "$OUT/utun$i.pcap" >/dev/null 2>&1 &
        PIDS+=($!)
    fi
done
[[ ${#PIDS[@]} -gt 0 ]] || { echo "no utun interfaces to capture on" >&2; exit 1; }

for i in $(seq "$DUR"); do printf "\r  %ds/%ds" "$i" "$DUR"; sleep 1; done
echo
for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
sleep 1

say "captured:"
for f in "$OUT"/*.pcap; do
    n=$(tcpdump -r "$f" 2>/dev/null | wc -l | tr -d ' ')
    sz=$(du -h "$f" | cut -f1)
    printf "    %-28s %6s packets  %s\n" "$(basename "$f")" "$n" "$sz"
    [[ "$n" == "0" ]] && rm -f "$f"
done
chmod -R a+r "$OUT" 2>/dev/null || true

echo
say "next: python3 scripts/decode-remotexpc.py $OUT/<file>.pcap"
echo "    The interesting message is the startmediastream invocation — its"
echo "    avcMediaStreamNegotiatorMediaBlob is the offer we have been guessing at."
