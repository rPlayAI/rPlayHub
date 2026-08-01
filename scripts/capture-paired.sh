#!/usr/bin/env bash
#
# Capture the wire and Apple's per-frame decoder options AT THE SAME TIME, so the
# out-of-band resolution signal can be located in the RTP stream.
#
# What this is for. avconferenced passes VTDecompressionSessionDecodeFrameWithOptions a per-frame
# dictionary we never knew existed:
#
#     ActiveVideoResolution { Width = 1184; Height = 2576 }   548 frames
#     ActiveVideoResolution { Width = 1088; Height = 1920 }   106 frames
#     ActiveVideoResolution { Width =  720; Height = 1280 }    28 frames
#     ContentAnalyzerCropRectangle { X = 0; Y = 0; ... }
#
# The SPS says 1184x2576 for the whole session and never changes, and the slice headers of full-
# and reduced-resolution frames are structurally identical. So the encoder drops resolution under
# load and says so OUT OF BAND -- which is why ffmpeg, VideoToolbox, every build and every
# configuration produce the same mosaic from these bytes. None of them are told, because the
# information is not in the bitstream.
#
# The RTP header extension profile field takes exactly three values (0x9011, 0x9211, 0x9001) and
# there are exactly three resolutions. That is the hypothesis this run tests -- by pairing the wire
# with Apple's own per-frame ground truth, frame for frame.
#
#   sudo ./scripts/capture-paired.sh [seconds]
#
# Device Hub must be mirroring. Swipe HARD for the whole duration: the reduced resolutions only
# appear under heavy motion.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-25}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="build/paired-$STAMP"
mkdir -p "$OUT"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "needs sudo for tcpdump:  sudo $0 $DUR"
GUI="${SUDO_USER:-$(stat -f%Su /dev/console)}"
runas() { sudo -u "$GUI" "$@"; }

pgrep -x DeviceHub >/dev/null || die "Device Hub is not running."
UTUN="$(ifconfig | awk '/^utun/{n=$1} /inet6 fd/{gsub(":","",n); print n}' | tail -1)"
[ -n "$UTUN" ] || die "no CoreDevice tunnel found"

say "arming the avconferenced interposer"
runas ./scripts/hook-avconferenced.sh arm >/dev/null

say "capturing $UTUN for ${DUR}s"
say "OPEN VIEW SCREEN NOW AND SWIPE HARD -- reduced resolutions only appear under heavy motion"
tcpdump -i "$UTUN" -w "$OUT/session.pcap" -s 0 -U 2>"$OUT/tcpdump.log" &
TPID=$!
sleep "$DUR"
kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null

runas ./scripts/hook-avconferenced.sh disarm >/dev/null
cp /tmp/avconferenced.vtc "$OUT/decoder-input.vtc" 2>/dev/null
chown -R "$GUI" "$OUT" 2>/dev/null

say "tcpdump:"; sed 's/^/     /' "$OUT/tcpdump.log"
say "captured:"
ls -la "$OUT" | sed 's/^/     /'
echo
say "now correlate:"
echo "    python3 scripts/correlate-resolution.py $OUT"
