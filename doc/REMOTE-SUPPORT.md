# Remote iPhone support — research (2026-08-22)

Question asked: can a phone attached somewhere else — another Mac's USB port, or across the
internet — drive rplay-hub, so "a USB sitting across the internet" works?

**Verdict up front: yes for everything we ship, and the architecture already pays for it.**
The CoreDevice tunnel is just a stream of raw IPv6 packets (`KICKOFF.md`), so a remote link is
one new transport below `TunnelLink` and nothing above it — RSD, screen, HID, screenshots,
recording — changes at all. Two of the three useful shapes are designed already; one is even
prototyped on paper. The one genuinely unvalidated goal is making *Apple's own tools* see a
remote phone; that is separable and should be treated as its own experiment.

Evidence this is not wishful: Apple's own RemotePairing machinery was observed live on this Mac
during today's session (`rapportd` holding established connections to the phone's LAN address),
and usbmuxd's intermittent wifi `Network` entry is the same idea done worse. The mechanism is
real and running on iOS 27; we would be speaking a protocol the devices already answer.

## Three architectures, in build order

### A. Agent relay — phone-side helper, developer-side server. **Build this first.**

```
┌─ phone side ──────────────┐             ┌─ developer side ──────────────┐
│ rplayhub agent            │             │ cdhost --relay                │
│  UsbmuxTransport → link   │◀── TLS ────▶│  RelayTransport → TunnelLink  │
│  (phone on THIS machine's │  outbound   │   → utun → RSD → services     │
│   USB or LAN)             │  dial       └───────────────────────────────┘
└───────────────────────────┘
```

- The agent dials OUT to the server, so NAT/firewall needs no inbound rule on the phone side.
  Any small VPS (or Tailscale) completes an internet path without touching the router.
- Wire protocol already sketched in `host/rplayhub/transport/relay.py`: hello → accept →
  u32-length-prefixed raw IPv6 packets both ways. The length prefix matters because the tunnel's
  16000-byte MTU exceeds typical internet paths — never fragment mid-packet.
- Security: this carries full device control. TLS with pinned identities from day one; never a
  bare TCP listener. (For a first LAN proof, SSH port-forwarding is acceptable auth-by-proxy.)
- Known design points from `relay.py`, still open: two relayed phones can collide on IPv6
  addresses chosen by their phone-side handshakes — per-device utun + routing entry is the
  simple answer and matches what the registry already assumes.
- **What exists:** the `Transport`/`TunnelLink` seam (`host/rplayhub/transport/__init__.py`),
  a working UsbmuxTransport, `RelayTransport` raising NotImplementedError with the plan in its
  docstring. **Effort: days** for a Python prototype proving mirror + touch across two machines;
  then a C port into cdhost.

This alone delivers the user story for OUR app: mirror and control a phone plugged into any
machine that can reach the developer's.

### B. RemotePairing direct door — kill the usbmuxd dependency. **Second.**

Today's wifi path rides Apple's usbmuxd ("Mac-tethered, intermittent Network entry" — seen live
today when the daemon bound the wrong phone because the 13 Pro's entry had not appeared yet).
The direct door replaces it: speak the ControlChannel handshake to the phone over the LAN
ourselves, then `requestTunnelBringup` and hold the tunnel with no Apple daemon involved.

- Fully specified in `doc/REMOTEPAIRING-PROTOCOL.md` down to wire messages, state machine, OPACK
  body encoding, and Bonjour service names — from a symbol dump of the host binary. Pair-setup
  (PIN + consent) once, pair-verify after; keys are generated in-process with **no Secure Enclave
  attestation**, so we can pair legitimately.
- Open items listed there need one live capture each (exact SRP/HKDF parameters, `withKey` shape,
  OPACK schemas of the handshake structs). The capture harness is ready.
- **Effort: weeks.** It is also the prerequisite for C and removes our worst operational wart
  (the intermittent Network entry), so it earns its cost twice. Pair/Unpair as a FEATURE
  (`doc/DEVICEHUB-PARITY.md`) is blocked on exactly this work — same investment, three payoffs.

### C. Make APPLE'S tools see the remote phone — Xcode/Device Hub compatibility. **Spike only, last.**

Our own stack over A/B is the easy half. Xcode does not talk to us; it talks to Apple's daemons,
so the remote device must be injected into THEIR discovery. Two candidate mechanisms, neither
verified (`relay.py` §"the part that is NOT just a transport"):

1. Advertise `_apple-mobdev2._tcp` locally + proxy lockdown → classic usbmuxd Network device.
2. Advertise `_remotepairing._tcp` locally and play the DEVICE role of the ControlChannel toward
   the Mac's remoted, relaying to the real phone — the full proxy recipe is step-by-step in
   `doc/REMOTEPAIRING-PROTOCOL.md` ("Proxy recipe"), including presenting the phone's RSD identity
   inside the tunnel.

Option 2 shares all its wire work with B, which is why B comes first. Until a spike proves remoted
accepts a proxy-paired peer presenting the phone's identity, "remote iPhone works with Xcode" is
an unvalidated goal, not a feature — timebox the experiment and be ready to drop it.

## Internet-path realities (for whichever architecture)

| Constraint | Value / answer |
|---|---|
| Bandwidth | Video negotiates ~4–6 Mbps; fits ordinary home upload. Control traffic is negligible. |
| Latency | Touch feedback inherits full RTT. Mirror stays watchable well past 100 ms RTT; taps start to feel rubbery much past ~150 ms. Prefer a relay endpoint geographically near the phone. |
| MTU | Tunnel wants 16000 bytes; internet gives ~1400. Length-prefixed framing (A) or clamp the tunnel MTU. Already flagged in `relay.py`. |
| NAT | Outbound dial from the phone side (A) needs no port forwarding. B and C are LAN-scoped unless run inside a mesh VPN (Tailscale et al.), which makes them internet-capable unchanged. |
| Security | Full control of a paired phone = treat like a private key. Pinned TLS identities, no anonymous listeners, and the agent should be the only thing that can reach the phone's pairing surface. |

## What remote support does NOT change

- iOS < 26/27 gating: a remote iOS 26 phone binds but cannot mirror, same as a local one.
- RVRA decode constraint: unchanged (`doc/RVRA-AND-PORTABILITY.md`); targets stay Apple-HW Macs.
- Bug #7 liveness: a relayed session can go dead exactly like a local wifi session (phone leaves
  the REMOTE network); whatever liveness check gets built must sit above the transport seam so it
  covers all three architectures.

## Recommended sequence

1. **A-spike (Python):** RelayTransport + agent over SSH-forwarded localhost between two machines on this LAN. Acceptance: rPlayHub mirrors + taps through the relay.
2. **A-product (C):** `cdhost --relay host:port` + standalone agent binary; TLS with pinned keys. Acceptance: same test across a real VPS path, no router changes.
3. **B:** RemotePairing handshake against the live phone, one open item per capture. Payoffs: true wireless door, Pair/Unpair feature, prerequisite for C.
4. **C-spike:** `_remotepairing._tcp` advertisement experiment. Timebox; kill cleanly if remoted rejects the proxy identity.
