#!/usr/bin/env bash
#
# Mount the Developer Disk Image after a phone reboot, so rPlayHub works without ever launching
# Xcode or Device Hub. iOS 17+ drops the personalized DDI on every reboot, and the developer
# services (displayservice, appservice, ...) will not answer until one is mounted again. This
# does what Device Hub does silently on connect: query the phone's identity, ask Apple's signing
# server for a per-device ticket, and mount. See doc/DEVICEHUB-PARITY.md and host/ddi_mount.py.
#
# Usage:
#   ./scripts/activate-after-reboot.sh            # wait for the daemon, then mount if needed
#   ./scripts/activate-after-reboot.sh --watch    # keep running; mount whenever a reboot unmounts
#
# The daemon (sudo ./host-c/cdhost) must be running -- this talks to it on :9876 for the tunnel.
# Nothing here needs sudo itself.
set -u
cd "$(dirname "$0")/.."

api() { printf '{"id":1,"method":"%s"}\n' "$1" | nc -w 3 127.0.0.1 9876 2>/dev/null; }

wait_for_tunnel() {
  for i in $(seq 1 60); do
    if api tunnel_info | grep -q '"device_addr"'; then
      [ "$i" -gt 1 ] && echo ""
      return 0
    fi
    [ "$i" -eq 1 ] && printf '%s  waiting for the daemon on :9876 (start it with: sudo ./host-c/cdhost)' "$(date '+%H:%M:%S')"
    printf '.'
    sleep 2
  done
  echo ""
  echo "no live tunnel after 120s -- is 'sudo ./host-c/cdhost' running?" >&2
  return 1
}

mount_once() {
  wait_for_tunnel || return 1
  if python3 host/ddi_mount.py status 2>/dev/null | grep -q "True"; then
    echo "$(date '+%H:%M:%S')  DDI already mounted -- nothing to do"
    return 0
  fi
  echo "$(date '+%H:%M:%S')  no DDI mounted; requesting a ticket from Apple and mounting..."
  if python3 host/ddi_mount.py mount; then
    echo "$(date '+%H:%M:%S')  mounted. The developer services are live; open rPlayHub."
  else
    echo "$(date '+%H:%M:%S')  mount failed -- see the message above" >&2
    return 1
  fi
}

if [ "${1:-}" = "--watch" ]; then
  echo "watching: will mount the DDI whenever the phone reboots (Ctrl-C to stop)"
  while true; do
    mount_once || true
    # Re-check every 30s; a mount is idempotent (status short-circuits when present).
    sleep 30
  done
else
  mount_once
fi
