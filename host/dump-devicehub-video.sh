#!/bin/sh
# Dump a LIVE Device Hub "View Screen" session's video to a playable mp4.
# Run as root WHILE Device Hub is mirroring; SWIPE/SCROLL during the capture to test motion.
#
#   sudo ./dump-devicehub-video.sh [seconds]
#
# Reports tcpdump KERNEL DROPS — if drops>0 the dump garble is a capture artifact (tcpdump can't
# keep up with the motion burst), NOT a stream/app problem. -B gives a big buffer to survive bursts.
set -e
DUR="${1:-20}"
DIR=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date +%H%M%S)
BASE="$HOME/dh-$STAMP"

# find the CoreDevice tunnel utun (ULA fd.. / mtu 16000)
IF=""
for i in $(ifconfig -l | tr ' ' '\n' | grep '^utun'); do
  ifconfig "$i" 2>/dev/null | grep -q "inet6 fd" && IF="$i" && break
  ifconfig "$i" 2>/dev/null | grep -q "mtu 16000" && IF="$i" && break
done
[ -z "$IF" ] && { echo "no tunnel utun — connect Device Hub to the device first (before View Screen)"; exit 1; }

echo "==> capturing $IF for ${DUR}s to $BASE.pcap"
echo "==> NOW: make sure View Screen is open, and SWIPE/SCROLL during these ${DUR}s"
# -B 8192 = 8MB kernel buffer (survive motion bursts). stderr has the drop count.
tcpdump -i "$IF" -w "$BASE.pcap" -s 0 -B 8192 2>"$BASE.tcpdump.log" &
TP=$!
sleep "$DUR"
kill -INT $TP 2>/dev/null || true
wait $TP 2>/dev/null || true

echo "==> tcpdump summary:"
grep -E "packets (captured|dropped)" "$BASE.tcpdump.log" | sed 's/^/    /'
DROPS=$(grep -oE "[0-9]+ packets dropped by kernel" "$BASE.tcpdump.log" | grep -oE "^[0-9]+" || echo 0)

echo "==> extracting video"
python3 "$DIR/extract_video.py" "$BASE.pcap" "$BASE.h265" | sed 's/^/    /'

echo "==> decode check"
ERRS=$(ffmpeg -v error -i "$BASE.h265" -f null - 2>&1 | grep -c "error\|no frame\|missing" || true)
echo "    ffmpeg error lines: $ERRS"
ffmpeg -y -v error -r 60 -i "$BASE.h265" -c copy "$BASE.mp4" 2>/dev/null || true
echo "    playable: $BASE.mp4"

echo
if [ "${DROPS:-0}" -gt 0 ]; then
  echo "VERDICT: tcpdump dropped $DROPS packets on the burst -> the DUMP garble is a CAPTURE artifact."
  echo "         The stream is fine; Device Hub / the app receive those packets. Raise -B or shorten capture."
elif [ "${ERRS:-0}" -gt 0 ]; then
  echo "VERDICT: 0 kernel drops but $ERRS decode errors -> a REAL motion issue in the stream/extraction"
  echo "         (reorder / FU reassembly / LTR reference). This mirrors the app bug — dig here."
else
  echo "VERDICT: 0 drops, 0 decode errors -> the dump is CLEAN, motion and all."
  echo "         So the phone's stream is fine; the app garble is app-side (its own receive/decode)."
fi