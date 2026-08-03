#!/usr/bin/env bash
#
# One run of the bitrate experiment: restart the daemon with the flags under test, measure, save.
#
#   sudo ./scripts/rvra-bitrate-run.sh <label> [cdhost flags...]
#
# The daemon is restarted for every run because the offer and the RCTL target are both fixed at
# stream setup, so changing them means a new session. Its log is kept next to the measurement:
# the flag echo at startup is the only proof the override applied, and a run whose override
# silently did not apply looks exactly like a device ignoring it.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

[ "$(id -u)" = "0" ] || { echo "needs sudo: sudo $0 $*" >&2; exit 1; }
[ $# -ge 1 ] || { echo "usage: $0 <label> [cdhost flags...]" >&2; exit 1; }

LABEL="$1"; shift
UDID="${RVRA_UDID:-DEVICE-UDID-REDACTED}"
OUT="build/rvra-bitrate"
mkdir -p "$OUT"

# Only one viewer, or the device splits its encoder budget and the bitrate figures mean nothing.
for app in rPlayHub DeviceHub; do
    pgrep -x "$app" >/dev/null && { echo "$app is running -- quit it first" >&2; exit 1; }
done

pkill -f 'host-c/cdhost' 2>/dev/null
sleep 2

echo "==> $LABEL: cdhost --udid $UDID $*"
./host-c/cdhost --udid "$UDID" "$@" > "$OUT/$LABEL.log" 2>&1 &
CDPID=$!
trap 'kill $CDPID 2>/dev/null' EXIT

# The measurement script waits for the daemon itself; this only catches a daemon that died.
sleep 12
kill -0 $CDPID 2>/dev/null || { echo "cdhost exited early:"; tail -20 "$OUT/$LABEL.log"; exit 1; }

python3 scripts/rvra-bitrate.py --label "$LABEL" --json "$OUT/results.jsonl"
RC=$?

echo "--- daemon said:"
grep -E 'RPLAY_|rctl:|streamConfig|offer\]|media stream|codec' "$OUT/$LABEL.log" | head -12
exit $RC
