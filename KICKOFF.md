# rplay-hub — new session kickoff prompt

Paste the block below as the first message in a new session (run from `~/rplay-hub`).

---

We're building **rplay-hub**: a from-scratch CoreDevice host that mirrors and controls an iPhone —
screenshot, live HEVC screen video, touch/HID injection — with NO Xcode and no Apple daemons in the
runtime path. It is **`adb` for iPhone plus a Device Hub-style GUI**: a device list and a live
**View Screen** window you can click and drag in. A development tool, not a consumer product.

**Languages:** **Swift** for the macOS app (`app/`, an Xcode project), **C/C++** for the portable
protocol core (`host-c/`). **Python (`host/`) is prototyping/verification only** — wire formats get
proven there against a real phone, then committed to in C or Swift; `host/mirror.py` is also the
engine the app currently talks to. Shipped as a **Developer ID signed, notarized DMG** from our own
site (the Mac App Store is not possible with the current design — see `app/DISTRIBUTION.md`).
CarPlay is NOT part of this project (it was only where the earliest experiments happened). Sibling
project: `~/rplay`.

**Constraint: do not modify or add files outside `~/rplay-hub`.** Copy what you need in, then edit
the copy.

## What's here

Folder conventions follow `~/carplay-dev` (which adopted from `~/rplay` first and reorganized its
folders): `app/` split by subsystem, adopted sources recorded in `refs/`, third-party in `deps/`,
tooling in `scripts/`.

- `app/` — **rPlayHub**, the native macOS app. An Xcode project (`rPlayHub.xcodeproj`, target
  `rPlayHub`), Swift + AppKit: live View Screen, HEVC via `AVSampleBufferDisplayLayer`, clicks →
  taps and drags → swipes. **Builds clean in Debug and Release with no warnings**, and
  `scripts/package-dmg.sh` produces a working DMG. It is a *client* of the engine — the engine owns
  the privileged tunnel. Read `app/README.md`, `app/api/PROTOCOL.md` (the wire contract),
  `app/DISTRIBUTION.md` (signing/notarization, and why the Mac App Store is impossible today),
  `app/PORTING.md` (the four OS seams for Linux/Windows).
- `host/` — Python engine + harness. **`mirror.py` is the live mirror + control engine** (holds the
  media stream open, broadcasts Annex-B HEVC on :9877, serves the control API on :9876). Verified
  flat modules: `usbmux.py` `lockdown.py` `tunnel.py` `tunnel_up.py` `xpc.py` `remotexpc.py`
  `rsd.py` `coredevice.py` `hid.py` `screen.py` `probe.py`. Plus `rplayhub/` — a PARTIAL refactor,
  see below. `host/README.md` has the architecture + wire notes.
- `host-c/` — C core: `cdhost.c` (layers 0–1 working), `Makefile`, `README.md`.
- `deps/AccessorySDK/` — vendored pairing/crypto kit (SRP, `PairingUtils`, OPACK, TLV8,
  ChaCha20-Poly1305, Curve25519/Ed25519, LibTomMath, BonjourBrowser, CFLite). Read its
  `PROVENANCE.md`: `PairingUtils.c` is the pair-setup/pair-verify state machine, and it points at
  `~/iPhoneMirroring/iphone-mirror-client/src/rapport.c` — working C for the modern Pair-Verify flow
  and the better primary crib for the RemotePairing work.
- `refs/rplay/` — `MANIFEST.md` (what we took from `~/rplay` and where it went, plus what is NOT
  reusable) and `doc/` (its architecture notes, `sdk-api.md`, and `_claude-memory/` findings).
- `scripts/` — `mirror_check.py` verifies a running engine end to end; `tarplay_client.py` is
  rplay's reference client.
- `doc/` — `REMOTEPAIRING-PROTOCOL.md` (the direct-wifi pairing door, from a symbol-dump RE),
  `COREDEVICE-SCREEN-STREAMING.md`.

## Stack

```
usbmux → lockdown (+TLS) → CoreDeviceProxy CDTunnel → utun → RSD (RemoteXPC/HTTP2 + XPC codec)
  → coredevice.* services (screencaptureservice, displayservice, universalhidservice)
```

Everything above the tunnel is transport-agnostic: the tunnel is just raw IPv6 packets, so a new
transport (RemotePairing, relay) is one new file and nothing above it changes.

## Verified live — be precise, don't overclaim

- iPhone 12 Pro (iOS 26.5.2) over USB, and iPhone 13 Pro (iOS 27) over WIFI (udid
  `DEVICE-UDID-REDACTED`): transport → RSD, 85 services. ✅
- screenshot (PNG) ✅ · tap/swipe ✅ (opened Safari)
- HEVC screen record → decodable `.h265` of the real screen ✅ but **GLITCHY** (RTP reorder done;
  RTCP PLI/RR feedback deferred → residual artifacts are packet loss).
- **Live engine written, NOT yet run against a phone** — `host/mirror.py`. Offline logic is
  unit-tested (video fan-out with cached VPS/SPS/PPS for late joiners, request dispatch, coordinate
  clamping); the live path needs one `sudo` run. **No window yet** — `ffplay` is the stand-in.
- **RemotePairing direct-wifi door: NOT built** (handshake designed only).
- **Relay / remote-Xcode: NOT built, and the Xcode half is NOT verified** — see
  `host/rplayhub/transport/relay.py` for the two candidate mechanisms and why neither is a
  feature yet.

## Run

Live engine (root, long-running):

```
sudo python3 host/mirror.py DEVICE-UDID-REDACTED
ffplay -fflags nobuffer -flags low_delay tcp://127.0.0.1:9877
python3 scripts/mirror_check.py --tap
```

One-shot actions (root, per action):

`sudo python3 host/tunnel_up.py <udid> {shot out.png | tap fx fy | swipe fx0 fy0 fx1 fy1 | record out.h265 secs}`
(fx/fy are 0..1 screen fractions. Omit udid to auto-pick the first device.)

## Key gotchas (already solved — keep them)

- **HID auth gate:** touch is only routed to UIKit while a media stream runs → tap/swipe must open
  `screen.open_stream` first. A permanent View Screen stream makes this free.
- **RemoteXPC `receive()` must skip empty-dict `{}` acks** (device sends one before the real reply).
- **Video offer blob must be zlib LEVEL 9** (device rejects other levels).
- **No hardware attestation** in CoreDevice/RemotePairing → a non-Apple host can pair for real.
- **Wifi today goes THROUGH Apple's usbmuxd** (Mac-tethered, intermittent `Network` entry). The
  direct, portable path is the RemotePairing door.
- **iOS 26 cannot mirror.** Apple's own Device Hub says screen viewing is unsupported there, and it
  matches what we see: use the iOS 27 phone. `sudo python3 host/rsd_probe.py <udid>` prints a
  device's service catalog and says plainly whether `displayservice` exists.
- **The coded video is NOT the screen.** `displayservice` encodes at 16-pixel alignment with no
  conformance-window crop: a 1170×2532 screen arrives as **1184×2576** with black padding on the
  right and bottom (verified frame-by-frame against a real recording — padding starts exactly at
  column 1170 and row 2532). Crop to the real screen size, and map touches inside the crop, or every
  tap drifts up to 1.7% toward the top-left.
- **AppKit flips sublayer content.** A non-flipped `NSView` gets a geometry-flipped backing layer, so
  a manually added `AVSampleBufferDisplayLayer` renders the video upside down. `MirrorView` is
  `isFlipped = true`, which fixes it and matches the device's top-left origin.
- **Only one IDR per ~10 s** in the device's stream, and this caused the **first live run to render
  a black window** even though the tunnel, RSD, the media stream (705 RTP packets) and the touch
  channel were all healthy. The device's parameter sets and its only IDR arrive at stream start,
  before the app connects; a joiner then receives nothing but P-frames referencing pictures it never
  saw, so the decoder produces no output *and no error*. Parameter sets alone are NOT enough to
  start decoding.
  Mitigations now in place: the engine caches the last keyframe for late joiners and **restarts the
  media stream when a viewer connects** to force a fresh IDR (debounced 5 s); the app drops frames
  until an IRAP arrives and says so in the title (`waiting for keyframe (N frames skipped)`) rather
  than showing an unexplained black window, and recovers on its own once one arrives.
  **RTCP PLI is the real fix** — request a keyframe without disturbing the stream. Reproduce the
  failure any time with `python3 scripts/mock_engine.py screen3.h265 --start-at 150`.

## State of the Python refactor (`host/rplayhub/`)

Started, then deliberately halted when the shipping languages were settled as Swift + C — a large
Python refactor stopped being the right investment. It is left in a consistent, importable state,
and **the verified flat modules and `tunnel_up.py` entry point are untouched and still work.**

Landed: `errors.py` · `wire/` (xpc, remotexpc, cbor — frozen codecs) · `transport/` (Transport ABC
+ `TunnelLink`/`TunnelParams`, usbmux transport built, RemotePairing + relay documented stubs) ·
`net/` (tun device — macOS verified, Linux written-but-unrun — and the packet pump) · `rsd.py`
(`ServiceCatalog`).

Not written: service broker, screen/HID wrappers on the broker, reconnecting session, multi-device
server, CLI.

Worth keeping from it regardless of language, because these are design decisions and not Python:
the Transport/TunnelLink seam, the tun-device platform seam, and the packet pump's failure
signalling (the original `splice()` let a dead tunnel look alive — either direction dying now
marks the pump failed exactly once so a supervisor can reconnect).

## Device behaviour measured on 2026-07-27 — expensive to rediscover, trust these

All from live runs against iPhone 13 Pro / iOS 27 over wifi. Every one contradicted an assumption.

1. **The device sends HEVC under RTP payload type 100.** PT 100 is what our offer used for the AVC
   bank, so the number is NOT a codec indicator. Inferring the codec from it switched us to the
   H.264 depacketizer and produced a black window. `screen.sniff_codec()` reads the payload
   structure instead (HEVC AP=48/FU=49 vs H.264 STAP-A=24/FU-A=28) — verified against captured
   bytes: `6001...` is an AP carrying a VPS, `620194...` an FU carrying IDR_N_LP.
2. **Exactly ONE IDR per session, at stream start.** `nal_types` showed `20x1` with ~600 P-frames
   over 20 s. A viewer attaching later has nothing it can ever decode from.
3. **RTCP PLI and FIR are both ignored.** 8 PLI + 8 FIR (framing verified against RFC 4585/5104,
   correct SSRC, sent to the RTP source) produced no keyframe. On-demand keyframes are not
   available on this device.
4. **RTCP Receiver Reports are REQUIRED.** The device sends Sender Reports on the same port as the
   RTP; if nothing answers, it stops transmitting after ~30 s. `screen.RTCPSession` now sends an RR
   every second, and that is what keeps video alive — not a quality refinement.
5. **`startmediastream` works at startup and times out later in the session.** Both a deferred start
   (on viewer connect) and a restart fail identically with `TimeoutError`, so the failure tracks
   elapsed time rather than the reason for the call. Hypothesis: RSD service ports go stale.
   `_resolve_display_port()` now re-queries RSD immediately before opening the stream —
   **written but NOT yet tested against the device. That is the next thing to run.**
6. **RTCP must be split off before depacketizing.** RTCP's packet type is 200-204, but after the
   marker bit is masked it arrives as 72-76, so `if 200 <= pt <= 204` can never match. That bug fed
   every RTCP packet to the video depacketizer. Reject 64-95 (RFC 5761).
7. **iOS 26 cannot mirror at all** — Apple's Device Hub says so too. Use the iOS 27 device.

Two of tonight's faults were self-inflicted "fixes": the RTCP mask comparison, and restarting the
media stream on viewer connect (which killed the stream outright — now opt-in via
`--restart-on-viewer`, and it is known to fail). What located every fault was recording raw bytes,
never reasoning about what the device ought to send.

## Why the device sends one keyframe — answered from AVConference (2026-07-28)

Decompiled `reference/binaries/AVConference` explains the keyframe behaviour that shaped every
mirroring decision. Apple's media stack has:

```
RtcpPSFB_PLIEnabled:     RtcpPSFB_FIREnabled:     RtcpPSFB_LTRAckEnabled:
RtcpRTPFB_GNACKEnabled:  RtcpAppLTRAckRx/Tx       RtcpPSFBForLTRAck
_VideoReceiver_SendLTRACK     "Received LTR ACK timestamp=%d"
```

Two conclusions:

1. **PLI and FIR are negotiated capabilities, not always-on.** They sit behind `*Enabled:` flags,
   which is why 8 PLIs and 8 FIRs produced no keyframe — the feature was never enabled for our
   stream. Turning them on is a negotiation change, not a protocol one.
2. **The real recovery mechanism is LTR** (long-term reference frames). The encoder marks them,
   the RECEIVER acknowledges them, and the encoder recovers using an ACKed long-term reference
   instead of sending a fresh IDR. That is exactly consistent with one IDR per session and no
   amount of asking producing another. We send no LTR ACKs at all.

Our offer's feature strings are genuine — `FLS;SW:1;` and `FLS;VRAE:0;SW:1;` appear **verbatim**
in AVConference, alongside `-[VCMediaNegotiatorV2 mediaBlobHasFLSPerCodec:]`. So the RE was right;
what is unknown is the rest of the vocabulary and what `clientSupportedFeatures = 140` selects.

**So the way to get keyframes on demand is to negotiate for them**, not to send more feedback. That
is the thread to pull if late-joining viewers or loss recovery ever matter — and it is where the
"our capture parameters may not be as good" instinct leads.

## Where mirroring stands — WORKING

**Live mirroring works end to end.** `./scripts/live.sh` brings up the engine and rPlayHub and the
phone's screen appears at full quality with low latency. Measured on a live session: HEVC 1170×2532,
0.0% loss, 0 queue drops, 678 NALs from 2421 RTP packets, uptime past the 30 s point where the
device used to stop sending.

Both halves were also verified in isolation, which is what ended the guessing:
`sudo ./scripts/diagnose.sh` captured 10 s from the engine and ffmpeg decoded **417 clean frames**
from it (engine correct); replaying that same capture to the app rendered it sharply (app correct).

The fix that closed it was finding 5 — re-resolving the displayservice port. Every black-window run
had `✗ could not start the media stream: TimeoutError` in the log, meaning the media stream never
started at all.

`sudo ./scripts/diagnose.sh` is the fastest way back in: it starts the engine, attaches only a
byte-capturing viewer, records 10 s, and decodes it with ffmpeg — an independent decoder, used as an
instrument and not shipped. It prints a verdict saying whether the engine or the app is at fault,
which is the question guessing kept getting wrong.

If finding 5's fix does not hold, the durable answer is not RTCP at all — it is asking for a shorter
GOP in the offer blob (`host/screen.py`, `_media_blob_video`), whose constants
(`_CLIENT_SUPPORTED_FEATURES = 140`, `_HEVC_FEATURES`, the bitrate tiers) are all unverified RE
guesses.

## Settled architecture — one app, like Device Hub

**One `.app`: unprivileged GUI + a root launchd daemon embedded in the bundle** (`SMAppService`),
talking over the existing localhost API. Verified as Apple's own shape: `DeviceHub` runs as the
user, `remoted` runs as root and creates the `utun`. Details and the measurements in
`app/DISTRIBUTION.md`. The GUI is never root.

The blocking prerequisite is the **C engine**, since Python is not in the shipping path.
`host-c/cdhost` now does usbmux → lockdown → TLS → CDTunnel → **utun + packet pump → RSD reachable**
(`sudo ./host-c/cdhost`). Missing: RemoteXPC/HTTP2, the XPC codec, RSD enumeration, the media-stream
offer blob, HID. `core/` already holds the portable frame assembly and coordinate maths in tested C,
so the engine will not need to re-derive those.

## Next tasks

1. **Port the engine above the tunnel to C** — RemoteXPC/HTTP2 + XPC codec first, then RSD, then
   `startmediastream` and HID. Feed frames into `core/rp_video_stream.c`, which is already tested.
   This is what unblocks the single-app packaging.
2. **Then package as one app**: engine as an embedded launchd daemon via `SMAppService`.
3. **Run the engine + app against the phone.** One `sudo python3 host/mirror.py <udid>`, then launch
   rPlayHub. `scripts/mirror_check.py` verifies the engine independently. Everything below depends on
   this being green, and it is the only step that has never been executed.
2. **Fix whatever the first live run shows.** The likely suspects, in order: HEVC parameter sets
   arriving in an order the format-description builder does not expect; access-unit grouping on
   multi-slice frames; and click coordinates if the device's video dimensions differ from its
   screenshot dimensions (the engine reports `screen_size` from a PNG, the app maps against the
   video's own dimensions).
3. **RTCP PLI/RR feedback** — the fix for the glitchy video, and far more visible in a live view than
   in a recording. Must be written by hand; `~/rplay` has no video RTCP at all.
4. **RemotePairing direct door** — read `~/iPhoneMirroring/iphone-mirror-client/src/rapport.c` (working
   Pair-Verify) and `deps/AccessorySDK/Support/PairingUtils.c` (pair-setup + the authoritative
   constants) and establish how much transfers. That answers whether this is days or weeks.
5. **Relay + the Xcode-visibility experiment** — run the experiment before designing anything on top
   of it. See `host/rplayhub/transport/relay.py`.
