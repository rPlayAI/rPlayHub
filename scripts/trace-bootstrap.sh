#!/usr/bin/env bash
#
# Capture how a device becomes reachable — the "bootstrap" we need to reproduce without Device Hub.
#
#   ./scripts/trace-bootstrap.sh [seconds]      default 90
#
# Run this, then do whatever currently makes the device appear (plug in a cable, unlock the phone,
# open Device Hub, select the device in it). Everything below is recorded so the sequence can be
# read afterwards instead of guessed at:
#
#   * mDNS advertisements   — _apple-mobdev2._tcp (wifi sync), _remotepairing._tcp, _rp-tunnel._tcp
#   * usbmuxd's device list — polled once a second, so the moment an entry appears is visible
#   * unified log           — usbmuxd, remoted, remotepairingd, CoreDevice
#
# Why this matters: for Linux and Windows we must trigger the same bootstrap ourselves. Right now
# the phone advertises nothing until something on the Mac provokes it, and which component does
# the provoking is the open question.
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
DUR="${1:-90}"
OUT="$ROOT/logs/bootstrap-$(date +%H%M%S)"
mkdir -p "$OUT"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }

say "tracing for ${DUR}s into $OUT"
say "NOW: do the thing that makes the device appear (cable, unlock, open Device Hub, select it)"
echo

for svc in _apple-mobdev2._tcp _remotepairing._tcp _rp-tunnel._tcp; do
    ( timeout "$DUR" dns-sd -B "$svc" local > "$OUT/mdns$svc.log" 2>&1 & )
done

# The unified log is where Apple's own components narrate what they are doing.
( timeout "$DUR" log stream --style compact --level info \
    --predicate 'process == "usbmuxd" OR process == "remoted" OR process == "remotepairingd" OR subsystem CONTAINS "coredevice" OR subsystem CONTAINS "CoreDevice"' \
    > "$OUT/system.log" 2>&1 & )

( for i in $(seq "$DUR"); do
    printf '%s ' "$(date +%H:%M:%S)"
    (cd host && python3 -c "
from rplayhub.transport.usbmux_transport import list_devices
ds = list_devices()
print(' '.join(f\"{d['udid'][:8]}:{d['connection']}\" for d in ds) or 'none')
" 2>/dev/null || echo "?")
    sleep 1
  done > "$OUT/usbmux.log" 2>&1 & )

for i in $(seq "$DUR"); do
    printf "\r  %ds/%ds" "$i" "$DUR"
    sleep 1
done
echo; echo

say "results"
echo "  --- when did usbmuxd first see each device?"
awk '{ if ($2 != last) { print "      " $0; last = $2 } }' "$OUT/usbmux.log" 2>/dev/null | head -12

echo "  --- mDNS advertisements seen"
for svc in _apple-mobdev2._tcp _remotepairing._tcp _rp-tunnel._tcp; do
    n=$(grep -c "Add" "$OUT/mdns$svc.log" 2>/dev/null || echo 0)
    printf "      %-24s %s\n" "$svc" "$n"
    grep "Add" "$OUT/mdns$svc.log" 2>/dev/null | awk '{for(i=7;i<=NF;i++) printf "%s ", $i; print ""}' \
        | sort -u | sed 's/^/          /'
done

echo "  --- what Apple's components said (first 25 interesting lines)"
grep -iE "pair|tunnel|discover|browse|attach|connect|wake" "$OUT/system.log" 2>/dev/null \
    | head -25 | cut -c1-160 | sed 's/^/      /'

echo
say "full logs in $OUT"
