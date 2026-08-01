#!/usr/bin/env bash
#
# Capture rPlayHub's own session and run the full validation battery on it.
#
# This is the comparison the whole investigation has been missing. Every capture
# analysed so far has been Device Hub's, and all of them decode to garbled
# pictures -- which was never the complaint. Our own stream has never once been
# captured. Now that usbmuxd exposes the phone over Wi-Fi (ConnectionType=Network),
# both apps can be captured on the identical transport, so a difference between
# them means something.
#
# Compare the output against the Device Hub Wi-Fi baseline, which went through
# exactly the same checks:
#
#     extension disagreements   0 of 364 frames
#     frame-index gaps          0
#     depacketizer cross-check  byte-identical
#     ffmpeg decode errors      0
#     P-frames under 2 kB       39%
#     P mean/median/p95         9,972 / 4,432 / 44,590 B
#
#   sudo ./scripts/capture-ours.sh [seconds]
#
# Start rPlayHub and get the mirror live FIRST, then run this and swipe fast for
# the whole duration.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DUR="${1:-20}"
OUT="build/ours-$(date +%Y%m%d-%H%M%S).pcap"
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "needs sudo:  sudo $0 $DUR"
GUI_USER="${SUDO_USER:-$(stat -f%Su /dev/console)}"

pgrep -x rPlayHub >/dev/null || die "rPlayHub is not running. Start it, get the mirror
     live over Wi-Fi, then re-run."
pgrep -x DeviceHub >/dev/null && die "Device Hub is running -- quit it. A second viewer
     splits the device's encoder budget and would invalidate the comparison."

# The newest CoreDevice tunnel. rPlayHub brings its own up, so this must be read
# after the mirror is live or it will find a stale one.
UTUN="$(ifconfig | awk '/^utun/{n=$1} /inet6 fd/{gsub(":","",n); print n}' | tail -1)"
[ -n "$UTUN" ] || die "no CoreDevice tunnel found -- is the mirror actually live?"

# The video route must be DIRECT. RPLAYHUB_VIDEO defaults to "auto", which falls
# back to the loopback proxy silently -- logs/app.log has already recorded one
# such fallback ("video route: falling back to proxy, direct did not start").
# A capture taken while the proxy is in the path measures a different pipeline
# from the one we mean to test, and nothing in the packets would reveal it.
LAST_DIRECT=$(grep -n "direct stream:" logs/app.log 2>/dev/null | tail -1 | cut -d: -f1)
LAST_PROXY=$(grep -n "video route:.*proxy" logs/app.log 2>/dev/null | tail -1 | cut -d: -f1)
if [ -z "${LAST_DIRECT:-}" ]; then
    die "logs/app.log shows no direct stream at all. Relaunch rPlayHub with
     RPLAYHUB_VIDEO=direct so it fails loudly instead of falling back."
fi
if [ -n "${LAST_PROXY:-}" ] && [ "$LAST_PROXY" -gt "$LAST_DIRECT" ]; then
    die "the most recent video route in logs/app.log is the PROXY, not direct.
     Relaunch with RPLAYHUB_VIDEO=direct and get the mirror live, then re-run."
fi
say "video route: direct (verified from logs/app.log line $LAST_DIRECT)"

say "capturing $UTUN for ${DUR}s"
say "SWIPE THE PHONE FAST, REPEATEDLY, for the whole ${DUR}s"
tcpdump -i "$UTUN" -w "$OUT" -s 0 -U 2>build/ours-tcpdump.log &
TPID=$!
sleep "$DUR"
kill -INT $TPID 2>/dev/null; wait $TPID 2>/dev/null

say "tcpdump:"; sed 's/^/     /' build/ours-tcpdump.log
grep -qE "(^| )0 packets dropped by kernel" build/ours-tcpdump.log \
  || printf '\033[1;31m     ^^ NON-ZERO KERNEL DROPS -- lossy capture, re-run\033[0m\n'

chown "$GUI_USER" "$OUT" 2>/dev/null
say "captured $OUT"
echo
sudo -u "$GUI_USER" ./scripts/validate-capture.sh "$OUT" "rPlayHub-WiFi"
