#!/bin/sh
# Decode our live RTP with somebody else's stack.
#
# The app mirrors every RTP packet, byte for byte, to a loopback port. ffmpeg reads them there
# and does its own depacketization, reordering and decoding. Both receivers therefore see the
# SAME packets, and any difference in what they show is a difference between the two
# implementations rather than between two runs of a phone.
#
#   RPLAY_RTP_FORWARD=5004 open build/dd/Build/Products/Debug/rPlayHub.app
#   ./scripts/independent-receiver.sh
#
# Reading it:
#   ffmpeg shows the same corruption -> our depacketizer is not the cause. The bytes the phone
#     sent already contained it, and no amount of work on this side will change that.
#   ffmpeg is clean and we are not -> the fault is ours, and this gives a working reference to
#     diff against, live, rather than against a capture that may not reproduce it.
#
# Nothing here perturbs the real path: the mirror is a fire-and-forget UDP send on loopback, so a
# slow or absent consumer cannot push back on the receive thread.
set -e

PORT="${1:-5004}"
# The payload NUMBER is chosen by the device in its answer and does not have to match the bank
# we offered -- a live HEVC session was measured on payload type 100, which is the number our
# offer uses for its H.264 bank. Do not infer the codec from it; the payload STRUCTURE decides
# (HEVC types 48/49, H.264 types 24/28), which is what our depacketizer keys on.
PT="${2:-100}"
ENC="${3:-H265}"
SDP="${TMPDIR:-/tmp}/rplayhub-live.sdp"

# The number here must equal what the device actually sends or ffmpeg discards every packet in
# silence -- which is why this harness first appeared to receive nothing while the mirror was
# demonstrably delivering 3 Mbit/s.  usage: independent-receiver.sh [port] [payload-type] [H265|H264]
cat > "$SDP" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=rplay-hub live RTP
c=IN IP4 127.0.0.1
t=0 0
m=video $PORT RTP/AVP $PT
a=rtpmap:$PT $ENC/90000
EOF

echo "SDP:   $SDP"
echo "port:  $PORT   (start the app with RPLAY_RTP_FORWARD=$PORT)"
echo "codec: $ENC on payload type $PT"
echo

if ! command -v ffplay >/dev/null 2>&1; then
    echo "ffplay not found — brew install ffmpeg" >&2
    exit 1
fi

# -fflags nobuffer + -flags low_delay so ffplay shows frames as they arrive rather than building
# a play-out buffer; a buffered player hides exactly the timing effects worth looking at.
exec ffplay -hide_banner \
    -protocol_whitelist file,rtp,udp \
    -fflags nobuffer -flags low_delay \
    -analyzeduration 0 -probesize 32 \
    -i "$SDP"
