#!/usr/bin/env bash
#
# Replay Apple's captured Device Hub session through the C depacketizer and compare the result
# against the reference Annex-B stream, byte for byte.
#
# This is the strongest offline test available: the input is real device output, and the expected
# result is a file produced independently (and which ffmpeg's own depacketizer agrees with).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PCAP=""
for c in logs/devicehub.pcap reference/captures/devicehub-iphone13-ios27.pcap; do
    [[ -f "$c" ]] && PCAP="$c" && break
done
REF=reference/captures/apple_video_REFERENCE.h265
[[ -n "$PCAP" && -f "$REF" ]] || { echo "  need a capture and $REF"; exit 0; }

make -C core test_rp_rtp >/dev/null || exit 1

python3 - "$PCAP" > /tmp/rtp-replay.bin <<'PY'
import importlib.util, sys, struct
spec = importlib.util.spec_from_file_location("dec", "scripts/decode-remotexpc.py")
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
out = sys.stdout.buffer
for lt, frame in d.read_pcap(sys.argv[1]):
    p = d.ip_payload(lt, frame)
    if not p:
        continue
    _s, _d, proto, payload = p
    if proto != 17 or len(payload) < 20:
        continue
    b = payload[8:]
    if (b[1] & 0x7F) != 100 or (b[0] >> 6) != 2:
        continue
    out.write(struct.pack(">H", len(b)) + b)
PY

./core/test_rp_rtp < /tmp/rtp-replay.bin > /tmp/rtp-out.h265
if cmp -s /tmp/rtp-out.h265 "$REF"; then
    echo "  depacketized output is BYTE-IDENTICAL to $REF ($(wc -c < /tmp/rtp-out.h265) bytes)"
else
    echo "  DIFFERS from the reference:"
    echo "    ours $(wc -c < /tmp/rtp-out.h265) bytes, reference $(wc -c < "$REF") bytes"
    cmp /tmp/rtp-out.h265 "$REF" | head -3
    exit 1
fi
