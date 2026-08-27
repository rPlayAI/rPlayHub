# Remote iPhone — the option space, the mesh-VPN path, and the agent spike

Written 2026-08-26. Companion to `doc/REMOTE-SUPPORT.md` (the original three-architecture framing)
and `doc/REMOTEPAIRING-PROTOCOL.md` (the wire protocol Architecture B needs). The goal is the
project's founding objective: **a USB port sitting across the internet** — remote viewing and
control, and, where possible, Apple's own tools (Xcode / Device Hub) reaching the same phone.

This doc surveys the full option space, then details the two things worth building first: the
**mesh-VPN (Tailscale) path** for a phone that is alone with the user, and the **agent spike** for
the far more common case where *some* machine sits next to the phone. Prompted by kvnpt's write-up,
"How to remotely iterate & deploy your sideloaded iOS apps over tailnet"
(dev.to/kvnpt/how-to-remotely-iterate-deploy-your-sideloaded-ios-apps-over-tailnet-jak).

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

## The deciding variable: is there a machine next to the phone?

Every remote approach forks here.

- **Yes — any box at the phone's site** (a laptop, a Mac mini, a $60 Raspberry Pi): the phone
  plugs into it over USB and a small **rPlayHub agent** dials *out* to the host. No RemotePairing,
  no Wi-Fi-association gate, no inbound NAT. This is **Architecture A**, the least new code —
  build it first (spike at the end).
- **No — only the phone travels**: the phone itself must reach the host over IP, which forces the
  CoreDevice network path (**Architecture B**, RemotePairing) and the Wi-Fi-association gate. More
  capable, more to build. The mesh-VPN analysis above is this case.

## The full option space

### Remote viewing & control (our own stack)

| Approach | Box at the phone? | New code | Notes |
|---|---|---|---|
| **A. Agent relay** | yes | small (RelayTransport + agent) | fastest to a demo; transport is a menu below |
| **B. RemotePairing direct** | no | large (the handshake) | phone-only; the mesh-VPN path |
| **ReplayKit broadcast** | no | a separate iOS app | *viewing only, no control*; escapes the Wi-Fi gate (works on cellular); a different product |

Transport choices for **A** (how the agent reaches the host):
- **Mesh VPN (Tailscale)** — cleanest, direct P2P when possible, no router config.
- **SSH reverse tunnel** (`ssh -R`) — zero infra, ideal for the first localhost/LAN proof.
- **Rendezvous VPS** — both ends dial a cheap cloud box; solves double-NAT without a full mesh; put it near the phone to keep RTT down.
- **WebRTC / ICE (STUN+TURN)** — P2P with no VPN dependency; most work, best "from anywhere."

### Xcode / Device Hub access (Apple's stack)

Xcode talks to Apple's daemons, not to us, so a relayed phone must be injected into *Apple's*
discovery. Worst-to-best:

1. **usbmux Network device** (advertise `_apple-mobdev2._tcp` + proxy lockdown) — surfaces the
   phone to Finder/lockdown-era tooling, but **not** the CoreDevice tunnel modern Xcode debugging
   needs. Partial.
2. **The article's method** — Bonjour-spoof `_remotepairing._tcp` on the host Mac's `en0` + relay
   to the remote phone; Apple's `remoted` (hence Xcode, Device Hub, `devicectl`) sees it. Requires
   a Mac (Xcode needs one anyway). Works today-ish; this is C-option-2 in `REMOTE-SUPPORT.md`.
3. **The unification proxy** — build the engine as one CoreDevice proxy that (a) holds the real
   tunnel to the remote phone via our own RemotePairing (B) and (b) re-advertises it locally via
   Bonjour so Apple's `remoted`/Xcode connect *through us*. One component then serves rPlayHub's
   viewing/control **and** Xcode from the same tunnel — the phone appears local to everything on
   the Mac. B's tunnel + C's advertisement, fused; the satisfying end state.

## The recommended ladder

1. **Premise test (no code):** confirm the phone's CoreDevice door is reachable over the tailnet
   at all — see "Testing it" next. Make-or-break, five minutes.
2. **Architecture A** over `ssh -R` then Tailscale (spike below). rPlayHub remote view+control the
   soonest. If the phone-side box is a Mac, the article's method on it adds Xcode access nearly
   for free.
3. **Architecture B** (RemotePairing) — the phone-only unlock, also the Pair/Unpair feature and
   the prerequisite for the unification proxy.
4. **The unification proxy** — one engine serving rPlayHub *and* Xcode from the remote tunnel.

## What is already proven, and what actually remains

**CoreDevice-over-IP on the LAN is already verified** — rPlayHub mirrors and controls the phone
over home Wi-Fi, confirmed many times. So "does iOS expose its developer services over IP when
Wi-Fi-associated" is answered *yes*; no probe needed for that. But note *how* today's Wi-Fi path
works, because it is exactly what does not generalize to the internet: **after a one-time USB
pairing handshake, usbmuxd exposes the phone over Wi-Fi** — usbmuxd on the Mac discovers the
already-paired device via link-local mDNS (`_apple-mobdev2._tcp`) and carries the connection
(`REMOTE-SUPPORT.md` — "the wifi path rides Apple's usbmuxd, Mac-tethered, intermittent Network
entry"). Two properties of that path are the whole reason remote needs more: the discovery is
**LAN-scoped** (mDNS does not cross routers or the internet), and it is **Mac-tethered** (it needs
Apple's usbmuxd running on the same LAN as the phone). It also still requires that **prior USB
handshake** to exist.

So the CoreDevice *protocol* over IP is not what's unproven — the only thing remote adds is a
**transport that carries it across networks**. Two unverified pieces remain, and they are the only
reasons to test anything:

1. **Can the phone be reached directly on `:49152` (not via usbmuxd) across a unicast VPN?** This
   is the Architecture-B question — a *direct* RemotePairing connect, which the LAN/usbmux path
   never exercises. This is what the Tailscale premise probe below actually checks.
2. **Does a relay carry the working usbmux/Wi-Fi tunnel across the internet unchanged?** This is
   the Architecture-A question, answered by the agent spike, not by a probe.

## Tailscale premise probe (checks piece 1 only; needs no rPlayHub code)

Does the phone answer a *direct* connection to its CoreDevice door over the tailnet — the one
thing LAN mirroring via usbmuxd doesn't prove?

1. **Host (this Mac):** install Tailscale — `brew install tailscale` for the CLI, or the Mac app —
   then `sudo tailscale up` and authenticate to your tailnet.
2. **iPhone:** install the Tailscale app from the App Store, sign into the **same** tailnet, and
   put the phone on **real Wi-Fi** (not cellular-only — the `WiFiManager` gate). Read its tailnet
   IP (`100.x.y.z`) from the Tailscale app.
3. **From the host**, probe the RemotePairing front door:
   ```sh
   nc -vz <iphone-tailnet-ip> 49152     # CoreDevice / RemotePairing door
   ```
   - **Connects** → premise holds: the dev-services door is reachable over unicast Tailscale, and
     Architecture B is worth building. (The article's `socat` targets exactly this
     `iPhone_tailnet_ip:49152`, so a modern iOS RemotePairing listener does bind the tailnet
     interface when Wi-Fi-associated.)
   - **Refused / times out** → the phone isn't Wi-Fi-associated, Developer Mode is off, or the port
     isn't exposed on that interface. Fix the gate before building.

**What this does *not* test:** rPlayHub actually driving the phone — that needs Architecture B (not
built). A passing premise test is the green light to build B, not a working mirror yet. For a
*working* remote mirror the soonest, use the Architecture-A agent below with a second machine at the
phone's site.

## Architecture A — the agent spike (build this first)

The cheapest path to a real remote mirror. Today's engine is unchanged above the transport seam;
all that is new is a byte pump.

```
  phone site (any box, phone on USB)                 host site (rPlayHub + app)
┌───────────────────────────────┐                 ┌──────────────────────────────┐
│ cdhost --agent <host:port>    │                 │ cdhost --relay-listen :port  │
│  usbmux → CoreDeviceProxy      │◀── TLS ────────▶│  relay socket → usernet(lwIP)│
│  → raw-IPv6 tunnel conn        │  raw IPv6 pkts  │  → RSD → displayservice/HID  │
│  → pump conn ⇄ TLS socket      │                 │  app connects :9876 / :9877  │
└───────────────────────────────┘                 └──────────────────────────────┘
```

**The splice point already exists.** `usernet_start(conn, our_addr, dev_addr)` drives the lwIP
stack off a connection it reads raw IPv6 packets from via `imd_conn_recv` / `imd_conn_send`
(`host-c/usernet.c`). Today that `conn` is the CoreDeviceProxy tunnel; for the relay host, hand
`usernet_start` a connection whose recv/send are the relay socket instead — nothing above changes.
The Python seam mirrors this: `Transport.open() → TunnelLink(stream, params)` with `stream`
carrying raw IPv6 packets (`host/rplayhub/transport/__init__.py`); `relay.RelayTransport` is the
stub to fill in.

**Wire protocol** (trivial; refines `relay.py`'s docstring):
- On connect, the **agent** sends a one-time JSON preamble = the tunnel params
  `{our_addr, dev_addr, rsd_port, mtu}` (u32-length-prefixed).
- Then **raw IPv6 packets both ways, verbatim.** Prefer self-delimiting packets — each IPv6 header
  carries its own payload length, which is how `usernet.c`'s reader already frames them — so the
  relay is a byte-identical pipe and the host stack is unchanged. TCP segments the stream, so the
  16000-vs-1400 MTU gap needs no IP fragmentation; large tunnel packets ride fine. (The u32
  per-packet framing in the old docstring is an equivalent alternative, not a requirement.)

**Two cdhost modes**
- `cdhost --agent <host:port>`: run the normal usbmux→CoreDeviceProxy path to get the tunnel conn,
  TLS-connect *out* to the host (so the phone site needs no inbound NAT rule), send the params
  preamble, then pump conn⇄socket until either side closes.
- `cdhost --relay-listen :port`: accept the agent (TLS), read the params, feed `usernet_start` a
  relay-backed conn, then serve exactly as a local session — the app talks to :9876/:9877 unaware.

**Transport for the first proof:** skip TLS/VPN at first — `ssh -R` from the phone-site box to the
host, agent dials `localhost:port`. Once green, swap in Tailscale (both boxes on the tailnet, agent
dials the host's tailnet IP) for the real internet path; add pinned-TLS as production auth — this
carries full device control, never a bare listener.

**Acceptance:** rPlayHub on the host mirrors and taps a phone plugged into a *different* machine —
the two connected first by `ssh -R`, then by Tailscale across networks. A Linux phone-site box also
proves the no-Mac path.

**Open items** (small): one device for the spike, so ignore the IPv6-address collision noted in
`relay.py` (multi-device gets a per-device utun/route the registry already assumes); liveness
(bug #7) sits *above* the transport, so a relayed session can go dead like a local Wi-Fi one and
its check must cover all transports.

**Effort:** days — the pump, the two modes, and an `ssh -R` proof; the C splice is small because
`usernet_start` already takes an arbitrary conn.

## Architecture B spike — phone-only, over the mesh VPN

The phone-only path: no box at the phone's site, so RemotePairing does the work. Goal: rPlayHub
mirrors + taps a phone that is on a *different* network, reachable only over Tailscale. Smallest
real proof:

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
Architecture A or B is built, and the article drives Apple's stack, not ours. What it *does* is
confirm the approach is sound: a unicast mesh VPN is enough transport, and the hard constraint is
Wi-Fi association. The fastest route to a working remote mirror is **Architecture A** (a byte pump
over `ssh -R`, then Tailscale) with any box next to the phone; the phone-only unlock is
**Architecture B** (the RemotePairing handshake we already specced). Either way, "a USB port
across the internet" — the project's original objective — is real, on Linux, with no Mac in the
path. Run the premise test first; it costs five minutes and decides whether B is worth the build.

## Live experiment — Xcode/Device Hub over Tailscale (2026-08-27)

Ran the article's method end-to-end against a real remote iPhone 13 Pro (iOS 27) over Tailscale.
**Result: Apple's Device Hub discovers the remote phone and the RemotePairing front door connects
through the proxy — the trusted QUIC tunnel is the one remaining wall.** Concrete findings, so this
isn't re-learned:

**Setup that works.**
- Tailscale must be **routed mode** (a real `utun`), not userspace — `socat` needs to route to the
  phone's `100.x`, and the CoreDevice tunnel is UDP. Rootless userspace mode (`--tun=userspace-networking`)
  is fine only for the reachability probe via `tailscale nc`.
- The phone must stay **Wi-Fi-associated AND Tailscale-connected**; iOS drops the tunnel when the
  Tailscale app backgrounds or the phone sleeps (seen repeatedly — `offline, last seen …, tx N rx 0`).
  Keep the Tailscale app foreground / phone awake.

**The RemotePairing port is DYNAMIC — not 49152.** The article's `49152` is that author's value; our
phone advertised **`56418`** (verified `Connection refused` on 49152, `succeeded` on 56418 over the
tailnet). The port is in the phone's link-local Bonjour `_remotepairing._tcp` record, which does NOT
cross the tailnet — so it must be read on the phone's own network (`dns-sd -L …`). It also changes
across sessions/reboots.

**The spoof must mirror the phone's real TXT exactly.** The `identifier` is the **RemotePairing
identity** (e.g. `BB1F23F8-…`), *different* from the CoreDevice/`devicectl` id (`F96F2729-…`), and
there is an `authTag` plus `ver` (26 here, not the article's 24). With the wrong identifier `remoted`
reports `CurrentlyAssertableStates = ()` (no path); with the exact record it advances to actually
dialing. The SRV host must resolve to this Mac — use a clean unique `*.local` (not the phone's real
hostname, which conflicts; not the Mac's own name, which `dns-sd -P` then breaks). Verify with
`ping host.local`, NOT `dns-sd -Gv4` (that only shows the AAAA and misreports).

**How far it gets.** With the exact record + resolving host + `socat` relay on 56418:
- `devicectl list devices` flips the phone from `unavailable` → **`available (paired)`**.
- **Device Hub shows the remote iPhone** (briefly).
- netstat confirms the front door is fully proxied: `remoted → Mac:56418 (ESTABLISHED)` and
  `socat → phone:56418 (ESTABLISHED)` over the tailnet.

**Where it stalls.** `devicectl` live ops fail with `RemotePairingError 4 / tunnel connection
failed`, and Device Hub drops the device after it appears. The RemotePairing handshake succeeds on
the TCP front door but the **trusted tunnel (QUIC over UDP)** does not come up through the relay.
Two suspected causes, unresolved: (1) `socat UDP-LISTEN,fork` does not carry QUIC cleanly, and
(2) the tunnel moves to a **dynamic UDP port** the phone negotiates (the article blanket-relayed
55000–55300 for its phone; ours would be a different range and wasn't covered). Solving this means
a QUIC-aware UDP relay and/or discovering the tunnel port — the genuinely hard remaining work, and
the same tunnel machinery our own Architecture B (`REMOTEPAIRING-PROTOCOL.md`) would implement
natively.

**Bottom line:** the article's approach is validated for our devices through discovery and the
front-door connection — a remote iPhone made visible to Apple's tools over the internet — with the
QUIC trusted tunnel as the one unsolved layer.

### Why the tunnel can't be socat-relayed (2026-08-27, definitive)

`sudo tcpdump -n -i any 'udp and host <phone-tailnet-ip>'` during an Xcode/Device Hub connect
captured **zero packets**. So `remoted` sends **no UDP to the phone** while establishing the
trusted tunnel — it is not a "missed dynamic port" problem. The TCP RemotePairing front door
proxies fine (device appears in Xcode 26 Devices / Device Hub), but the QUIC tunnel is dialed
along a path that never targets the phone's IP — consistent with the RemotePairing handshake
handing `remoted` a **loopback/link-local endpoint** (the phone's own record advertises `::1`,
`127.0.0.1`, `fe80::1%lo0`). A transparent byte relay cannot bridge that. Verdict: the article's
socat approach yields **discovery + front-door only** for this stack; completing the tunnel needs
either a QUIC/loopback-aware relay that understands CoreDevice's endpoint negotiation, or — the
right answer — establishing the tunnel natively (**Architecture B**, `REMOTEPAIRING-PROTOCOL.md`),
which is the project's own path and avoids Apple's daemons entirely.
