#!/usr/bin/env bash
#
# Capture a Device Hub session AND what Device Hub's window actually showed, at
# the same time, so the two can be put side by side frame by frame.
#
# Why this and not another plain tcpdump. The existing capture
# (reference/captures/devicehub-iphone13-ios27.pcap) is provably complete: 3849
# of 3849 video packets, no sequence gaps, no duplicates, and every payload byte
# accounted for. Four independent depacketizers -- our C, our vendored Miracast
# assembler, a Python one written from RFC 7798, and ffmpeg's own -- produce
# byte-identical output from it. Two independent decoders, ffmpeg and
# VideoToolbox, then render the same visibly garbled pictures from frame ~46 on,
# while its IDR decodes pixel-perfect.
#
# HEVC is bit-exact: a conforming decoder reproduces the encoder's
# reconstruction exactly, so a stream cannot drift from a clean IDR unless data
# is absent. Nothing is absent from that capture. Yet Device Hub displayed the
# session cleanly, watched closely, repeatedly. Those statements cannot all
# describe the same bytes -- so the missing evidence is not more packet
# analysis, it is a recording of the window next to the packets that fed it.
#
# This records both, timestamped together, and then decodes the capture so the
# same instant can be compared in both.
#
#   sudo ./scripts/capture-and-compare.sh [seconds]
#
# During the capture: swipe the phone's home screen fast, several times.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-20}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="build/capture-$STAMP"
mkdir -p "$OUT"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "needs sudo for tcpdump: sudo $0 $DUR"

# The user who owns the GUI session, so the screen recording is not written as
# root into a place the app cannot reach.
GUI_USER="${SUDO_USER:-$(stat -f%Su /dev/console)}"

# --- 1. no second client -----------------------------------------------------
# A second viewer makes the device split its encoder budget, which would produce
# a starved capture and invalidate the comparison. This is the single most
# likely explanation for the previous capture and must be excluded by control,
# not by memory.
if pgrep -x rPlayHub >/dev/null 2>&1; then
    die "rPlayHub is running. Quit it first (osascript -e 'tell application \"rPlayHub\" to quit')
     -- a second client splits the device's encoder budget and starves the stream."
fi
say "no rPlayHub running: Device Hub will be the only client"

# --- 2. find the tunnel ------------------------------------------------------
# Connect the device in Device Hub but do NOT open View Screen yet: the tunnel
# must exist so we can find it, while startmediastream must still be ahead of us.
UTUN="$(ifconfig | awk '/^utun/{n=$1} /inet6 fd/{gsub(":","",n); print n}' | tail -1)"
[ -n "$UTUN" ] || die "no CoreDevice tunnel found. In Device Hub, select the iPhone
     (so the tunnel comes up) but do NOT press View Screen yet, then re-run."
say "capturing on $UTUN"

# --- 3. start both recorders -------------------------------------------------
PCAP="$OUT/session.pcap"
MOV="$OUT/screen.mov"

# -s 0 because the tunnel is cleartext and we need whole packets. The kernel
# drop count printed at the end is the thing to read: a lossy capture would
# manufacture exactly the fault we are chasing.
tcpdump -i "$UTUN" -w "$PCAP" -s 0 -U 2>"$OUT/tcpdump.log" &
TPID=$!

# screencapture -v records the screen to a movie. Run it as the GUI user so the
# file is theirs and the screen-recording permission applies to their session.
sudo -u "$GUI_USER" screencapture -v -V "$DUR" "$MOV" >/dev/null 2>&1 &
SPID=$!

sleep 1
say "RECORDING for ${DUR}s"
say "  -> press View Screen now if it is not open, then SWIPE THE HOME SCREEN FAST, repeatedly"
sleep "$DUR"

kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null
wait $SPID 2>/dev/null
say "stopped"

# --- 4. report capture health ------------------------------------------------
say "tcpdump reported:"
sed 's/^/     /' "$OUT/tcpdump.log"
echo
grep -q "0 packets dropped by kernel" "$OUT/tcpdump.log" \
  || printf '\033[1;31m     ^^ NON-ZERO KERNEL DROPS: this capture is lossy and cannot be trusted\033[0m\n'

# --- 5. decode the capture ---------------------------------------------------
[ -x build/pcapreplay ] || clang -O2 -o build/pcapreplay tools/pcapreplay/pcapreplay.c \
    core/rp_rtp.c core/rp_rtp_assembler.c
./build/pcapreplay "$PCAP" "$OUT/video.h265" --stats 2>&1 | sed 's/^/     /'

say "reference structure (POC continuity, dangling references):"
python3 scripts/hevc-refs.py "$OUT/video.h265" 2>&1 | sed -n '2,12p' | sed 's/^/     /'

mkdir -p "$OUT/decoded"
ffmpeg -loglevel error -i "$OUT/video.h265" "$OUT/decoded/f-%04d.png" 2>/dev/null
say "decoded $(ls "$OUT/decoded" | wc -l | tr -d ' ') frames -> $OUT/decoded/"

mkdir -p "$OUT/window"
ffmpeg -loglevel error -i "$MOV" -vf fps=30 "$OUT/window/w-%04d.png" 2>/dev/null
say "extracted $(ls "$OUT/window" | wc -l | tr -d ' ') window frames -> $OUT/window/"

chown -R "$GUI_USER" "$OUT" 2>/dev/null

cat <<EOF

==> done: $OUT

    $OUT/decoded/  what the captured PACKETS decode to
    $OUT/window/   what Device Hub actually SHOWED

Find a garbled frame in decoded/, note its number N, and look at the window
frame for the same instant. The stream runs about 40 fps and the window was
sampled at 30, so window frame ~= N * 30/40.

If the window is clean where the packets are garbled, then Device Hub is not
displaying this RTP stream, and the whole media-plane investigation has been
aimed at the wrong transport.
EOF
