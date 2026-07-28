#!/usr/bin/env bash
#
# One command, one answer: is the engine emitting decodable video?
#
#   sudo ./scripts/diagnose.sh [udid]
#
# Starts the engine, attaches ONLY a byte-capturing viewer (no app), records 10 seconds, stops the
# engine, then decodes what it captured with an independent decoder. The point of ffmpeg here is
# that it is NOT our code: if it renders frames, the engine is fine and the fault is in the app's
# decode path; if it cannot, the engine is at fault and the app was never the problem.
#
# Nothing here ships. The app links no ffmpeg.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[[ $EUID -eq 0 ]] || { echo "run with sudo: sudo ./scripts/diagnose.sh" >&2; exit 1; }

OUT="$ROOT/build/diagnose"
mkdir -p "$OUT"
CAP="$OUT/live.h265"
ENGINE_LOG="$OUT/engine.log"
rm -f "$CAP" "$ENGINE_LOG" "$OUT/frame.png"   # a stale frame.png made a past run report success

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

for port in 9876 9877; do
    if lsof -nP -iTCP:$port -sTCP:LISTEN >/dev/null 2>&1; then
        echo "port $port is busy — stop the other engine first" >&2; exit 1
    fi
done

UDID="${1:-}"
if [[ -z "$UDID" ]]; then
    # Screen viewing needs iOS 27+. Auto-picking the first device selected an iOS 26 phone and
    # produced a run that could never capture anything.
    UDID=$(cd host && python3 -c "
from rplayhub.transport.usbmux_transport import list_devices, probe_device
for d in list_devices():
    try: v = probe_device(d['udid']).get('ProductVersion','')
    except Exception: v = ''
    major = v.split('.')[0]
    if major.isdigit() and int(major) >= 27:
        print(d['udid']); break
" 2>/dev/null)
    [[ -n "$UDID" ]] || { echo "no iOS 27+ device is visible to usbmuxd — screen viewing needs one" >&2; exit 1; }
fi
say "using $UDID"

say "starting the engine (log: $ENGINE_LOG)"
python3 -u host/mirror.py "$UDID" >"$ENGINE_LOG" 2>&1 &
ENGINE=$!
trap 'kill $ENGINE 2>/dev/null; wait $ENGINE 2>/dev/null' EXIT

for i in $(seq 30); do
    lsof -nP -iTCP:9877 -sTCP:LISTEN >/dev/null 2>&1 && break
    sleep 0.5
    kill -0 $ENGINE 2>/dev/null || { echo "engine exited early:"; tail -20 "$ENGINE_LOG"; exit 1; }
done

say "capturing 10s from :9877 (this counts as the viewer, so the stream starts now)"
python3 - "$CAP" <<'PY'
import socket, sys, time
path = sys.argv[1]
s = socket.create_connection(("127.0.0.1", 9877), timeout=15)
s.settimeout(12)
data, t0 = b"", time.time()
while time.time() - t0 < 10:
    try:
        chunk = s.recv(1 << 16)
    except socket.timeout:
        break
    if not chunk:
        break
    data += chunk
s.close()
open(path, "wb").write(data)

nals = [n for n in data.split(b"\x00\x00\x00\x01")[1:] if n]
from collections import Counter
types = Counter((n[0] >> 1) & 0x3F for n in nals)
names = {32: "VPS", 33: "SPS", 34: "PPS", 19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA",
         1: "TRAIL_R", 0: "TRAIL_N", 39: "SEI", 48: "AP", 49: "FU"}
print(f"    captured {len(data)} bytes, {len(nals)} NAL units")
for t, n in sorted(types.items()):
    print(f"      type {t:>2} x{n:<6} {names.get(t, '?')}")
if any(t in types for t in (48, 49)):
    print("      !! AP/FU types present — those are RTP packetization headers and must never")
    print("         reach the file; the depacketizer is passing packets through unreassembled")
have_params = all(t in types for t in (32, 33, 34))
have_key = any(16 <= t <= 23 for t in types)
print(f"    parameter sets: {'yes' if have_params else 'NO'}   keyframe: {'yes' if have_key else 'NO'}")
PY

say "stopping the engine"
kill $ENGINE 2>/dev/null
wait $ENGINE 2>/dev/null
trap - EXIT

say "decoding with an independent decoder"
if ffprobe -v error -select_streams v:0 -count_frames \
     -show_entries stream=codec_name,width,height,nb_read_frames \
     -of default=noprint_wrappers=1 "$CAP" 2>&1 | sed 's/^/    /'; then :; fi
ffmpeg -v error -i "$CAP" -frames:v 1 -y "$OUT/frame.png" 2>&1 | head -5 | sed 's/^/    /'

echo
if [[ ! -s "$CAP" ]]; then
    say "VERDICT: NO VIDEO WAS CAPTURED — the engine never produced any bytes"
    echo "    This says nothing about decoding. Read the engine log below for why the stream"
    echo "    did not start; a wrong device or a wedged displayservice both land here."
elif [[ -s "$OUT/frame.png" ]]; then
    say "VERDICT: the engine emits decodable video — a frame rendered to $OUT/frame.png"
    echo "    So the remaining fault is in the app's decode path, not the engine."
else
    say "VERDICT: the engine's own bytes do NOT decode"
    echo "    So the app was never the problem. Fix the engine's depacketizer/stream first."
fi
echo
echo "    engine log tail:"
tail -25 "$ENGINE_LOG" | sed 's/^/      /'
