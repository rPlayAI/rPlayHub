#!/bin/bash
#
# remote-xcode-proxy.sh — make a REMOTE iPhone visible to Apple's own tools (Xcode, Device Hub,
# xcrun devicectl) on this Mac, by spoofing its Bonjour records locally and relaying the bytes to
# the phone over a unicast link (Tailscale, a VPS, plain LAN — anything that gives a route).
#
# This is Architecture C from doc/REMOTE-SUPPORT.md / doc/REMOTE-OVER-TAILSCALE.md, and it is the
# method from kvnpt's "remote iOS over tailnet" article. It drives APPLE'S CoreDevice stack, NOT
# rplay-hub — so it needs none of our (unbuilt) relay transport. The trick, in one sentence:
# iOS 17+ `remotepairingd` only connects to a phone it discovered via link-local Bonjour on the
# interface that announced it, so we advertise the phone's records pointing at THIS Mac's en0 IP,
# and a dumb socat relay carries en0 -> the phone across the tunnel. TLS/pairing stay end-to-end
# between remotepairingd and the real phone; the relay never inspects anything.
#
#   ./remote-xcode-proxy.sh capture                 # one-time: phone on USB/same-LAN, grab its Bonjour record
#   ./remote-xcode-proxy.sh run <iphone-ip>         # ongoing: spoof + relay to <iphone-ip> (e.g. its tailnet 100.x)
#
# STATUS: spike, following the article; NOT yet validated end-to-end here. Treat a first run as an
# experiment, not a feature. See "Prerequisites" and "Caveats" below.
#
# Prerequisites (all of Apple's remote-device requirements — we do not remove any):
#   * socat + dns-sd (dns-sd is built in; `brew install socat`).
#   * The phone is PAIRED with this Mac (one-time USB trust) and its Developer Disk Image is staged
#     (sticky until reboot; rplay-hub's scripts/activate-after-reboot.sh can re-stage it).
#   * The phone is on REAL Wi-Fi (not cellular-only) — Apple gates developer services on Wi-Fi
#     association; a VPN utun alone does not satisfy it.
#   * A route to the phone's IP exists (Tailscale on both ends, a VPS, or same LAN).
#   * Developer Mode is on.
#
set -euo pipefail

PORT="${RPLAY_RSD_PORT:-49152}"          # RemotePairing / CoreDevice front door
RANGE_LO="${RPLAY_TUNNEL_LO:-55000}"     # per-session dynamic tunnel ports the phone allocates
RANGE_HI="${RPLAY_TUNNEL_HI:-55300}"     # (article observed ~55110-55115; commenters saw wider — widen if needed)
STATE_DIR="${RPLAY_PROXY_STATE:-$HOME/.rplay-xcode-proxy}"
mkdir -p "$STATE_DIR"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }

cmd_capture() {
  need dns-sd
  echo "Discovering the phone's _remotepairing._tcp record (Ctrl-C when you see it resolve)..."
  echo "  Phone must be on USB or the same LAN as this Mac for this one-time capture."
  echo
  echo "1) Browsing — note the instance name that appears:"
  echo "     dns-sd -B _remotepairing._tcp local."
  echo "2) Then resolve it to get host, port, and the TXT (UUID + authTag):"
  echo "     dns-sd -L \"<instance-name>\" _remotepairing._tcp local."
  echo
  echo "Save the instance name, the UUID, and the authTag into $STATE_DIR/record.env like:"
  cat <<'EOF'
     INSTANCE="My-iPhone"
     UUID="XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
     AUTHTAG="....."
EOF
  echo
  echo "Launching the browse now:"
  exec dns-sd -B _remotepairing._tcp local.
}

cmd_run() {
  need dns-sd
  need socat
  local iphone_ip="${1:-}"
  [ -n "$iphone_ip" ] || { echo "usage: $0 run <iphone-ip>" >&2; exit 2; }

  local mac_ip="${RPLAY_MAC_IP:-$(ipconfig getifaddr en0 2>/dev/null || true)}"
  [ -n "$mac_ip" ] || { echo "could not determine this Mac's en0 IP; set RPLAY_MAC_IP=..." >&2; exit 1; }

  # Load the captured record (INSTANCE / UUID / AUTHTAG). The pairing is certificate-based, so the
  # TXT is mostly for discovery; still, spoof it faithfully.
  local INSTANCE="${INSTANCE:-My-iPhone}" UUID="${UUID:-}" AUTHTAG="${AUTHTAG:-}"
  [ -f "$STATE_DIR/record.env" ] && . "$STATE_DIR/record.env"
  local host="${INSTANCE}.local"

  echo "Spoofing Bonjour for '$INSTANCE' -> $host -> $mac_ip, relaying to $iphone_ip"
  echo "  front door tcp/udp $PORT, tunnel range $RANGE_LO-$RANGE_HI"

  local pids=()
  cleanup() { echo; echo "tearing down proxy..."; kill "${pids[@]}" 2>/dev/null || true; }
  trap cleanup EXIT INT TERM

  # 1) Spoof the three records remoted/remotepairingd look for, all pointing at THIS Mac's en0.
  #    dns-sd -P: Name Type Domain Port Host IPaddr [TXT key=val ...]
  local txt=()
  [ -n "$UUID" ]    && txt+=("UUID=$UUID")
  [ -n "$AUTHTAG" ] && txt+=("authTag=$AUTHTAG")
  dns-sd -P "$INSTANCE" _remotepairing._tcp local. "$PORT" "$host" "$mac_ip" "${txt[@]}" & pids+=($!)
  dns-sd -P "$INSTANCE" _remoted._tcp        local. "$PORT" "$host" "$mac_ip"               & pids+=($!)
  dns-sd -P "$INSTANCE" _apple-mobdev2._tcp  local. "$PORT" "$host" "$mac_ip"               & pids+=($!)

  # 2) Relay the front door (TCP and UDP) to the phone.
  socat TCP-LISTEN:"$PORT",bind="$mac_ip",reuseaddr,fork TCP:"$iphone_ip":"$PORT" & pids+=($!)
  socat UDP-LISTEN:"$PORT",bind="$mac_ip",reuseaddr,fork UDP:"$iphone_ip":"$PORT" & pids+=($!)

  # 3) Relay the dynamic per-session tunnel ports. This is a lot of listeners; narrow the range
  #    (RPLAY_TUNNEL_LO/HI) once you observe which ports your phone actually picks (stream_info /
  #    the article's notes) to avoid spawning hundreds of socats.
  local p
  for ((p=RANGE_LO; p<=RANGE_HI; p++)); do
    socat TCP-LISTEN:"$p",bind="$mac_ip",reuseaddr,fork TCP:"$iphone_ip":"$p" & pids+=($!)
    socat UDP-LISTEN:"$p",bind="$mac_ip",reuseaddr,fork UDP:"$iphone_ip":"$p" & pids+=($!)
  done

  echo "proxy up (${#pids[@]} helpers). Now, in another terminal:"
  echo "    xcrun devicectl list devices          # the phone should appear"
  echo "    xcrun devicectl device install app --device <udid> <App.app>"
  echo "Ctrl-C here to tear everything down."
  wait
}

case "${1:-}" in
  capture) shift; cmd_capture "$@";;
  run)     shift; cmd_run "$@";;
  *) echo "usage: $0 {capture | run <iphone-ip>}"; exit 2;;
esac
