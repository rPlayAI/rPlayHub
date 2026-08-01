#!/usr/bin/env bash
#
# Depacketize a captured session with ffmpeg's RTP stack instead of ours.
#
# Our C depacketizer, our vendored Miracast assembler and a Python one written
# from RFC 7798 all produce byte-identical output from
# reference/captures/devicehub-iphone13-ios27.pcap -- and that output decodes to
# visibly garbled pictures, while Device Hub showed the same session cleanly,
# repeatedly, watched closely. Three agreeing implementations that were all
# written here is not independence: doc/RENDERING-HANDOFF.md already records one
# occasion where our C and our Python agreed only because they shared an
# omission. This runs a depacketizer nobody here wrote.
#
# It replays the video RTP over UDP to ffmpeg, which reads it through an SDP so
# its real RTP demuxer and HEVC depacketizer are exercised, not a file parser.
#
#   ./scripts/replay-rtp-to-ffmpeg.sh [pcap] [out.h265]
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PCAP="${1:-reference/captures/devicehub-iphone13-ios27.pcap}"
OUT="${2:-build/replay-ffmpeg.h265}"
PORT="${PORT:-5004}"
SDP="build/replay.sdp"

mkdir -p build

# Payload type 100 is what this device uses for HEVC. That is the number we
# advertise for AVC, which has misled this project before -- the SDP has to say
# H265 regardless of the number, because the number does not identify the codec.
cat > "$SDP" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=rplay-hub replay
c=IN IP4 127.0.0.1
t=0 0
m=video $PORT RTP/AVP 100
a=rtpmap:100 H265/90000
EOF

echo "==> ffmpeg listening on udp/$PORT via $SDP"
ffmpeg -hide_banner -loglevel warning \
       -protocol_whitelist file,udp,rtp \
       -fflags +genpts -use_wallclock_as_timestamps 1 \
       -i "$SDP" -c copy -f hevc -y "$OUT" &
FFPID=$!

sleep 2   # let ffmpeg bind before the first packet arrives

python3 - "$PCAP" "$PORT" <<'PY'
import socket, struct, sys, time

pcap, port = sys.argv[1], int(sys.argv[2])

def packets(path):
    with open(path, "rb") as f:
        h = f.read(24)
        end = "<" if h[:4] == b"\xd4\xc3\xb2\xa1" else ">"
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            _, _, caplen, _ = struct.unpack(end + "IIII", ph)
            d = f.read(caplen)
            if len(d) < caplen:
                return
            yield end, d

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)

sent = 0
for end, frame in packets(pcap):
    if len(frame) < 44:
        continue
    if struct.unpack(end + "I", frame[:4])[0] != 30:      # AF_INET6
        continue
    ip = frame[4:]
    if ip[6] != 17:
        continue
    rest = ip[40:]
    if len(rest) < 8:
        continue
    ulen = struct.unpack(">H", rest[4:6])[0]
    pay = rest[8:ulen] if 8 <= ulen <= len(rest) else rest[8:]
    if len(pay) < 12 or (pay[0] >> 6) != 2:
        continue
    pt = pay[1] & 0x7F
    if pt != 100:                                        # video only
        continue
    sock.sendto(pay, ("127.0.0.1", port))
    sent += 1
    # Paced so the receive buffer never overflows; losing packets here would
    # manufacture exactly the fault we are trying to rule out.
    time.sleep(0.0004)

print(f"replayed {sent} video RTP packets")
PY

sleep 3
kill -INT $FFPID 2>/dev/null
wait $FFPID 2>/dev/null

echo "==> ffmpeg depacketized -> $OUT"
ls -la "$OUT"
