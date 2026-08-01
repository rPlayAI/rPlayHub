#!/usr/bin/env bash
#
# Capture one session containing a still baseline, SLOW swipes and FAST swipes,
# with the swipes driven synthetically so the only difference between them is
# speed.
#
# Why synthetic. "A slow swipe never artefacts, a fast one always does" is the
# observation that told us the artefacts are rate-bound rather than corruption
# (doc/RENDERING-HANDOFF.md). Turning it into a number needs the two swipes to
# differ in one variable, and a hand swipe is never the same twice -- same trap
# as "change one variable per run" in the traps list. build/uiinput replays the
# identical pixel path at two durations, into whichever window is mirroring.
#
# It also records what the window showed, so the packets and the picture can be
# compared at the same instant instead of from memory.
#
#   sudo ./scripts/capture-swipe-rates.sh [--app DeviceHub|rPlayHub]
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

APP="DeviceHub"
[ "${1:-}" = "--app" ] && APP="${2:-DeviceHub}"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="build/swipe-$STAMP"
mkdir -p "$OUT"

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }
mark() { printf '%s %s\n' "$1" "$(python3 -c 'import time;print(repr(time.time()))')" \
         >> "$OUT/phases.txt"; say "phase: $1"; }

[ "$(id -u)" = "0" ] || die "needs sudo for tcpdump:  sudo $0 ${*:-}"
GUI_USER="${SUDO_USER:-$(stat -f%Su /dev/console)}"
runas() { sudo -u "$GUI_USER" "$@"; }

# Only one client, or the device splits its encoder budget between them and the
# bitrate figures mean nothing.
OTHER=$([ "$APP" = "DeviceHub" ] && echo rPlayHub || echo DeviceHub)
pgrep -x "$OTHER" >/dev/null && die "$OTHER is running -- quit it first, a second viewer
     splits the device's encoder budget and would invalidate the comparison."
pgrep -x "$APP" >/dev/null || die "$APP is not running. Start it, select the iPhone, and
     open the mirrored view so the picture is live, then re-run."

[ -x build/uiinput ] || clang -O2 -o build/uiinput tools/uiinput/uiinput.c \
    -framework ApplicationServices || die "cannot build uiinput"

UTUN="$(ifconfig | awk '/^utun/{n=$1} /inet6 fd/{gsub(":","",n); print n}' | tail -1)"
[ -n "$UTUN" ] || die "no CoreDevice tunnel -- is the device connected and mirroring?"
say "capturing on $UTUN, driving $APP"

# Where to swipe: the middle of the mirrored view. Ask the window server rather
# than guessing, because the window moves between runs.
read -r WX WY WW WH < <(runas osascript <<EOF
tell application "System Events" to tell process "$APP"
  set p to position of window 1
  set s to size of window 1
  return (item 1 of p as string) & " " & (item 2 of p as string) & " " & ¬
         (item 1 of s as string) & " " & (item 2 of s as string)
end tell
EOF
)
[ -n "${WH:-}" ] || die "could not read $APP's window geometry (needs Accessibility permission)"
CX=$(( WX + WW / 2 ))
TOP=$(( WY + WH / 4 ))
BOT=$(( WY + WH * 3 / 4 ))
say "window ${WW}x${WH} at ${WX},${WY} -> swiping x=$CX between y=$BOT and y=$TOP"

PCAP="$OUT/session.pcap"
MOV="$OUT/window.mov"
: > "$OUT/phases.txt"

tcpdump -i "$UTUN" -w "$PCAP" -s 0 -U 2>"$OUT/tcpdump.log" &
TPID=$!
runas screencapture -v -V 60 "$MOV" >/dev/null 2>&1 &
SPID=$!
sleep 2

# --- the experiment ----------------------------------------------------------
mark BASELINE_STILL;  sleep 5

# Same path, same 30 intermediate points, only the duration differs.
mark SLOW_SWIPES
runas ./build/uiinput swipes "$CX" "$BOT" "$CX" "$TOP" 1500 6 700 2>/dev/null

mark SETTLE_1;        sleep 4

mark FAST_SWIPES
runas ./build/uiinput swipes "$CX" "$BOT" "$CX" "$TOP" 120 6 700 2>/dev/null

mark SETTLE_2;        sleep 5
mark END

kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null
kill -INT $SPID 2>/dev/null; wait $SPID 2>/dev/null
say "capture stopped"

say "tcpdump reported:"; sed 's/^/     /' "$OUT/tcpdump.log"
grep -qE "(^| )0 packets dropped by kernel" "$OUT/tcpdump.log" \
  || printf '\033[1;31m     ^^ NON-ZERO KERNEL DROPS -- this capture is lossy, re-run\033[0m\n'

[ -x build/pcapreplay ] || clang -O2 -o build/pcapreplay tools/pcapreplay/pcapreplay.c \
    core/rp_rtp.c core/rp_rtp_assembler.c
./build/pcapreplay "$PCAP" "$OUT/video.h265" --stats 2>&1 | sed 's/^/     /'

chown -R "$GUI_USER" "$OUT" 2>/dev/null

say "analysing slow vs fast"
runas python3 scripts/analyze-swipe-rate.py "$PCAP" "$OUT/video.h265" "$OUT/phases.txt" \
    | tee "$OUT/analysis.txt"

cat <<EOF

==> $OUT
    session.pcap   the packets
    video.h265     depacketized by the shipped C
    window.mov     what $APP actually showed
    analysis.txt   slow vs fast, per phase

To see a frame: ffmpeg -i $OUT/video.h265 -vf "select='eq(n,N)'" -frames:v 1 out.png
EOF
