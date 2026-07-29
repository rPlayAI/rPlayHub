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
SDP="${TMPDIR:-/tmp}/rplayhub-live.sdp"

# Payload type 123 is what our offer asks for; the device echoes it. It has to match or ffmpeg
# will drop every packet without saying why.
cat > "$SDP" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=rplay-hub live RTP
c=IN IP4 127.0.0.1
t=0 0
m=video $PORT RTP/AVP 123
a=rtpmap:123 H265/90000
EOF

echo "SDP:   $SDP"
echo "port:  $PORT   (start the app with RPLAY_RTP_FORWARD=$PORT)"
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
