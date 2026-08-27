#!/bin/bash
#
# remote-xcode-proxy.sh — make a REMOTE iPhone visible to Apple's tools (Xcode / Device Hub /
# devicectl) on this Mac, by spoofing its CoreDevice Bonjour records locally and relaying the
# bytes to the phone over a unicast link (Tailscale, a VPS, plain LAN — anything routable).
#
# This is the article's method (kvnpt, "remote iOS over tailnet"), as actually run and verified
# on 2026-08-27 against a real iPhone 13 Pro over Tailscale. See doc/REMOTE-OVER-TAILSCALE.md.
#
#   WHAT WORKS: discovery + the RemotePairing front door. The phone flips to `available (paired)`
#   and appears in Xcode 26 Devices / Device Hub.
#   WHAT DOES NOT: the trusted tunnel. Any real op fails with RemotePairingError 4 /
#   ControlChannelConnectionError — the CoreDevice tunnel is QUIC to a loopback/link-local
#   endpoint that a dumb socat relay cannot bridge (proven: tcpdump shows no UDP to the phone).
#   Completing it needs a native tunnel endpoint (Architecture B, doc/REMOTEPAIRING-PROTOCOL.md),
#   not this proxy. This script is a proof-of-concept / diagnostic, not a shipping path.
#
# ---------------------------------------------------------------------------------------------
# Values are PER-SESSION and DYNAMIC — read them off the phone's own network each time (mDNS is
# link-local; it does not cross the tunnel), then pass them in. On a machine on the phone's LAN:
#
#     dns-sd -B _remotepairing._tcp local.                 # note the instance (== identifier)
#     dns-sd -L "<identifier>" _remotepairing._tcp local.  # -> SRV port + TXT (authTag, ver)
#     # optional, for the mobdev2 record (helps discovery; its port is usually NOT remote-reachable):
#     dns-sd -L "<instance>" _apple-mobdev2._tcp local.
#
# Then (with the phone reachable at its tailnet/VPS IP, Wi-Fi-associated, Developer Mode on):
#
#     PHONE_IP=100.x.y.z \
#     RP_PORT=56418 RP_ID=BB1F23F8-... RP_AUTHTAG=Oj75pvyy RP_VER=26 \
#     ./scripts/remote-xcode-proxy.sh run
#
# Optional _apple-mobdev2 spoof (adds the `-supportsRP-N` discovery record; no live relay if its
# port is refused over the link, which is the usual case — it is LAN-scoped):
#
#     MOB_INSTANCE='ce:ad:..-supportsRP-26' MOB_PORT=32498 MOB_ID=9BA871D2-... \
#     MOB_AUTHTAG=wymkoCULa9k= MOB_AUTHTAG1=Vg5kqoKCveU= \
#     ... ./scripts/remote-xcode-proxy.sh run
#
# Requires: socat (`brew install socat`), dns-sd (built in), a routed link to PHONE_IP (Tailscale
# in ROUTED mode — userspace mode cannot carry the UDP the tunnel needs; verify `nc -vz PHONE_IP
# RP_PORT` succeeds first).
#
set -euo pipefail

RELAY_HOST="${RELAY_HOST:-rphubrelay.local}"      # a clean *.local that resolves to this Mac
MAC_IP="${MAC_IP:-$(ipconfig getifaddr en1 2>/dev/null || ipconfig getifaddr en0 2>/dev/null || true)}"

usage() { sed -n '2,45p' "$0"; exit 2; }

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 (brew install socat)" >&2; exit 1; }; }

run() {
  need socat; need dns-sd
  : "${PHONE_IP:?set PHONE_IP=<phone tailnet/VPS IP>}"
  : "${RP_PORT:?set RP_PORT=<RemotePairing SRV port, e.g. 56418>}"
  : "${RP_ID:?set RP_ID=<identifier from the _remotepairing TXT>}"
  : "${RP_AUTHTAG:?set RP_AUTHTAG=<authTag from the _remotepairing TXT>}"
  RP_VER="${RP_VER:-26}"; RP_MINVER="${RP_MINVER:-8}"
  [ -n "$MAC_IP" ] || { echo "could not determine this Mac's IP; set MAC_IP=..." >&2; exit 1; }

  echo "relay host $RELAY_HOST -> $MAC_IP ; forwarding to phone $PHONE_IP"
  echo "checking the RemotePairing port is live over the link..."
  nc -vz -G 6 "$PHONE_IP" "$RP_PORT" 2>&1 | tail -1

  local pids=()
  cleanup() { echo; echo "tearing down proxy..."; kill "${pids[@]}" 2>/dev/null || true; }
  trap cleanup EXIT INT TERM

  # --- _remotepairing._tcp: the front door (spoof + TCP/UDP relay) ---
  dns-sd -P "$RP_ID" _remotepairing._tcp local. "$RP_PORT" "$RELAY_HOST" "$MAC_IP" \
    authTag="$RP_AUTHTAG" flags=0 identifier="$RP_ID" minVer="$RP_MINVER" ver="$RP_VER" \
    >/tmp/rxp_rp.log 2>&1 & pids+=($!)
  socat TCP-LISTEN:"$RP_PORT",bind=0.0.0.0,reuseaddr,fork TCP:"$PHONE_IP":"$RP_PORT" \
    >/tmp/rxp_rp_tcp.log 2>&1 & pids+=($!)
  socat UDP-LISTEN:"$RP_PORT",bind=0.0.0.0,reuseaddr,fork UDP:"$PHONE_IP":"$RP_PORT" \
    >/tmp/rxp_rp_udp.log 2>&1 & pids+=($!)

  # --- _apple-mobdev2._tcp: optional discovery record (usually LAN-scoped -> relay is a no-op) ---
  if [ -n "${MOB_INSTANCE:-}" ] && [ -n "${MOB_PORT:-}" ] && [ -n "${MOB_ID:-}" ]; then
    local txt=(identifier="$MOB_ID")
    [ -n "${MOB_AUTHTAG:-}" ]  && txt+=(authTag="$MOB_AUTHTAG")
    [ -n "${MOB_AUTHTAG1:-}" ] && txt+=("authTag#1=$MOB_AUTHTAG1")
    dns-sd -P "$MOB_INSTANCE" _apple-mobdev2._tcp local. "$MOB_PORT" "$RELAY_HOST" "$MAC_IP" \
      "${txt[@]}" >/tmp/rxp_mob.log 2>&1 & pids+=($!)
    socat TCP-LISTEN:"$MOB_PORT",bind=0.0.0.0,reuseaddr,fork TCP:"$PHONE_IP":"$MOB_PORT" \
      >/tmp/rxp_mob_tcp.log 2>&1 & pids+=($!)
    socat UDP-LISTEN:"$MOB_PORT",bind=0.0.0.0,reuseaddr,fork UDP:"$PHONE_IP":"$MOB_PORT" \
      >/tmp/rxp_mob_udp.log 2>&1 & pids+=($!)
    echo "  + _apple-mobdev2 spoof on $MOB_PORT"
  fi

  echo "proxy up (${#pids[@]} helpers). In Xcode: Window > Devices and Simulators — the phone"
  echo "should appear as 'available (paired)'. Real ops will still hit the QUIC-tunnel wall."
  echo "Ctrl-C to tear down."
  wait
}

case "${1:-}" in
  run) shift; run "$@";;
  *)   usage;;
esac
