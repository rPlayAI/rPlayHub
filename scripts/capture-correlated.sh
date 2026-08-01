#!/usr/bin/env bash
#
# Capture the RTP packets and Device Hub's window at the same time, with
# timestamps on both, so a garbled decoded frame can be looked up in the window
# at the instant it was displayed.
#
# This is the experiment the whole investigation now rests on. Established so
# far, on two independent captures of Device Hub itself:
#
#   * the capture is complete   -- 0 kernel drops, 0 sequence gaps, 0 dup/lost
#   * depacketization is right  -- our C, our Miracast port, a from-scratch RFC
#                                  7798 Python, and ffmpeg's own all agree byte
#                                  for byte
#   * decode is right           -- ffmpeg and VideoToolbox produce the same
#                                  pictures, with zero reported errors
#   * the pictures are garbled  -- badly, and still obviously garbled when
#                                  downscaled to Device Hub's on-screen size
#   * Device Hub looked clean   -- watched closely, repeatedly
#
# Those cannot all be true of the same bytes. Every previous attempt to resolve
# it compared a decode against a memory. This compares it against a photograph.
#
# An earlier version used `screencapture -v`, which silently produces no file on
# this machine. It uses timestamped stills instead, which do work.
#
#   sudo ./scripts/capture-correlated.sh [seconds]
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-20}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="build/corr-$STAMP"
mkdir -p "$OUT/shots"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "needs sudo for tcpdump:  sudo $0 $DUR"
GUI_USER="${SUDO_USER:-$(stat -f%Su /dev/console)}"
runas() { sudo -u "$GUI_USER" "$@"; }

pgrep -x DeviceHub >/dev/null || die "Device Hub is not running with a live mirror."
pgrep -x rPlayHub  >/dev/null && die "rPlayHub is running -- quit it, a second client splits
     the device's encoder budget."

UTUN="$(ifconfig | awk '/^utun/{n=$1} /inet6 fd/{gsub(":","",n); print n}' | tail -1)"
[ -n "$UTUN" ] || die "no CoreDevice tunnel found"

# Capture only Device Hub's window: a region grab is several times faster than a
# full screen, which is what makes a useful still rate possible at all.
read -r WX WY WW WH < <(runas osascript <<'EOF'
tell application "System Events" to tell process "DeviceHub"
  set p to position of window 1
  set s to size of window 1
  return (item 1 of p as string) & " " & (item 2 of p as string) & " " & ¬
         (item 1 of s as string) & " " & (item 2 of s as string)
end tell
EOF
)
[ -n "${WH:-}" ] || die "cannot read Device Hub's window geometry (Accessibility permission)"
say "Device Hub window ${WW}x${WH} at ${WX},${WY}"

say "capturing on $UTUN for ${DUR}s -- SWIPE THE PHONE FAST, REPEATEDLY"
tcpdump -i "$UTUN" -w "$OUT/session.pcap" -s 0 -U 2>"$OUT/tcpdump.log" &
TPID=$!

# Record when the packet capture began, so still timestamps and packet
# timestamps share an origin.
python3 -c 'import time;print(repr(time.time()))' > "$OUT/t0.txt"

END=$(python3 -c "import time;print(time.time()+$DUR)")
i=0
while [ "$(python3 -c "import time;print(1 if time.time()<$END else 0)")" = "1" ]; do
    i=$((i+1))
    n=$(printf "%04d" $i)
    python3 -c 'import time;print(repr(time.time()))' >> "$OUT/shots/times.txt"
    runas screencapture -x -o -t jpg -R "$WX,$WY,$WW,$WH" "$OUT/shots/w-$n.jpg" 2>/dev/null
done

kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null
say "captured $i window stills"
sed 's/^/     /' "$OUT/tcpdump.log"
grep -qE "(^| )0 packets dropped by kernel" "$OUT/tcpdump.log" \
  || printf '\033[1;31m     ^^ NON-ZERO KERNEL DROPS -- lossy capture, re-run\033[0m\n'

[ -x build/pcapreplay ] || clang -O2 -o build/pcapreplay tools/pcapreplay/pcapreplay.c \
    core/rp_rtp.c core/rp_rtp_assembler.c
./build/pcapreplay "$OUT/session.pcap" "$OUT/video.h265" --stats 2>&1 | sed 's/^/     /'

mkdir -p "$OUT/decoded"
ffmpeg -loglevel error -i "$OUT/video.h265" "$OUT/decoded/f-%04d.png" 2>/dev/null
chown -R "$GUI_USER" "$OUT" 2>/dev/null

cat <<EOF

==> $OUT
    decoded/    what the PACKETS decode to  ($(ls "$OUT/decoded" | wc -l | tr -d ' ') frames)
    shots/      what Device Hub SHOWED      ($i stills, times.txt has epoch times)
    t0.txt      when the packet capture started

Now run:  python3 scripts/correlate-capture.py $OUT
(or just find a garbled decoded/ frame and open the still nearest its timestamp)
EOF
