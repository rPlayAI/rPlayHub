#!/bin/sh
# Record a mirroring window at 60 fps while a scripted swipe runs, then split it into frames.
#
# Every "Device Hub looks clean, ours does not" comparison so far has been by eye, in separate
# sessions, with a hand swipe that is never the same twice. That cannot settle the question --
# and the question now carries the whole investigation, because the capture work proved both
# apps receive the identical stream at the identical bitrate: 6 Mbps negotiated, ~2.7 Mbps
# actual, ghosted pictures under motion that ffmpeg and VideoToolbox both decode without a
# single warning. If Device Hub really is clean on that stream, the difference is in what it
# puts on screen, and stepping through its frames is the only way to see which frames it chose.
#
#   scripts/record-swipe.sh dh   x1 y1 x2 y2      # whatever window is being mirrored
#   scripts/record-swipe.sh ours x1 y1 x2 y2
#
# Coordinates are screen points, origin top-left, same as screencapture reports. Pick a path
# across the mirrored phone: a home-screen page flick is what reproduces the artefact.
set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:?usage: record-swipe.sh <tag> x1 y1 x2 y2}"
X1="${2:?}" Y1="${3:?}" X2="${4:?}" Y2="${5:?}"
OUT="${RECORD_OUT:-$DIR/logs/swipe-$TAG}"
UIINPUT="${UIINPUT:-/tmp/uiinput}"

[ -x "$UIINPUT" ] || clang -O2 -o "$UIINPUT" "$DIR/tools/uiinput/uiinput.c" \
    -framework ApplicationServices -framework CoreGraphics

rm -rf "$OUT"; mkdir -p "$OUT"

# 60 fps so a 33 fps source lands roughly one frame per two captured -- enough to see every
# decoded picture the app presented, without inventing ones it never showed.
ffmpeg -hide_banner -loglevel error -f avfoundation -capture_cursor 0 -framerate 60 \
    -i "0:none" -t 6 -c:v libx264 -preset ultrafast -crf 12 "$OUT/screen.mp4" &
FFPID=$!

sleep 1.5
# Four hard flicks with a short gap: enough motion to starve the encoder, enough pause between
# them that the recovery frames are visible too.
"$UIINPUT" swipes "$X1" "$Y1" "$X2" "$Y2" 220 4 700
wait $FFPID

ffmpeg -hide_banner -loglevel error -i "$OUT/screen.mp4" "$OUT/f%04d.png"
echo "$OUT: $(ls "$OUT"/f*.png | wc -l | tr -d ' ') frames"
echo "step through them, or build a contact sheet:"
echo "  ffmpeg -i $OUT/f%04d.png -vf \"select='not(mod(n,4))',scale=200:-1,tile=6x4\" -frames:v 1 /tmp/$TAG.png -y"
