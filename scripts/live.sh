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
# Sign with a real identity when one exists, not ad-hoc.
#
# This is not cosmetic. TCC keys camera permission on the app's code identity, and an ad-hoc
# signature changes on every single build — so each rebuild looked like a brand-new app, the grant
# never stuck, and macOS eventually recorded a refusal and stopped asking. That is exactly how USB
# capture ended up permanently denied while appearing to be a cable problem.
#
# Override with RPLAYHUB_SIGN_IDENTITY. Falls back to ad-hoc, which still builds and runs — it just
# cannot hold a camera grant across rebuilds.
# Select by SHA-1 hash, never by name: the same certificate name can exist several times, both
# revoked and valid, and xcodebuild picking a revoked one by name fails the whole build.
SIGN_ID="${RPLAYHUB_SIGN_IDENTITY:-}"
if [[ -z "$SIGN_ID" ]]; then
    # As the invoking user, not root. This script runs under sudo, and root sees a different
    # keychain -- which is how a revoked certificate got picked despite the filter: the marker
    # that identifies it as revoked simply was not there in root's view.
    _find_ids() { security find-identity -v -p codesigning 2>/dev/null; }
    if [[ -n "${SUDO_USER:-}" ]]; then
        SIGN_ID=$(sudo -u "$SUDO_USER" security find-identity -v -p codesigning 2>/dev/null \
            | grep -v "CSSMERR_TP_CERT_REVOKED" | grep -oE "[0-9A-F]{40}" | head -1)
    else
        SIGN_ID=$(_find_ids | grep -v "CSSMERR_TP_CERT_REVOKED" | grep -oE "[0-9A-F]{40}" | head -1)
    fi
fi
if [[ -n "$SIGN_ID" ]]; then
    say "signing with identity $SIGN_ID (stable, so the camera grant survives rebuilds)"
else
    SIGN_ID="-"
    say "no valid signing identity — using ad-hoc; camera permission will not stick across rebuilds"
fi

say "building rPlayHub"
LOG="$ROOT/build/xcodebuild.log"
mkdir -p "$ROOT/build"
# Build as the invoking user, not as root.
#
# Only the engine needs root (it creates a utun). Building as root breaks code signing outright:
# codesign cannot reach the user's login keychain, so the private key is unavailable and it fails
# with errSecInternalComponent -- "unable to build chain to self-signed root". Dropping privileges
# for the build is what makes a real signature possible, and a real signature is what lets the
# camera grant survive a rebuild.
build_app() {
    local as_user=()
    if [[ -n "${SUDO_USER:-}" ]]; then
        # Existing root-owned build output would block the unprivileged build.
        chown -R "$SUDO_USER" "$ROOT/build/DerivedData" 2>/dev/null || true
        as_user=(sudo -u "$SUDO_USER")
    fi
    "${as_user[@]}" xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug \
        -derivedDataPath "$ROOT/build/DerivedData" \
        CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$1" \
        DEVELOPMENT_TEAM="" PROVISIONING_PROFILE_SPECIFIER="" \
        build >"$LOG" 2>&1
}
if ! build_app "$SIGN_ID"; then
    if [[ "$SIGN_ID" != "-" ]] && grep -qE "Signing certificate is invalid|not valid for code signing|errSecInternalComponent|CodeSign failed" "$LOG"; then
        # A signing problem must not stop the app from running. Ad-hoc still builds and runs; it
        # just cannot hold a camera grant across rebuilds, which is worth saying out loud.
        say "that identity was rejected — falling back to ad-hoc (re-grant camera each rebuild)"
        SIGN_ID="-"
        build_app "$SIGN_ID" || { grep -E "error:" "$LOG" | head -10 >&2; die "app build failed — full log: $LOG"; }
    else
        grep -E "error:" "$LOG" | head -10 >&2 || true
        die "app build failed — full log: $LOG"
    fi
fi
[[ -d "$APP_BUILD" ]] || die "build produced no app at $APP_BUILD"

# A running copy would keep serving the old code, so replace it after a rebuild.
if pgrep -x rPlayHub >/dev/null; then
    say "restarting rPlayHub with the fresh build"
    pkill -x rPlayHub || true
    sleep 1
fi
# Surface a recorded camera denial, because it is invisible otherwise: the app falls back to the
# CoreDevice stream and simply looks worse, with nothing on screen to say why. The grant is what
# unlocks the capture path, and TCC will not prompt again once a refusal is stored.
# Start each run with a clean app log, so anything reported below is from THIS run. The previous
# version warned about a stale denial from a run that had already been fixed.
mkdir -p "$ROOT/logs"
: > "$ROOT/logs/app.log" 2>/dev/null || true
chown "${SUDO_USER:-root}" "$ROOT/logs/app.log" 2>/dev/null || true

say "opening rPlayHub"
# As the invoking user, never as root.
#
# This script runs under sudo because the engine needs a utun, but a GUI app launched by root is a
# root process — and TCC does not prompt root processes, it denies them outright. That is why
# resetting the camera permission changed nothing and no dialog ever appeared: the request could
# never reach the user. Same reason the build had to drop privileges to reach the keychain.
if [[ -n "${SUDO_USER:-}" ]]; then
    # launchctl asuser, not just sudo -u. Dropping the uid is not sufficient: the process must be
    # placed inside the user's GUI login session, and only there can TCC show a prompt. Launched
    # any other way from a root script the app is simply denied, silently and permanently, which
    # is exactly how the camera permission became impossible to grant.
    launchctl asuser "$(id -u "$SUDO_USER")" sudo -u "$SUDO_USER" open "$APP_BUILD"
else
    open "$APP_BUILD"
fi

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
( sleep 4
  if grep -q "camera permission denied" "$ROOT/logs/app.log" 2>/dev/null; then
      say "camera access is denied, so USB capture cannot run — the picture will be the capped one."
      printf '    to fix:  tccutil reset Camera com.rplay.rplayhub   then rerun and grant the prompt\n'
  elif grep -q "USB capture started" "$ROOT/logs/app.log" 2>/dev/null; then
      say "USB capture is live — the picture is coming from the cable, not the capped stream"
  fi ) &

say "starting the engine as root (Ctrl-C to stop)"
echo
if [[ ${#ARGS[@]} -gt 0 ]]; then
    exec sudo $(rplay_env) python3 host/mirror.py "${ARGS[@]}"
else
    exec sudo $(rplay_env) python3 host/mirror.py "$UDID"
fi
