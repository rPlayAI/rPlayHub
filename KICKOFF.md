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
- `doc/` — `RVRA-AND-PORTABILITY.md` (**read before planning the C port's video path**: this
  stream only decodes correctly on Apple hardware, ffmpeg produces silent garbage, and the way out
  may be one token in our offer), `RSD-SERVICES.md` (**all 85 services the device advertises, in
  tables, with which are used, which a planned feature needs, and which are unverified guesses**),
  `rsd-services-ios27.json` (the raw map), `RENDERING-HANDOFF.md` (the resolution-switch
  investigation, resolved — read its top section before touching video),
  `REMOTEPAIRING-PROTOCOL.md` (the direct-wifi pairing door, from a symbol-dump RE),
  `COREDEVICE-SCREEN-STREAMING.md`, `devicehub_video_architecture_and_diagnosis.md`.
- `builds/known-good-*/` — snapshots of `rPlayHub.app` and `cdhost` that demonstrably worked, each
  with a README stating what did and did not work at that commit.

## Stack

```
usbmux → lockdown (+TLS) → CoreDeviceProxy CDTunnel → utun → RSD (RemoteXPC/HTTP2 + XPC codec)
  → coredevice.* services (screencaptureservice, displayservice, universalhidservice)
```

Everything above the tunnel is transport-agnostic: the tunnel is just raw IPv6 packets, so a new
transport (RemotePairing, relay) is one new file and nothing above it changes.

**Decoding is the exception, and it is not a seam.** It lives entirely in
`app/rPlayHub/VideoDecoder.swift` on VideoToolbox; `core/hwdecoder.h` is a vendored AirPlay header
that nothing here implements. Swapping in ffmpeg does NOT work: the device negotiates RVRA and
ffmpeg decodes the result to garbage with zero warnings. See `doc/RVRA-AND-PORTABILITY.md` —
turning RVRA off in the offer is the cheapest thing to try and is untested.

## Verified live — be precise, don't overclaim

Updated 2026-08-02. The C engine (`host-c/cdhost`) is what runs now; `host/mirror.py` still works
and still serves the same contract, but the app is developed against the C one.

- iPhone 13 Pro (iOS 27, udid `00008110-...`) over wifi, and iPhone 12 Pro (iOS 26.5.2) over USB:
  transport → RSD, 85 services. ✅ Catalogued in `doc/RSD-SERVICES.md`.
- **Live mirroring, clean, no artifacts under fast swipes.** ✅ 1170×2532 at 30–50 fps, 0.0% loss
  measured over long sessions. This was broken for days; see "The resolution switch" below, which
  is the single most expensive thing in this file to rediscover.
- Screenshot ✅ · tap / swipe ✅ · **Home** ✅ (a real bottom-edge gesture) · recording ✅
- **Diagnostics tab** ✅ — model, build, ECID, serial, battery, storage. Reads lockdown over
  usbmuxd, so it answers while the tunnel is down and even while Apple's Device Hub holds the
  device.
- **Simulators listed** ✅ (via `simctl`) — boot, shut down, screenshot, copy UDID. **No live
  mirroring**: a booted simulator is not a display on this Mac (checked — the screen count does not
  change), `recordVideo` buffers to disk rather than streaming (a fifo got zero bytes), and
  screenshots come back at ~1.5/s. Live needs Simulator.app's window via ScreenCaptureKit, or the
  private SimulatorKit.
- **Device selection works from the sidebar.** cdhost needs no udid; select_device rebinds it.
- **Simulators are listed** but cannot be mirrored -- see below.
- **RVRA cannot be turned off from the offer.** Tested with a verified offer; the encoder adapts
  regardless. `doc/RVRA-AND-PORTABILITY.md` has the numbers. This is the blocker for decoding the
  stream anywhere but Apple hardware. The remaining lever is bitrate, and **that experiment is
  built but has not been run**: `sudo ./scripts/rvra-bitrate-all.sh`, phone unlocked on the home
  screen, five conditions in one pass with a printed table. Read the positive control first — the
  doc says why that ordering is the whole design. **Expect it to fail**: our stream already spends
  2.3x the bits per frame Apple's does and downshifts twelve times as often, which is not what
  rate starvation looks like.
- **Our sessions downshift far more than Device Hub's on the same phone** — 30% and 58% of frames
  below full tier across two of our captures, against 4.8% for Apple's. That decides whether the
  fallback plan is viable at all (a freeze half the time is not a product), so it is the more
  promising thread than the bitrate ceiling. Confounded by motion; the new instrument controls for
  it. `python3 scripts/rvra-bitrate.py --self-test <capture.h265>` reproduces the table.
- **Rotate turns the VIEW, not the device.** Verified against the full service catalogue: there is
  no orientation, accelerometer or motion service. Nothing advertised can rotate a physical device.
- Lock / volume / Siri: **refused, not faked.** They need the `mainScreenButtons` HID report format
  (`_ServiceID 1026`), which is not decoded. Keyboard (`_ServiceID 512`) is advertised and unused.
- **Pair / Unpair / Restart: NOT built.** Restart and Shutdown have no blocker —
  `com.apple.mobile.diagnostics_relay.shim.remote`, a classic documented protocol. Pairing is
  blocked on the RemotePairing handshake, which is designed only.
- **RemotePairing direct-wifi door: NOT built** (handshake designed only).
- **Relay / remote-Xcode: NOT built, and the Xcode half is NOT verified** — see
  `host/rplayhub/transport/relay.py` for the two candidate mechanisms and why neither is a
  feature yet.
- **Device discovery is usbmuxd-only.** Device Hub discovers through CoreDevice (remoted, Bonjour)
  and therefore sees devices we cannot. This is the same gap the remote-device goal has to close.

## Run

The C engine and the app — this is the product:

```
sudo ./host-c/cdhost                                # no flags: pick the device in the sidebar
xcodebuild -project app/rPlayHub.xcodeproj -scheme rPlayHub -configuration Debug \
    -derivedDataPath build/dd build && open build/dd/Build/Products/Debug/rPlayHub.app
```

Clicking a device in the sidebar binds it, as in Device Hub. The daemon serves one device at a
time -- the tunnel and media session belong to it -- so rebinding **re-executes cdhost**, and the
app reconnects a few seconds later. `--udid <prefix>` still starts on a chosen device,
`--dump-services` prints the catalogue, `--rctl` / `--max-bitrate` / `--min-bitrate` vary the rate
budget for the experiment above, and `--help` lists the rest. Every override is echoed at startup
and carried across a rebind, so a run cannot silently revert to the default halfway through.

**Flags, not just environment variables: sudo strips the environment.** `RPLAY_UDID=x sudo ./cdhost`
sets it for sudo, which discards it, and the daemon then binds whatever it would have anyway
without saying so. Three runs were lost to that.

Known-good binaries are kept in `builds/known-good-*/` with a README stating what worked at that
commit. Compare against those before concluding something regressed.

The Python engine still serves the same contract:

```
sudo python3 host/mirror.py DEVICE-UDID-REDACTED
ffplay -fflags nobuffer -flags low_delay tcp://127.0.0.1:9877
python3 scripts/mirror_check.py --tap
```

One-shot actions (root, per action):

`sudo python3 host/tunnel_up.py <udid> {shot out.png | tap fx fy | swipe fx0 fy0 fx1 fy1 | record out.h265 secs}`
(fx/fy are 0..1 screen fractions. Omit udid to auto-pick the first device.)

## The resolution switch — the fix that took four days (2026-08-02)

Mirroring was garbled under any fast swipe. Every integrity check passed while the picture was
wrong, which sent the investigation into loss, reordering, LTR, the depacketizer and the reference
chain — all genuinely fine, all eliminated at length.

**The device appends a trailer to the last slice NAL of every access unit**, carrying the
resolution the encoder actually coded that picture at:

```
[width:u16be][height:u16be][00 ...][4-byte session tag]
04a0 0a10 0000030000049209e403   = 1184x2576
0440 0780 0000030000049209e403   = 1088x1920
02d0 0500 0000030000049209e403   =  720x1280
```

Under motion the encoder downshifts and squeezes the whole screen into the top-left of the coded
frame. The SPS never changes. Measured: 601/601 slice NALs on the wire carry the trailer, and
0/1368 access units that `avconferenced` feeds VideoToolbox still have it — Apple strips it and
passes the size to its decoder as `ActiveVideoResolution`.

Three things must all be true:

1. strip the trailer before decoding,
2. pass the size per frame as `ActiveVideoResolution`,
3. **put the decoder in RVRA mode first** —
   `VTDecompressionSessionSetProperty(session, "VideoResolutionAdaptationType", 3)`, plus
   `NegotiationDetails = "RVRA1:0;SW:1;FLS"`.

Number 3 is what defeated every earlier attempt. Passing `ActiveVideoResolution` to an ordinary
session changes nothing — replaying 401 of Apple's own captured frames with and without it gave
**0 of 401 differing**, which reads as "the key is inert" and actually means "RVRA is off, so the
key is discarded". `avconferenced` sets the switch AFTER creation via the private
`VTDecompressionSessionSetProperty`, so an interposer hooking only `Create` and `DecodeFrame` can
never see it. Found by Kimi K3; confirmed here offline, where the same wire bytes decode to a
mosaic on a plain session and cleanly on an RVRA one, all 601 frames.

The trailer bytes are harmless in themselves — they sit past `rbsp_slice_trailing_bits`, ffmpeg
decodes the unstripped stream with zero warnings, and stripping alone yields byte-identical
pictures. That is exactly why every conformance check passed while the picture was wrong.

### Display geometry that goes with it

- **The padding fraction is invariant across tiers.** screen/active measured 0.9882x0.9829 at
  1184x2576, 0.9890x0.9833 at 1088x1920, 0.9889x0.9836 at 720x1280 — the encoder scales screen and
  padding together. Treating reduced tiers as "pure screen" put the padding back on screen and
  made the picture twitch on every downshift.
- **The notch must be covered; the Dynamic Island must not.** On a notched phone the display does
  not exist behind the notch but the framebuffer still allocates those pixels, so they arrive as
  picture the real device never shows. An island sits over a display that DOES extend behind it and
  iOS draws it black itself. Confirmed from Apple's own framebuffer masks (the per-device PDFs in
  Xcode's simulator profiles): the iPhone 13 Pro mask cuts a 484x101px hole out of 1170x2532; the
  iPhone 16 Pro mask has no central cutout at all.
- **cdhost never populates `screen_w`/`screen_h`** — they are declared, read, and never assigned,
  and the wire shows `"screen_width":0`. The app works around it with a per-model table.
  Implementing `getdisplayinfo` would retire that.

## Getting the service catalogue without restarting anything

RSD can be handshaked directly over the tunnel `cdhost` already holds — there is no need to restart
the daemon with a dump flag:

```python
info = json_rpc("tunnel_info")                      # device_addr + rsd_port from :9876
peer = rsd.enumerate_services(info["device_addr"], info["rsd_port"])
services = peer["Services"]                          # 85 of them, name -> {Port}
```

This was blocked on a daemon restart for hours before anyone tried it. **Ports are per-session** and
a cached one is what made `startmediastream` time out for days — always look up by name.

## Bugs found on 2026-08-02 that will look like device faults if reintroduced

Every one of these presented as "the phone is not sending video" and none of them was.

1. **iOS 26 cannot mirror, and cdhost preferred it.** With an iOS 26 phone on USB and an iOS 27
   phone on wifi, the USB preference bound the one that can never mirror. It now names every
   attached device at startup, marks which it bound, and warns when the bound device is below
   iOS 27.
2. **File descriptors survive `execv`.** Rebinding by re-exec inherited the listening sockets on
   9876/9877, so the new image could not bind them. Everything from fd 3 up is closed before the
   exec now.
3. **`api_serve`'s return value was dropped**, so a daemon that could not listen ran on pumping
   packets forever, answering nothing and looking alive in the terminal.
4. **Restoring a table selection fires the same delegate a click does.** The device list refreshes
   on a timer, and selecting a device restarts the daemon, so this was a reboot loop.
5. **`directStream` was never stopped.** The 2-second retry timer built a new receiver on every
   tick -- 71 in half a minute, all answering the device's Sender Reports, nothing decoding. The
   only retry cancellation sat in the USB path, which is off by default.
6. **Two `connect()` calls could overlap.** Both negotiated a media stream; the device permits one
   per session, so the second killed the first and no packets arrived.

7. **A daemon keeps serving a session after its device has gone.** When the wifi entry drops, the
   daemon holds the old tunnel: `tunnel_info` still answers with addresses and per-session service
   ports, `stream_info` still answers, and `list_devices` can even show the device back again — but
   a TCP connect to the displayservice port on the tunnel address times out, and a viewer on 9877
   gets a silent nothing. Observed 2026-08-02: the phone left and rejoined wifi while `cdhost` ran,
   and everything except the video kept looking healthy. There is no liveness check; restarting
   `cdhost` is the fix. `scripts/rvra-bitrate.py` now preflights exactly this and names it, because
   it costs 45 seconds and a wrong conclusion otherwise.

The pattern worth keeping: each was found by reading the log for repetition -- "direct stream:
receiving" 71 times against a packet counter stuck at 35 -- not by reasoning about the protocol.

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
