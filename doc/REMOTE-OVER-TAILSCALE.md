# Remote iPhone over a mesh VPN (Tailscale) — analysis and plan

Written 2026-08-26. Companion to `doc/REMOTE-SUPPORT.md` (the three architectures) and
`doc/REMOTEPAIRING-PROTOCOL.md` (the wire protocol Architecture B needs). This doc is specifically
about the **mesh-VPN shape**: the phone is *with the user, somewhere else*, and the host (a Mac or
a Linux box) is here, with only an internet path between them.

Prompted by kvnpt's write-up, "How to remotely iterate & deploy your sideloaded iOS apps over
tailnet" (dev.to/kvnpt/how-to-remotely-iterate-deploy-your-sideloaded-ios-apps-over-tailnet-jak).

## The scenario

```
   here (host)                         internet                     there (with the user)
┌──────────────────┐                                          ┌──────────────────────┐
│ rPlayHub engine  │◀───────── Tailscale unicast ───────────▶│ iPhone (Wi-Fi + TS)  │
│ (Mac or Linux)   │        route to 100.x.y.z:49152          │ dev services on :49152│
└──────────────────┘                                          └──────────────────────┘
```

No second machine next to the phone — the phone itself is on the tailnet (Tailscale iOS app),
and the host reaches it directly by tailnet IP. This is the shape that answers "a USB port
sitting across the internet" when the user has *only the phone* with them.

## What the article proves (and what it actually builds)

The article deploys apps to a remote iPhone by driving **Apple's own daemons** (`remoted` /
`remotepairingd`) on a Mac and tricking them into connecting across the tunnel. Its machinery
exists entirely to satisfy Apple's link-local invariants:

- **Bonjour spoofing** (`dns-sd -P`): registers the phone's `_remotepairing._tcp` /
  `_remoted._tcp` / `_apple-mobdev2._tcp` records pointing at the Mac's own `en0` IP, so
  `remotepairingd` — which pins its outgoing connection to the interface that announced the record
  (`nw_parameters_set_required_interface`) — discovers the "phone" locally and connects on `en0`.
- **`socat` relays**: dumb byte pipes from `en0:49152` (and the dynamic tunnel range, observed
  ~55000–55300) to the phone's tailnet IP. TLS is end-to-end phone↔remotepairingd; the proxy never
  inspects it.

**The load-bearing lesson for us: the actual CoreDevice data path needs only unicast.** No
multicast, no L2, no broadcast — Tailscale's plain IP route is sufficient. All the Bonjour/socat
scaffolding is there *only* because Apple's daemons insist on local discovery. That is the single
most useful thing the article validates.

## Why rPlayHub can be simpler than the article

rPlayHub does **not** use `remoted`/`remotepairingd`. The engine speaks CoreDevice / RemoteXPC /
RSD itself, and since the lwIP work runs the whole tunnel in userspace, non-root
(`doc/LINUX-PORT-HANDOFF.md`). So for our own mirror/control we can throw almost all of the
article away:

| Article needs | rPlayHub (Architecture B) |
|---|---|
| `dns-sd -P` Bonjour spoof to fool Apple's discovery | **none** — we do our own discovery; the engine is simply *told* the phone's tailnet IP |
| `socat` on a guessed 55000–55300 port range | **none** — we learn the tunnel port from the RSD manifest ourselves |
| A **Mac** to run Apple's daemons | **none** — a Linux box can drive the phone directly |
| Interface-scoping gymnastics | irrelevant — not Apple's daemons |

What remains for us is the honest core: **connect to `phone:49152`, do the RemotePairing
handshake, get RSD, bring up the tunnel** — then everything above the transport (displayservice,
screencaptureservice, universalhidservice, screenshots, recording) is unchanged, exactly as
`REMOTE-SUPPORT.md` predicts. That handshake is Architecture B, specified in
`doc/REMOTEPAIRING-PROTOCOL.md` but **not built**.

## Two ways rPlayHub is actually *more* capable here

1. **Pairing without a prior USB tether.** The article requires an initial USB pairing to create
   the trust record. Our RemotePairing pair-setup flow (PIN + on-device consent, keys generated
   in-process, no Secure Enclave attestation — `REMOTEPAIRING-PROTOCOL.md`) can establish trust
   **over the network**, with a PIN shown on the phone. If that holds up live, a phone that never
   touched this host by cable can still be paired remotely.
2. **DDI re-staging after reboot, over the air.** iOS discards its Developer Disk Image on every
   reboot. The article's answer is "briefly re-tether over USB." We already mount the DDI ourselves
   with no Xcode (`host/ddi_mount.py`, proven live; `scripts/activate-after-reboot.sh`), and that
   mount rides the tunnel — so once a tunnel exists, we can re-stage the DDI remotely. No USB.

## The constraints that bind everyone (article and us)

These are properties of iOS, not of any implementation. Document them wherever remote is offered:

- **Wi-Fi association is mandatory — the hardest one.** Apple exposes developer services only when
  the phone is *associated to a Wi-Fi SSID* (same `WiFiManager` gate as AirDrop/Handoff). A phone
  on cellular-only, or Wi-Fi radio-on-but-not-associated, exposes nothing — and Tailscale's own
  `utun` interface does **not** satisfy the gate. The remote phone must be on real Wi-Fi (home
  Wi-Fi, a tethered hotspot, a café network — any true association).
- **A trust relationship must exist.** USB-once for the article; over-the-air PIN for our
  (unbuilt) Architecture B.
- **Developer Mode on** (needs a reboot + on-device confirm).
- **A staged DDI**, re-staged after each reboot (USB for the article; over-the-tunnel for us).
- **iOS ≥ 27 for screen viewing** — a remote iOS 26 phone binds but cannot mirror, same as a local
  one (`doc/RVRA-AND-PORTABILITY.md`, `doc/DEVICEHUB-PARITY.md`).

## Internet-path realities

From `REMOTE-SUPPORT.md`, unchanged by the VPN shape:

- **Bandwidth**: video negotiates ~4–6 Mbps; fits ordinary home upload. Control traffic is
  negligible.
- **Latency**: touch feedback inherits full RTT; watchable well past 100 ms, taps feel rubbery
  past ~150 ms. Prefer a tailnet path that stays direct (Tailscale DERP relays add hops).
- **MTU**: the tunnel wants 16000 B; the internet gives ~1400. Length-prefixed framing or an MTU
  clamp — already flagged in the relay design.
- **NAT**: a mesh VPN is the clean answer — both ends dial the tailnet, no router changes. (Our
  Architecture A relay, phone-side agent dialing out, is the alternative when there *is* a machine
  next to the phone; the VPN shape is what handles "only the phone is there.")
- **Security**: full control of a paired phone ≈ a private key. Pinned identities, no anonymous
  listeners.

## The spike, concretely

Goal: rPlayHub mirrors + taps a phone that is on a *different* network, reachable only over
Tailscale. Smallest real proof:

1. **Route**: iPhone on Wi-Fi + Tailscale; host on the same tailnet; confirm
   `nc -vz <tailnet-ip> 49152` (or the RSD port) succeeds from the host.
2. **Discovery bypass**: teach the engine a "connect to this IP" path that skips usbmux entirely
   (the transport seam already exists; this is a new transport below `TunnelLink`).
3. **RemotePairing** (`REMOTEPAIRING-PROTOCOL.md`): pair-verify against an already-trusted phone
   first (defer over-the-air pair-setup to a second step), then `requestTunnelBringup`.
4. **Tunnel → RSD → services**: feed the tunnel's raw IPv6 packets into the existing lwIP stack;
   from here nothing new — screen, HID, screenshots all ride it.
5. **Acceptance**: a live mirror and a working tap, host and phone on different networks, no Mac
   required if the host is Linux.

Blocking work is the RemotePairing handshake — the same lift as the Pair/Unpair feature in
`DEVICEHUB-PARITY.md`, and now also the unlock for the original "remote iPhone" objective. One
capture per open item (SRP/HKDF params, OPACK schemas) as listed in the protocol doc.

## Bottom line

The article does **not** let rPlayHub see a remote phone today — rPlayHub is USB/usbmux-only until
Architecture B is built, and the article drives Apple's stack, not ours. What it *does* is confirm
the approach is sound: a unicast mesh VPN is enough transport, the hard constraint is Wi-Fi
association, and the only substantial code we owe is the RemotePairing handshake we already
specced. Build that, and "a USB port across the internet" — the project's original objective —
is real, on Linux, with no Mac in the path.
