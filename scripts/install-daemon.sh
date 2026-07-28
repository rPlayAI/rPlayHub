#!/usr/bin/env bash
#
# Install (or remove) the privileged engine as a launchd system daemon.
#
#   sudo ./scripts/install-daemon.sh            install and start
#   sudo ./scripts/install-daemon.sh --remove   stop and uninstall
#   ./scripts/install-daemon.sh --status        no root needed
#
# Same architecture as Apple's: this daemon runs as root and owns the device session; the GUI is
# an ordinary unprivileged process that connects to 127.0.0.1:9876 / :9877. The GUI never needs
# root and must never be run as root.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="com.rplay.rplayhubd"
PLIST_SRC="$ROOT/daemon/$LABEL.plist"
PLIST_DST="/Library/LaunchDaemons/$LABEL.plist"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m==>\033[0m %s\n' "$*" >&2; exit 1; }

status() {
    if launchctl print "system/$LABEL" >/dev/null 2>&1; then
        say "$LABEL is loaded"
        launchctl print "system/$LABEL" 2>/dev/null \
            | grep -E "^\s+(state|pid|last exit code) " | sed 's/^/    /'
    else
        say "$LABEL is not loaded"
    fi
    [[ -f "$PLIST_DST" ]] && echo "    plist installed at $PLIST_DST" \
                          || echo "    no plist at $PLIST_DST"
    if lsof -nP -iTCP:9876 -sTCP:LISTEN >/dev/null 2>&1; then
        echo "    control port 9876 is listening"
    else
        echo "    control port 9876 is NOT listening"
    fi
}

case "${1:-}" in
    --status) status; exit 0 ;;
esac

[[ $EUID -eq 0 ]] || die "run with sudo (installing a system daemon is privileged)"

# launchctl bootout is the modern unload; it is also how we stop cleanly before replacing.
unload() {
    if launchctl print "system/$LABEL" >/dev/null 2>&1; then
        say "stopping $LABEL"
        # SIGTERM reaches the engine, which releases the device's media stream slot. That matters:
        # the device allows one stream, and an abandoned one blocks later sessions.
        launchctl bootout "system/$LABEL" 2>/dev/null || true
        sleep 1
    fi
}

if [[ "${1:-}" == "--remove" ]]; then
    unload
    rm -f "$PLIST_DST"
    say "removed $PLIST_DST"
    exit 0
fi

[[ -f "$PLIST_SRC" ]] || die "missing $PLIST_SRC"
PYTHON="$(command -v python3)"
[[ -n "$PYTHON" ]] || die "python3 not found"

mkdir -p "$ROOT/logs"

unload
say "installing $PLIST_DST"
sed -e "s|__ROOT__|$ROOT|g" -e "s|__PYTHON__|$PYTHON|g" "$PLIST_SRC" > "$PLIST_DST"
chown root:wheel "$PLIST_DST"
chmod 644 "$PLIST_DST"

say "loading"
launchctl bootstrap system "$PLIST_DST"
sleep 2

status
echo
say "the GUI stays unprivileged — launch it normally, never with sudo:"
echo "    open $ROOT/build/DerivedData/Build/Products/Debug/rPlayHub.app"
echo
echo "    logs:    tail -f $ROOT/logs/rplayhubd.log"
echo "    remove:  sudo $0 --remove"
