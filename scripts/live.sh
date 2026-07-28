#!/usr/bin/env bash
#
# Live mirroring and control: start the engine against a real phone and open rPlayHub.
#
#   ./scripts/live.sh                              # first mirroring-capable device
#   ./scripts/live.sh DEVICE-UDID-REDACTED
#   ./scripts/live.sh --codec h264                 # ask the device for H.264 instead of HEVC
#
# Any extra arguments are passed straight through to host/mirror.py.
#
# Only the engine runs as root — creating the tunnel interface is privileged. The app runs as
# you, deliberately: it never needs root, and running a GUI as root is a bad idea besides.
# You will be prompted for your password once, for the engine.
#
# Ctrl-C stops the engine and tears the tunnel down. The app can stay open; it reconnects.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ARGS=("$@")
UDID=""
for a in "$@"; do case "$a" in -*) ;; *) UDID="$a"; break ;; esac; done
APP_BUILD="$ROOT/build/DerivedData/Build/Products/Debug/rPlayHub.app"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- port check
# The replay engine (scripts/mock_engine.py) uses the same two ports. If it is still running the
# real engine cannot bind, and the app would silently keep talking to the replay.
for port in 9876 9877; do
    if lsof -nP -iTCP:$port -sTCP:LISTEN >/dev/null 2>&1; then
        holder=$(lsof -nP -iTCP:$port -sTCP:LISTEN -Fc 2>/dev/null | grep '^c' | head -1 | cut -c2-)
        die "port $port is already in use by '${holder:-unknown}'.
    If that is the replay engine:  pkill -f mock_engine"
    fi
done

# ---------------------------------------------------------------- device check
# One pass: list every device with its iOS version, because the choice depends on the version.
# Screen viewing needs iOS 27+ — Apple's own Device Hub reports the same limitation — so an
# iOS 26 device is not a candidate even when it is the only one plugged in.
say "looking for a device"
DEVICES=$(cd host && python3 -c "
from rplayhub.transport.usbmux_transport import list_devices, probe_device
for d in list_devices():
    try:
        v = probe_device(d['udid']).get('ProductVersion') or '?'
    except Exception:
        v = '?'
    print(d['udid'], d['connection'], v)
" 2>/dev/null)
[[ -n "$DEVICES" ]] || die "no devices visible to usbmuxd — plug one in, or wake the wifi one"

while read -r u c v; do
    case "$v" in
        [3-9][0-9].*|2[7-9].*) mark="can mirror" ;;
        \?)                    mark="version unknown" ;;
        *)                     mark="iOS $v — CANNOT mirror" ;;
    esac
    printf '    %-40s %-8s iOS %-7s %s\n' "$u" "$c" "$v" "$mark"
done <<< "$DEVICES"

if [[ -z "$UDID" ]]; then
    UDID=$(awk '$3 ~ /^(2[7-9]|[3-9][0-9])\./ {print $1; exit}' <<< "$DEVICES")
    if [[ -n "$UDID" ]]; then
        say "using $UDID (first mirroring-capable device)"
    else
        die "no mirroring-capable device is visible to usbmuxd.

    Screen viewing needs iOS 27+ — the device says so itself:
      \"Remote control requires iOS 27.0 or later on this device.\"

    If your iOS 27 device is powered on and Device Hub can see it, the problem is NOT that the
    phone is unreachable: it is that we only reach devices through usbmuxd, and after a reboot a
    wifi device often stops registering there. Apple\'s stack reaches it a different way
    (remoted builds its own tunnel), which we do not implement yet.

    Fastest fix:  PLUG THE PHONE IN WITH A USB CABLE.
                  usbmux registers it immediately and the connection is far more stable than the
                  wifi entry, which iOS sleeps aggressively.

    Also worth knowing: close Device Hub\'s View Screen before testing — the device allows only
    ONE media stream, and Device Hub holds it.

    To force a specific device anyway (screenshots and touch may still work on iOS 26):
      ./scripts/live.sh <udid>"
    fi
fi

# ---------------------------------------------------------------- app
# Always build. The previous version built only when the bundle was MISSING, so an existing
# bundle was launched forever while the sources moved on — a stale app silently lacking every
# recent fix (this is how a rebuilt sidebar failed to appear). Incremental builds cost seconds.
say "building rPlayHub"
LOG="$ROOT/build/xcodebuild.log"
mkdir -p "$ROOT/build"
if ! xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug \
        -derivedDataPath "$ROOT/build/DerivedData" \
        CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="-" \
        DEVELOPMENT_TEAM="" PROVISIONING_PROFILE_SPECIFIER="" \
        build >"$LOG" 2>&1; then
    grep -E "error:" "$LOG" | head -10 >&2 || true
    die "app build failed — full log: $LOG"
fi
[[ -d "$APP_BUILD" ]] || die "build produced no app at $APP_BUILD"

# A running copy would keep serving the old code, so replace it after a rebuild.
if pgrep -x rPlayHub >/dev/null; then
    say "restarting rPlayHub with the fresh build"
    pkill -x rPlayHub || true
    sleep 1
fi
say "opening rPlayHub"
open "$APP_BUILD"

# ---------------------------------------------------------------- engine
# sudo drops the environment, so the RPLAY_* experiment knobs would silently never reach the
# engine — and an experiment that quietly tests the default is worse than no experiment. Forward
# them explicitly and say which ones are in play.
rplay_env() {
    local v out=""
    for v in ${!RPLAY_@}; do out+="$v=${!v} "; done
    [[ -n "$out" ]] && printf 'env %s' "$out"
}
if [[ -n "$(rplay_env)" ]]; then
    say "experiment knobs: $(rplay_env | sed 's/^env //')"
fi
say "starting the engine as root (Ctrl-C to stop)"
echo
if [[ ${#ARGS[@]} -gt 0 ]]; then
    exec sudo $(rplay_env) python3 host/mirror.py "${ARGS[@]}"
else
    exec sudo $(rplay_env) python3 host/mirror.py "$UDID"
fi
