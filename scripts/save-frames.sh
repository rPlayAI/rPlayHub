#!/usr/bin/env bash
#
# Save decoded frames as PNGs, from our live stream or from Apple's captured Device Hub session.
#
#   ./scripts/save-frames.sh live [seconds]     record the running engine, then decode
#   ./scripts/save-frames.sh devicehub          decode the Device Hub capture we took
#
# Why this exists: "it looks garbled" is not something two people can compare, and neither is a
# bitrate number. Frames are. This produces the same artefact for both sources so ours and Apple's
# can be put next to each other.
#
# Recording our own stream goes through the engine's recorder, which is fed from the NAL fan-out
# rather than from a socket — so it does NOT occupy a viewer slot. That matters: the device sends
# exactly one keyframe per session, so a second viewer joining late can never decode, and an
# earlier attempt to capture this way silently ruined the run it was measuring.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

MODE="${1:-live}"
SECONDS_TO_RECORD="${2:-12}"

# The decoder harness runs the app's real parse + decode path, so what we look at is what the app
# would have shown — not a second opinion from a different decoder.
if [[ ! -x build/decodecheck ]] || [[ app/rPlayHub/VideoDecoder.swift -nt build/decodecheck ]]; then
    say "building decodecheck"
    swiftc -O -o build/decodecheck \
        app/tools/decodecheck/main.swift app/rPlayHub/HEVCStream.swift app/rPlayHub/VideoDecoder.swift \
        -framework AVFoundation -framework VideoToolbox -framework CoreMedia \
        -framework QuartzCore -framework ImageIO -framework CoreGraphics \
        || die "could not build decodecheck"
fi

api() {
    printf '%s\n' "$1" | nc -w 5 127.0.0.1 9876 2>/dev/null
}

case "$MODE" in
live)
    OUT="$ROOT/build/live-frames"
    CLIP="$ROOT/build/live-recording.h265"
    api '{"id":1,"method":"ping","params":{}}' | grep -q '"ok"' \
        || die "engine not reachable on 127.0.0.1:9876 — start it with sudo ./scripts/live.sh"

    rm -f "$CLIP"
    say "recording ${SECONDS_TO_RECORD}s — SWIPE NOW"
    api "{\"id\":2,\"method\":\"start_recording\",\"params\":{\"path\":\"$CLIP\"}}" >/dev/null
    for i in $(seq "$SECONDS_TO_RECORD"); do printf "\r  %ds/%ds" "$i" "$SECONDS_TO_RECORD"; sleep 1; done
    echo
    api '{"id":3,"method":"stop_recording","params":{}}' | sed 's/^/  /'
    [[ -s "$CLIP" ]] || die "nothing was recorded"
    ;;
devicehub)
    OUT="$ROOT/build/devicehub-frames"
    CLIP="$ROOT/build/devicehub-recording.h265"
    PCAP=""
    for c in logs/devicehub.pcap reference/captures/devicehub-iphone13-ios27.pcap; do
        [[ -f "$c" ]] && PCAP="$c" && break
    done
    [[ -n "$PCAP" ]] || die "no Device Hub capture found"
    say "depacketizing Apple's own session from $PCAP"
    python3 - "$PCAP" "$CLIP" <<'PY' || die "could not depacketize"
import importlib.util, os, sys
sys.path.insert(0, "host")
import screen
spec = importlib.util.spec_from_file_location("dec", "scripts/decode-remotexpc.py")
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
pcap, out = sys.argv[1], sys.argv[2]
# The video stream is the payload type on the video receiver port; the other one is audio.
pk = []
for lt, frame in d.read_pcap(pcap):
    p = d.ip_payload(lt, frame)
    if not p:
        continue
    _src, _dst, proto, payload = p
    if proto != 17 or len(payload) < 20:
        continue
    b = payload[8:]
    if (b[1] & 0x7F) != 100 or (b[0] >> 6) != 2:
        continue
    pk.append(b)
reorder = screen.ReorderBuffer(window=256)
fu, nals = bytearray(), []
for b in pk:
    payload, seq, is_rtcp, _pt = screen.parse_rtp(b)
    if is_rtcp or not payload:
        continue
    released = []
    reorder.push(seq, payload, released)
    for r in released:
        screen._depacketize(r, fu, nals)
with open(out, "wb") as f:
    for n in nals:
        if n:
            f.write(b"\x00\x00\x00\x01" + n)
print(f"  {len(pk)} RTP packets -> {len(nals)} NALs, lost={reorder.lost}")
PY
    ;;
*)
    die "usage: $0 [live|devicehub] [seconds]"
    ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT"
say "decoding through the app's own path"
RPLAYHUB_FRAMES="$OUT" ./build/decodecheck "$CLIP" 4096 | sed 's/^/  /'
echo
say "frames in $OUT"
ls "$OUT" 2>/dev/null | sed 's/^/    /'
