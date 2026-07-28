# CoreDevice / DeviceHub screen streaming — protocol notes

Date: 2026-07-26. Investigated on macOS Tahoe + Xcode 27 beta (27A5218g), against an
**iPhone 13 Pro on iOS 27** (`COREDEVICE-ID-REDACTED`) and an iPhone 12 Pro on 26.5.2.

**Why we care:** CarCast *is* a screen-streaming receiver (CarPlay/AirPlay). CoreDevice is Apple
solving the same problem — get a phone's framebuffer onto another machine, live, with input — but
with a completely different architecture: **device-as-server** over an encrypted tunnel, rather than
AirPlay's receiver-as-server over RTSP/HTTP. Worth understanding as a design comparison, and because
`devicectl` gives us a scriptable way to *see* a phone's screen during CarPlay tests instead of
asking a human what's on it.

Everything below is verified by running it unless marked INFERRED.

---

## 1. What the tooling exposes

```sh
export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer

xcrun devicectl list devices                     # physical + simulated, with state
xcrun devicectl device info details --device <id>
xcrun devicectl device info displays --device <id>
xcrun devicectl device capture screenshot   --device <id> --destination out.png \
      [--display-unique-id <uuid>]
xcrun devicectl device capture screen-record --device <id> --destination out.mp4 \
      [--display-unique-id <uuid>] [--codec h264|hevc] \
      [--mask-policy ignored|premultipliedAlpha|black] [--duration <s>]
```

Gotchas hit in practice:
- `--destination` is **required** and the extension is enforced (`.png` / `.mp4`; `.mov` is rejected).
- `screen-record` is **not supported on every device**. The iPhone 13 Pro returned
  `com.apple.dt.CoreDeviceError 1001`, `CapabilityFeatureIdentifier =
  com.apple.coredevice.feature.screenrecording` — that feature is simply not in its advertised set
  (§2). Screenshots work fine on the same device.
- `-v/--verbose` and `--log-output` give only lifecycle lines ("Acquired tunnel connection to
  device", "Enabling developer disk image services", "Acquired usage assertion") — **no protocol
  detail**. Don't expect to learn the wire format this way.

### Displays

`devicectl device info displays` on the iPhone 13 Pro returned two:

| id | name | size | pointScale | type | uniqueId |
|---|---|---|---|---|---|
| 1 | LCD | 1170×2532 | 3 | integrated (primary) | — |
| 2 | Wireless | 1184×2544 | 1 | wireless | `8DCDC2FD-E2BD-4FC8-BE1F-A28B1E2F8238` |

The `Wireless` display captured as **entirely black**, and its geometry is phone-shaped
(portrait, pointScale 1) rather than car-shaped. INFERRED: it is DeviceHub's own screen-viewing
surface sitting idle, not CarPlay.

**Open and worth testing:** when a CarPlay session is live, does a CarPlay-shaped display appear
here? If so, `capture screenshot --display-unique-id <carplay>` would let us photograph the *car
screen* from the phone side — which would have shortcut most of the 2026-07-24 video-in-car session
(see [SENDER-APP-APTV-ANALYSIS](SENDER-APP-APTV-ANALYSIS.md), where "is the CarPlay screen black?"
was answered by asking a human).

## 2. Advertised feature set

The device advertises **64** `com.apple.*` services/features. Screen-related:

```
com.apple.coredevice.feature.capturescreenshot
com.apple.coredevice.feature.getdisplayinfo
com.apple.coredevice.feature.startmediastream
com.apple.coredevice.feature.stopmediastream
com.apple.coredevice.feature.getmediastreamserverstatus
com.apple.coredevice.feature.startvideooutput
com.apple.coredevice.feature.viewdevicescreen
com.apple.coredevice.feature.streamapplist
com.apple.coredevice.feature.streamprocesslist
com.apple.coredevice.feature.capturesysdiagnose
```

Plus the plumbing:
```
com.apple.dt.serviceconnection.create                        (Create Service Connection)
com.apple.coredevice.feature.servicexpcpeerconnection        (Service XPC Peer Connection)
```

Two things follow. First, **live viewing is `startmediastream`, not `screenrecording`** — which is
why DeviceHub can mirror a device whose `screen-record` capability is refused. Second, the name
`getmediastreamserverstatus` says the **device runs the media server** and the Mac is the client.
That is the inverse of AirPlay/CarPlay, where the head unit is the server the phone connects into.

## 3. Transport

From `devicectl device info details`:

```
Transport Type:            localNetwork
Tunnel IP Address:         fd15:7e67:ea3::1        (device)
Tunnel Transport Protocol: tcp
Authentication Type:       manualPairing
```

Mac side is a `utun` interface:

```
utun8   fd15:7e67:ea3::2   MTU 16000
```

**MTU 16000**, not 1500 — deliberately sized for framebuffer throughput.

### It is encrypted — QUIC + TLS-PSK

`RemotePairing.framework` imports settle the question:

```
_nw_protocol_copy_quic_definition
_nw_quic_get_stream_usable_datagram_frame_size
_sec_protocol_options_add_pre_shared_key
_sec_protocol_options_set_client_raw_public_key_certificates
_sec_protocol_options_set_server_raw_public_key_certificates
_sec_protocol_options_set_min_tls_protocol_version
_sec_protocol_options_set_peer_authentication_required
_nw_connection_write_multiple
_nw_connection_receive_multiple
```

So: **QUIC transport, TLS with pre-shared keys and RFC 7250 raw public keys** (identities from the
pairing, not an X.509 chain). `tcpdump -i utun8` yields ciphertext only.

Worth noting the symmetry: CarCast's own phone-proxy path uses the same construction —
`[proxy] Layer1: TLS-PSK connect ... pskLen=32 idLen=16`. Apple reaches for TLS-PSK + raw public keys
whenever both ends already share a pairing secret.

`CoreDevice.framework` itself imports **no** TLS/crypto symbols — it rides on RemotePairing. That
matters for interception (§5): the plaintext boundary is inside the process, above the transport.

### Connection topology

`lsof -nP -i6TCP` while DeviceHub was viewing:

```
CoreDeviceService (pid 1725)   334 connections
DeviceHub                       16 connections

fd15:7e67:ea3::2.<ephemeral>  ->  fd15:7e67:ea3::1.52613     (×13 observed)
fd15:7e67:ea3::2.<ephemeral>  ->  fd15:7e67:ea3::1.52631     (×2 observed)
```

- **`CoreDeviceService` owns the device tunnel**, not DeviceHub. The app is a client of the daemon.
- **Many TCP connections to one port (52613).** Matches `com.apple.dt.serviceconnection.create`:
  each service request opens its own connection rather than multiplexing services over one socket.
  Port 52631 is a second, smaller endpoint — INFERRED: the media stream itself.
- Throughput was **~183 MB in / ~15 MB out** cumulative, a ~12:1 inbound ratio: video in, control out.

## 4. Message layer is Swift `Codable`

`CoreDevice` exports are entirely Swift-mangled — **no ObjC classes at all**. Every message type
carries a `Codable` conformance pair:

```
$s10CoreDevice...OperationActionDeclarationV5InputV4fromAEs7Decoder_p_tKcfC   // init(from:)
$s10CoreDevice...OperationActionDeclarationV5InputV6encode2toys7Encoder_p_tKF // encode(to:)
```

The recurring `…ActionDeclaration.Input` shape suggests an RPC model of declared actions with typed
inputs, encoded via `Codable` and carried over the service connection. That differs from the AirPlay
side of the house (binary plists / HTTP-ish framing) and means there is no convenient C function like
`HTTPMessageWriteMessage` to break on.

## 5. Capturing plaintext (the AirPlay technique, adapted)

Since the tunnel is encrypted, intercept **above** the crypto — the same principle as
[[airplay-http-capture-method]].

Feasibility on this Mac:
- **SIP is disabled** (`csrutil status` → disabled), so lldb can attach to Apple-signed daemons.
- `lldb -p <CoreDeviceService>` attaches cleanly; `Network`, `CoreDevice`, and `RemotePairing` are
  all loaded in it, and `nw_connection_send` / `nw_connection_receive` resolve.

Harness: **`scripts/coredevice-capture/`** (`cdcap.py` + `cdcap.lldb`). It attaches to
CoreDeviceService, breaks on the send path, maps the `dispatch_data` rope with
`dispatch_data_create_map`, then does a pure `ReadMemory` of the contiguous buffer, and
auto-continues so the daemon is never held. Breakpoint evaluation uses
`SetIgnoreBreakpoints(True)` to avoid recursing into itself.

**Status: WORKING — plaintext captured.** `scripts/coredevice-capture/run-capture.sh [seconds]`.

Two fixes were needed to get there:
1. The first version hooked only `nw_connection_send` and caught nothing; hooking all four
   (`send`, `receive`, `write_multiple`, `receive_multiple`) showed the traffic actually uses
   **plain `nw_connection_send` / `nw_connection_receive`** — the `_multiple` variants that
   RemotePairing imports never fired in these runs.
2. Expression evaluation silently failed because CoreDeviceService is stripped: `dispatch_data_t`
   and the function prototypes aren't resolvable in the expression context. Casting the symbol to an
   explicit function-pointer type sidesteps type lookup:
   ```c
   ((unsigned long long(*)(void*))dispatch_data_get_size)((void*)ptr)
   const void *p=0; unsigned long l=0;
   ((void*(*)(void*,const void**,unsigned long*))dispatch_data_create_map)((void*)ptr,&p,&l);
   ```
   Then a pure `ReadMemory` of `p`. Breakpoints auto-continue and run with
   `SetIgnoreBreakpoints(True)` so the daemon is never held and the callback can't recurse.

Also worth knowing: a low tunnel byte-count means the stream was **idle**, not that the hooks are
wrong. Confirm DeviceHub is actively mirroring (utun byte-delta) before concluding anything.

## 5a. Wire format — RemoteXPC (DECODED)

Captured outbound message, 124 bytes total:

```
920bb029 01010200 6400000000000000 3000000000000000 42371342 05000000
0000f000 54000000 01000000 "XPCSideChannel.uniqueIdentifier"\0 ... 9000 ...
"E960FA7D-1466-4D0E-9D7B-0BA7F77B0648"
```

| Offset | Size | Value | Meaning |
|---|---|---|---|
| 0 | 4 | `92 0b b0 29` | **RemoteXPC framing magic**, LE `0x29b00b92` |
| 4 | 4 | `01 01 02 00` | flags / protocol version |
| 8 | 8 | `0x64` = 100 | payload length (LE); 24 + 100 = 124 total ✓ |
| 16 | 8 | `0x30` | message id / sequence — varied `0x14,0x18,0x28,0x30,0x44` across messages |
| 24 | 4 | `42 37 13 42` | **XPC serialization magic** `0x42133742` |
| 28 | 4 | `05 00 00 00` | XPC wire version 5 |
| 32 | 4 | `00 f0 00 00` | type `0xf000` = XPC **dictionary** |
| 36 | 4 | `0x54` = 84 | dictionary byte size |
| 40 | 4 | `01 00 00 00` | entry count = 1 |
| 44 | … | `XPCSideChannel.uniqueIdentifier` | key (NUL-terminated, padded) |
| … | … | `0x9000` + UUID string | value (INFERRED: `0x9000` = string type) |

So the stack is: **QUIC/TLS-PSK → 24-byte RemoteXPC frame header → XPC-serialized object graph.**
The captured messages are side-channel establishment, one UUID per channel — which corroborates the
"one TCP connection per service request" topology in §3 rather than service multiplexing.

The XPC magic `0x42133742` is the standard Apple XPC object serialization used by
`xpc_object` encoding, so an existing XPC deserializer should parse the payload directly once the
24-byte header is stripped.

### 5b. Full-session capture (device reboot → reconnect), 2026-07-26

**Raw capture: [`captures/coredevice-session-2026-07-26.txt`](captures/coredevice-session-2026-07-26.txt)**
(992 lines, ~92 KB — every dumped message with timestamps, ASCII and hex). Same file is kept next to
the tooling as `scripts/coredevice-capture/sample-capture.txt`.

`run-capture.sh 240` across an iPhone 13 Pro reboot. Traffic resumed ~25 s in (the reconnect);
223 control-plane messages dumped. Three things emerged.

**(1) A message-type discriminator at offset 4.** Two values seen so far:

| bytes 4..7 | meaning |
|---|---|
| `01 01 01 00` | CoreDevice RPC invocation |
| `01 01 02 00` | XPC side-channel establishment |

**(2) The RPC invocation envelope.** 616 bytes, an XPC dictionary of 6 entries:

```
920bb029 01010100 6802000000000000 0100000000000000   <- frame hdr, len 0x268, seq 1
42371342 05000000 0000f000 58020000 06000000          <- XPC dict, 0x258 bytes, 6 keys

CoreDevice.coreDeviceVersion            -> dict{ originalComponentsCount,
                                                 components[] (doubles),
                                                 stringValue "642.0.1" }
CoreDevice.deviceIdentifier             -> "COREDEVICE-ID-REDACTED"
CoreDevice.CoreDeviceDDIProtocolVersion -> int
CoreDevice.invocationIdentifier         -> UUID (per-RPC correlation id)
```

`deviceIdentifier` is the same UUID `devicectl list devices` prints, so messages are trivially
attributable to a device. `invocationIdentifier` gives request/response correlation — the hook for
reconstructing RPC pairs. `DDIProtocolVersion` refers to the Developer Disk Image services that
`devicectl -v` mentions mounting ("Enabling developer disk image services").

**(3) Payloads can be LZFSE-compressed.** 14 messages carried the **`bvx2`** magic — an LZFSE
compressed block. So a decoder must handle a compression layer *inside* the XPC payload, not just
raw XPC. (`bvx1`/`bvx2`/`bvxn`/`bvx-` are the LZFSE block magics; `compression_decode_buffer` with
`COMPRESSION_LZFSE` unpacks them.)

**Side-channel dominance:** 88 of 223 dumps were `XPCSideChannel.uniqueIdentifier` — one UUID per
channel. Combined with the connection-per-service topology (§3), the model looks like: open a TCP
connection, announce a side-channel UUID, then run typed RPCs over it.

Message sizes clustered under the 4 KB dump threshold (largest dumped: 4009 B). Anything larger was
counted into the histogram rather than dumped, by design — see the throughput note in
`scripts/coredevice-capture/cdcap.py`.

## 6. Comparison with CarPlay/AirPlay (why this is interesting for CarCast)

| | CoreDevice / DeviceHub | CarPlay / AirPlay (what CarCast implements) |
|---|---|---|
| Server role | **Device** serves; Mac is client | **Head unit** serves; phone connects in |
| Transport | QUIC + TLS-PSK + raw public keys, over a utun IPv6 tunnel, MTU 16000 | TCP; AirPlay pair-setup/pair-verify, then RTSP-ish HTTP + RTP |
| Service model | one TCP connection per service request to a multiplexer port | one control connection, streams SETUP-negotiated by type (110 screen, 103 buffered audio, 130 data) |
| Message encoding | Swift `Codable` | binary plists / HTTP-style headers |
| Discovery | pairing + CoreDevice feature list (64 advertised) | Bonjour + `/info` capability dict |
| Video | `startmediastream`, codecs h264/hevc | H.264 in the screen stream |

The shared piece is TLS-PSK from a pairing secret. The interesting divergence is the direction: a car
must serve because it owns the screen, whereas a debugging host pulls because it owns the operator.

## 6a. CoreDevice vs iPhone Mirroring — related goal, different stack

Natural hypothesis: DeviceHub's screen streaming is iPhone Mirroring under the hood. **The evidence
says no.** We have a deep prior reverse-engineering of iPhone Mirroring in **`~/iPhoneMirroring/`**
(`PROTOCOL.md` ~60 KB, `HANDOVER.md`, a client implementation, memory snapshots), and the two stacks
differ at every layer below "put a phone screen on a Mac".

| | **iPhone Mirroring** (`~/iPhoneMirroring/PROTOCOL.md`) | **CoreDevice / DeviceHub** (this doc) |
|---|---|---|
| Frameworks | `ScreenContinuityServices` → `Rapport`, `ReplicatorServices`, `Sharing` | `CoreDevice` → `RemotePairing` |
| Interfaces | `awdl0` (control) **+** `llw0` (media), different addressing per plane | one `utun` tunnel, IPv6 ULA |
| Control transport | Rapport over **plain TCP** (Rapport-internal AEAD) — **but see §6d: QUIC is also used** | **QUIC + TLS-PSK** + RFC 7250 raw public keys |
| Control framing | Rapport 4-byte header + **OPACK** encoding | 24-byte **RemoteXPC** header (`0x29b00b92`) + **XPC** serialization (`0x42133742`) |
| Auth | SPAKE2 Pair-Verify + IDS authentication | pairing identities as TLS-PSK / raw public keys |
| Media plane | **UDP / SRTP** (RFC 3711) on `llw0`, RTP-fragmented AVC, RTCP feedback | INFERRED: over the same tunnel (port 52631); codecs h264/hevc |
| Media keys | `_streamKey` → SRTP master, double encryption | unknown — not yet captured |

What they *do* share: pairing-derived key material, AEAD everywhere, H.264/AVC video, Apple ULA
`fd**::` addressing, and the device acting as the media source. So the family resemblance is real at
the design level — Apple reuses the same *ideas* — but a receiver written for one will not speak the
other.

**Practical consequence:** the `~/iPhoneMirroring` work does not directly decode CoreDevice traffic.
It is still valuable as a *prior*: it tells us what Apple's media plane tends to look like (SRTP over
UDP with RTCP feedback, AVC negotiated via a plist offer/answer carrying `streamPort`), which is the
first hypothesis to test when we finally capture CoreDevice's media stream (§7 Q2/Q3).

Also worth carrying over: that investigation found **AWDL unicast is gated to Apple-signed
processes**, which forced a third-party client onto infra Wi-Fi. CoreDevice has no equivalent
restriction visible so far — its tunnel is an ordinary `utun` any process can route over, though the
QUIC/TLS-PSK keys still come from the pairing.

## 6b. Should we build screen mirroring on this? — mostly NO

Assessed 2026-07-26 against what **`~/rplay`** already ships. Conclusion: **do not rebuild mirroring
on CoreDevice.** The one part worth investigating is *control*.

### Mirroring: no advantage

`~/rplay/docs/macos-usb-mirroring.md` — rplay already receives an iPhone screen over USB via the
**CoreMediaIO DAL plug-in** (`iOSScreenCapture.plugin`): the phone appears as an `AVCaptureDevice`
and is driven with a normal `AVCaptureSession`.

| | CoreMediaIO (rplay today) | CoreDevice (this doc) |
|---|---|---|
| API status | **public** since macOS 10.7, `CMIOHardwareSystem.h` | private, reverse-engineered |
| Setup | Trust This Computer + usbmuxd (on by default) | developer pairing **+ likely a personalized Apple-TSS-signed DDI** |
| Entitlements | none — any valid signature | unknown; some features may be gated |
| Media format | solved: video+audio via AVFoundation | **not yet decoded** |
| Stability | Apple-supported | may change without notice |

Rebuilding this on a private QUIC/RemoteXPC protocol with a runtime Apple dependency would be
strictly worse. Also note rplay has a second path already (AirPlay, phone-initiated), so mirroring is
doubly covered.

The "cross-platform gap" argument is weak too: the QuickTime-USB screen-capture protocol is already
reverse-engineered and cross-platform, so CoreDevice is not the only route off macOS.

### Control: possibly worth it

`~/rplay/doc/airplay-automation.md` — rplay drives the phone over **Bluetooth iAP, sending simulated
HID touch reports**, including tapping through Control Center to start mirroring with no user
interaction. It works, but the doc documents the cost: per-iPhone coordinate scripts in
`states.json` ("Customizing the script for your iPhone"). Coordinate-based UI automation is brittle
across models, iOS versions, locales, and layout changes, and it needs a Bluetooth pairing *in
addition* to the USB link.

DeviceHub controls the device **over the same tunnel it streams on** — no Bluetooth, no coordinates.
If that control channel is decodable, it could replace the fragile half of rplay's stack while
leaving mirroring untouched. That is a far better-shaped bet than reimplementing video.

**Caveat:** the DDI dependency applies to control too, so this trades *BT pairing + per-device
scripts* for *developer pairing + mounted DDI*. Which is better depends on which friction hurts more.

**Cheap test, no new tooling:** control messages are small, and `cdcap.py` dumps everything ≤4 KB in
full — the same path that produced the RPC envelope in §5b. So: start `run-capture.sh`, drive the
phone from DeviceHub (tap / swipe / type), and read the trace for input-event RPCs. If they are
structured events rather than coordinate blobs, that answers the question for one capture.

## 6c. WHERE TO LOOK — processes and frameworks (corrected 2026-07-26)

**We spent a session instrumenting the wrong process.** A count-only capture on
`CoreDeviceService` during heavy interactive use (taps, keyboard, rotate, Home) logged only ~10
outbound sends in 100 s, while control demonstrably worked. The earlier `lsof` already said why:

```
CoreDeviceService   334 connections   <- device management / DDI / RPC plumbing
DeviceHub            16 connections   <- its OWN connections: screen + input
```

`CoreDeviceService` carries management traffic only — which is exactly what the 223-message capture in
§5b contains (`coreDeviceVersion`, `deviceIdentifier`, `DDIProtocolVersion`, side-channel setup). No
pixels, no touches. **Instrument `DeviceHub` instead**: it is where video and input live, and being a
normal user-space app it is safer to hook than a launchd-managed daemon.

This likely also explains the earlier "I can't control the phone": payload dumping added milliseconds
to every `CoreDeviceService` send, and if DeviceHub's session depends on that daemon for keepalives or
channel management, stalling it breaks the session even though input flows elsewhere.

### Processes

| Process | Path | Owns |
|---|---|---|
| **DeviceHub** | `Xcode-beta.app/Contents/Applications/DeviceHub.app` | **screen + input** |
| CoreDeviceService | `CoreDevice.framework/Versions/A/XPCServices/CoreDeviceService.xpc` | device mgmt, DDI, RPC |
| remotepairingd | `RemotePairing.framework/Versions/A/XPCServices/remotepairingd.xpc` | QUIC/TLS-PSK tunnel |

### Frameworks

DeviceHub links (filtered to the interesting ones): `DeviceKit`, `CoreDevice`, `CoreDeviceUtilities`,
**`AVConference`**, CoreGraphics, AppKit, SwiftUI.

| Framework | Role |
|---|---|
| **`AVConference`** | **the media plane.** FaceTime's stack: `RTCPReceiverReport`, `RtcpPSFBForLossFeedback`, `RtcpPSFBForLTRAck`, `VCVideoJitterBuffer*`, `VCVideoTransmitter*`, `VCVideoReceiverDeferredAssemblyOffset`, `VCScreenCaptureDisablePrivateContentCapture`, `VCAudioRelay` |
| `CoreDevice` / `CoreDeviceUtilities` | RemoteXPC framing + Swift `Codable` RPC (§4, §5) |
| `RemotePairing` | QUIC + TLS-PSK + RFC 7250 raw public keys (§3) |
| `DeviceKit` | Xcode's device abstraction above CoreDevice |

### Important correction to §6a

§6a concluded CoreDevice and iPhone Mirroring are "different stacks". That holds for the **control**
plane (RemoteXPC/XPC vs Rapport/OPACK) but is **wrong about media**: DeviceHub links `AVConference`,
the same RTP/RTCP/SRTP machinery behind FaceTime, and `~/iPhoneMirroring/PROTOCOL.md` established that
iPhone Mirroring's media plane is SRTP (RFC 3711). So the two **converge on AVConference for media**.

Practical consequence: the SRTP work in `~/iPhoneMirroring` — RFC 3711 IV format, the master→session
PRF, the `_streamKey` identity-KDF finding — is likely to **transfer directly** rather than being only
a loose prior. That is a much better starting position than assuming a novel media format.

### Suggested entry points, by payoff

1. `VCVideoReceiver*` / `RTCPReceiverReport` in **AVConference** — inbound video assembly.
2. `nw_connection_send` in **DeviceHub** — outbound input events; the direction the existing hook
   already reads, so the input format may fall out cheaply.
3. `VCScreenCapture*` — capture/negotiation side.

### Tooling lesson

Breakpoint-based capture is unsuitable for interactive or media-rate paths: each hit stops the
process and (with dumping) runs lldb expression evaluations, costing milliseconds per event. Use it
for setup/control traffic only. For input or video, build a **`DYLD_INSERT_LIBRARIES` interposing
dylib** (viable here since SIP is disabled) that appends to a buffer in-process — nanoseconds, no
process stop. Also: **always detach from inside lldb**; killing it externally can leave breakpoint
traps patched into the target, which left `CoreDeviceService` misbehaving until it was restarted.
The `cdcap_autodetach` helper attempts this but does **not** fire under `lldb -b` (the Python timer
thread is not scheduled in batch mode) — treat clean detach as still unsolved.

## 6d. CORRECTION — the two stacks are MORE alike than §6a claimed

§6a said "different stack" and built a table implying iPhone Mirroring is plain-TCP-only while
CoreDevice is QUIC. **That was wrong, and the original intuition that these are similar was right.**

**QUIC — verified for both.** `ReplicatorEngine` (iPhone Mirroring's media engine) imports:

```
_nw_parameters_create_application_service_quic     (from Network)
_nw_protocol_copy_quic_definition
_nw_protocol_metadata_is_quic
_nw_quic_set_keepalive
```

The "plain TCP" row came from `~/iPhoneMirroring/PROTOCOL.md`, which documents the **Rapport control
plane**; generalising that to the whole stack was the error. Both products use QUIC via
Network.framework. Note `nw_parameters_create_application_service_quic` — an *application service*
QUIC constructor, which is a stronger hint of shared plumbing than QUIC alone.

**AVConference — asymmetric evidence, unresolved.**
- **DeviceHub: statically linked** (verified in its load commands), and AVConference exports confirm it
  is the FaceTime media stack: `RTCPReceiverReport`, `RtcpPSFBForLossFeedback`, `RtcpPSFBForLTRAck`,
  `VCVideoJitterBuffer*`, `VCVideoTransmitter*`, `VCScreenCaptureDisablePrivateContentCapture`.
- **iPhone Mirroring: zero static imports** across `ReplicatorEngine`, `ReplicatorServices`, and
  `ScreenContinuityServices`. But prior runtime investigation found AVConference in use and SRTP on
  the media plane, and static import tables cannot see `dlopen` or usage from another process.
  Runtime observation wins; treat AVConference as used by both, with the linkage path for Mirroring
  still unidentified.

**A methodological warning that produced the original error:** `nm -u` on these frameworks returns
*empty* because the binaries live in the **dyld shared cache**, not on disk. Empty output reads like
"no such symbols" but actually means "no such file". Use `dyld_info -imports` / `-exports` /
`-dependents` for anything in the shared cache. The §6a conclusion rested on exactly this false
negative.

**Consequence, if AVConference/SRTP is common to both:** the SRTP work in `~/iPhoneMirroring`
(RFC 3711 IV format, master→session PRF, the `_streamKey` identity-KDF result) should transfer to
CoreDevice's media plane, making it the natural first hypothesis rather than assuming a novel format.

## 6e. THE INPUT PATH — `universalhidservice` over Wi-Fi (CAPTURED 2026-07-26)

Attaching to **DeviceHub** (not `CoreDeviceService`) during live tapping immediately produced the
input protocol. Raw trace: [`captures/devicehub-input-2026-07-26.txt`](captures/devicehub-input-2026-07-26.txt).

**The feature identifier:**

```
com.apple.coredevice.feature.remote.universalhidservice
```

**The request shape** (XPC dictionary inside the RemoteXPC frame, §5a):

```
messageType       = "Request"
payload           = { send : { _0 : <binary blob>,        <- the HID report
                               _1 : <double>              <- timestamp (IEEE754, 0x40.. exponent)
                             } }
featureIdentifier = "com.apple.coredevice.feature.remote.universalhidservice"
```

`_0` / `_1` are Swift's `Codable` encoding of **unlabelled positional parameters**, consistent with the
Swift RPC model in §4 — i.e. this is a Swift method `send(_ data: Data, _ time: Double)`.

**Other methods seen on the same feature:**

| Method | Payload | Note |
|---|---|---|
| `send` | `_0` blob + `_1` double | the actual input events (dominant) |
| `connectedServices` | *(none)* | enumerate available HID services |
| `resetGestureState` | `_0` | clears in-progress gesture state |

Also present in the vocabulary: **`GestureState`** (×3) and **`keyboard`** (×1) — so gestures and key
input are modelled explicitly, not just raw pointer deltas.

**Frame type.** These ride frame-type bytes **`01 01 00 00`** at offset 4 (204 of 273 messages), a
distinct value from the management envelope's `01 01 01 00`. Full set observed:

| bytes 4..7 | count | meaning |
|---|---|---|
| `01 01 00 00` | 204 | **Request/Response RPC (incl. HID input)** |
| `01 01 01 00` | 43 | CoreDevice management envelope (§5b) |
| `01 02 00 00` | 9 | XPC side-channel establishment |
| `01 00 40 00` | 9 | bare header, no payload — keepalive? |
| `01 00 00 00` | 9 | empty XPC dict (payload len 0x14) — keepalive/ack? |

**Message sizes** for `universalhidservice` sends: **244 B dominant (166 of 212)**, plus 208/224/264/284.
The tight clustering suggests a fixed-size HID report for the common event with a few variants —
consistent with distinct report IDs (touch vs keyboard vs button).

### Why this matters for `~/rplay`

This is the outcome §6b hoped for. Input is a **HID service reachable over the Wi-Fi tunnel** — no
Bluetooth, no coordinate scripts:

- rplay currently drives the phone over **BT iAP with simulated HID touch reports**, needing a BT
  pairing plus per-iPhone coordinate scripts in `states.json`.
- CoreDevice carries **HID reports over the same Wi-Fi tunnel** already used for viewing. If the `_0`
  blob is a standard HID report, rplay's existing report-generation logic may transfer nearly as-is —
  and there is prior HID work to build on in `~/rplay/iphone-mirror-client/HID_DESCRIPTORS.md` and
  `HID_LIVE_CAPTURE.md`.

**Not yet proven:** that `_0` is a *standard* HID report. The capture's hex preview was capped at 96
bytes, so the blob (which sits past that offset in a 244-byte message) was not recorded. `HEX_BYTES` is
now 320; one more capture with taps at known screen positions should reveal the layout — and tapping
**opposite corners** would make any coordinate fields obvious by inspection.

The DDI caveat from §6b still stands: this trades *BT pairing + per-device scripts* for *developer
pairing + a mounted DDI*.

## 6f. WHAT TO DECOMPILE — targets, technique, and the full RPC surface

> **Local copies of all five binaries are in `reference/coredevice-binaries/`** (121 MB, deliberately
> **not committed** — see the `.gitignore` and `README.md` there for what each one is and the exact
> commands to reproduce them). Open them straight from that folder in a decompiler; it also sidesteps
> the `nm`-on-cache-path false negative described below.

### Targets, by payoff

| # | Binary | Size | Why |
|---|---|---|---|
| **1** | `/Library/Developer/PrivateFrameworks/CoreDeviceUtilities.framework/Versions/A/CoreDeviceUtilities` | 14 MB | **The RPC surface. 89 feature identifiers**, including every HID feature and all media/display ones. Start here. |
| **2** | `/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/CoreDevice` | 8 MB | Framing + transport + `Codable` RPC machinery; only 9 feature strings, but this is where the RemoteXPC frame (§5a) is built. |
| **3** | `/Library/Apple/System/Library/PrivateFrameworks/RemotePairing.framework/Versions/A/RemotePairing` | — | Tunnel: QUIC + TLS-PSK + RFC 7250 raw public keys (§3). |
| **4** | `/System/Library/PrivateFrameworks/AVConference.framework/Versions/A/AVConference` | — | **Media plane** — RTP/RTCP/SRTP, `VCVideoReceiver*`, jitter buffer (§6c). |
| **5** | `Xcode-beta.app/Contents/SharedFrameworks/DeviceKit.framework/…/DeviceKit` | 12 MB | Xcode's abstraction above CoreDevice. Zero HID strings — a consumer, not an implementer. |
| — | `Xcode-beta.app/Contents/Applications/DeviceHub.app/Contents/MacOS/DeviceHub` | 440 KB | **Low value.** Thin SwiftUI shell; zero HID strings. The logic is in the frameworks. |

Device-side implementations live in the **DDI** (Developer Disk Image) that `devicectl` mounts, not in
these Mac-side binaries — relevant if you want the server half.

### Technique — these are Swift binaries

No ObjC classes, so `class-dump` is useless. What works:

```sh
CU=/Library/Developer/PrivateFrameworks/CoreDeviceUtilities.framework/Versions/A/CoreDeviceUtilities

# feature/RPC surface
strings -a "$CU" | grep -oE "com\.apple\.coredevice\.feature\.[a-zA-Z.]+" | sort -u

# Swift symbols, demangled -> full signatures WITH parameter names
dyld_info -exports "$CU" | grep -oE '\$s[A-Za-z0-9_]*HID[A-Za-z0-9_]*' | sort -u \
  | while read s; do swift demangle -compact "$s"; done
```

Demangling recovers real signatures, e.g.:

```
CoreDevice.CoreDeviceError.hidRemoteCallFailed(deviceIdentifier: Foundation.UUID,
                                               underlyingError: Swift.Error?)
CoreDevice.CoreDeviceError.hidServiceIDInvalid(deviceIdentifier: Foundation.UUID?,
                                               serviceID: Swift.String,
                                               userInfo: [Swift.String : Any])
CoreDevice.CoreDeviceError.hidDeviceNotAvailable
CoreDevice.CoreDeviceError.hidDeviceNotSupported
```

Note `serviceID: String` — HID services are addressed by an ID, matching the `connectedServices`
method seen on the wire (§6e). Chasing the `Codable` conformances (`encode(to:)` / `init(from:)`, §4)
of each request type gives the field layout directly, since those are the functions that produce the
XPC dictionaries we captured.

### On disk vs inside the dyld shared cache

This decides which tools work, and it split cleanly along `/Library/…` vs `/System/Library/…`:

| Framework | Location | Standalone file? |
|---|---|---|
| CoreDevice (8.0 MB) | `/Library/Developer/PrivateFrameworks/` | **yes — on disk** |
| CoreDeviceUtilities (14.1 MB) | `/Library/Developer/PrivateFrameworks/` | **yes — on disk** |
| RemotePairing (7.1 MB) | `/Library/Apple/System/Library/PrivateFrameworks/` | **yes — on disk** |
| DeviceKit (12.1 MB) | `Xcode-beta.app/Contents/SharedFrameworks/` | **yes — on disk** |
| AVConference | `/System/Library/PrivateFrameworks/` | **no — shared cache** |
| ReplicatorEngine | `/System/Library/PrivateFrameworks/` | **no — shared cache** |
| Rapport | `/System/Library/PrivateFrameworks/` | **no — shared cache** |

**Good news for the CoreDevice work: all four primary targets are ordinary files**, so a decompiler
opens them directly with no extraction step. Only the media/mirroring frameworks need unpacking.

Cache location on this machine (macOS Tahoe):

```
/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/
    dyld_shared_cache_arm64e            <- main file, pass THIS one
    dyld_shared_cache_arm64e.01
    dyld_shared_cache_arm64e.02.dylddata
    dyld_shared_cache_arm64e.03.dyldreadonly
    dyld_shared_cache_arm64e.04.dyldlinkedit
```

**Extraction works** — `ipsw` is already installed (`/opt/homebrew/bin/ipsw`, v3.1.651). Verified:

```sh
ipsw dyld extract \
  /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e \
  /System/Library/PrivateFrameworks/AVConference.framework/Versions/A/AVConference \
  --output ./extracted
# -> ./extracted/AVConference, 85 MB, "Mach-O 64-bit dynamically linked shared library"
```

(The command self-describes as experimental, but produced a valid Mach-O.) The extracted image is what
you load in Hopper/IDA/Binary Ninja; the in-cache path cannot be opened directly by most decompilers.

**Tooling reminder:** for anything in the cache, `nm` returns *empty* — there is no file to read, which
looks identical to "no such symbols" and is exactly the false negative that produced the wrong
conclusion in §6a. Use `dyld_info -imports/-exports/-dependents` against the in-cache path, or extract
first with `ipsw`.

### The full 89-feature RPC surface

```
acquireusageassertion   applicationcontrol      audioinput              audiooutput
capturescreenshot       capturesysdiagnose      companiondevicepairing  configurationprofiles
connectdevice           customizeappearancesettings  customizeliquidglass  customizeuistyle
default.user.credentials  disableddiservices    disconnectdevice        displayinfoupdates
enableddiservices       enterDFU                exportVirtualMachineArchive  faceid
featureflags            fetchappicons           fetchddimetadata        fetchdyldsharecache
fetchmachodylibs        fileNodeDetails         filesystemoperation     getauthlistingidentifiers
getdeviceinfo           getdisplayinfo          getlockstate            getmediastreamserverstatus
getmediasupportinfo     inititatepairing        installapp              installProjectSet
installroot             launchapplication       listFiles               listroots
listusageassertions     monitorfilechanges      monitorprocesstermination  nvram
observedarwinnotification  opticid              pairingchallengeresponse  pasteboard
processcontrol          provisioningprofiles    querymobilegestalt      rebootdevice
receiveFiles            remote.devicecontrol.orientation                remote.hid.button
remote.hid.digitizer    remote.hid.keyboard     remote.hid.pointer      remote.hid.scroll
remote.hid.vendordefined  remote.universalhid   remote.universalhidservice
renamedevice            resizableappmanagement  rsyncfiles              screenrecording
sendmemorywarningtoprocess  sendsignaltoprocess  servicexpcpeerconnection  simulatefaceid
simulatelocation        simulateopticid         simulatetouchid         spawnexecutable
startaudiooutput        startmediastream        startvideooutput        stopmediastream
streamapplist           streamprocesslist       streamresizabilitystate  tags
touchid                 transferFiles           uninstallapp            uninstallroot
unpairdevice            viewdevicescreen        voiceover
```

**The HID decomposition is the important part.** Input is split along **standard HID usage pages** —
`digitizer` (touchscreen), `keyboard`, `pointer`, `button`, `scroll`, `vendordefined` — plus
`devicecontrol.orientation` for rotation. These are literal HID category names, which is much stronger
evidence that the `_0` blob in §6e is a **standard HID report** than anything inferable from the bytes
alone. `universalhid` / `universalhidservice` look like the umbrella carrier for all of them.

Also note `audioinput` / `audiooutput` / `startaudiooutput` — audio is a first-class feature, so a
mirroring client could get sound over the same tunnel.

## 6g. NO Apple ID required — the decisive practical difference

iPhone Mirroring requires the Mac and iPhone to be signed into the **same Apple ID**. CoreDevice does
not. Confirmed structurally by dependency analysis:

| | identity frameworks linked | auth model |
|---|---|---|
| **iPhone Mirroring** (`ReplicatorEngine`) | **`IDS`, `Accounts`, `AuthKit`, `UserManagement`** | Apple-ID / iCloud bound (matches the "IDS authentication" section of `~/iPhoneMirroring/PROTOCOL.md`) |
| **CoreDevice** + **RemotePairing** | **none** | `manualPairing` — TLS-PSK + RFC 7250 raw public keys from the *device pairing* |

So CoreDevice authenticates a **machine-to-device trust relationship** ("Trust This Computer"), not a
user account. That has consequences the media/protocol details do not:

- **Any paired device works** — test phones, borrowed devices, a lab fleet, devices belonging to other
  people. None of them need to share an Apple ID with the host, and no iCloud sign-in is involved.
- **Fleets are practical.** One Mac can drive many devices; with iPhone Mirroring every device would
  have to be on the host's account, which is a non-starter for QA fleets or automation rigs.
- **No account state to break.** Nothing depends on iCloud availability, 2FA prompts, or an account
  being signed in on the phone.

**Why this matters most for `~/rplay`:** its automation use cases — the MCP server, agents driving a
phone, `blockblast_solver` — are exactly "drive an arbitrary device from a host". Apple-ID coupling
would rule out test devices and customer devices entirely; pairing-based trust does not. Combined with
§6e (HID input over the Wi-Fi tunnel), this is the strongest argument yet for the control path:

> semantic HID input, over Wi-Fi, to **any paired device**, with no Bluetooth pairing, no per-device
> coordinate scripts, and no shared Apple ID.

The DDI dependency (§6b) remains the one real cost, and is now the *only* remaining setup friction
worth weighing.

## 6h. This is effectively "adb for iOS" — and only ONE primitive is missing

`devicectl` already ships most of an adb-equivalent, and CoreDevice's pairing-based trust (§6g) means
it works against **any paired device** without a shared Apple ID. For an AI agent driving a phone —
rplay's MCP server, `blockblast_solver` — that is the platform that has always been missing on iOS.

### Available TODAY via CLI — no reverse engineering

| Capability | Command |
|---|---|
| Screenshot | `devicectl device capture screenshot --device <id> --destination x.png` |
| Per-display screenshot | same + `--display-unique-id <uuid>` (`device info displays` lists them) |
| Orientation | `devicectl device orientation get | rotate | set` |
| **Pasteboard (text in/out)** | `devicectl device pasteboard …` — a text-entry shortcut that avoids keyboard HID entirely |
| Install / uninstall | `devicectl device install app` / `uninstall app` |
| Launch / kill / list processes | `devicectl device process …` |
| Files both directions | `devicectl device copy from|to`, `info files` |
| Darwin notifications | `devicectl device notification` (post + observe) |
| Simulate location / biometrics / status bar | `devicectl device simulate …` |
| Device + display info | `devicectl device info details|displays` |
| Reboot, profiles, sysdiagnose | `devicectl device reboot|profile|sysdiagnose` |

Screenshot latency measured ~1.6 s end-to-end (`devicectl -v`), which is workable for a
perceive-act agent loop, if not for video-rate control.

### The ONE gap: touch / keyboard input

`devicectl device simulate` covers only **biometrics, location, statusBar** — there is **no touch
subcommand**. Yet the device advertises the full input surface:

```
remote.hid.digitizer   remote.hid.keyboard   remote.hid.pointer
remote.hid.button      remote.hid.scroll     remote.hid.vendordefined
remote.universalhid    remote.universalhidservice
remote.devicecontrol.orientation      <- this one IS exposed, as `device orientation`
```

So the capability is present on the device and simply unexposed by the CLI. `orientation` being both
a `remote.devicecontrol.*` feature *and* a shipped subcommand proves the CLI can drive these features —
it just doesn't offer the HID ones.

**That makes the §6e capture the whole remaining piece:** implement
`com.apple.coredevice.feature.remote.universalhidservice`'s `send(_0: <HID report>, _1: <timestamp>)`
over RemoteXPC, and the loop closes — screenshot to perceive, HID to act, everything else already
scriptable.

### DDI friction is smaller than feared

`devicectl manage ddis` offers only `clean` and `update`, described as managing "developer disk images
installed on **the host**". So the DDI is a **host-side image set maintained by Xcode**, not a
per-action Apple round-trip. A product would need to ship or fetch that image set once, rather than
call Apple for every session. Still a dependency — but bounded, and much less onerous than §6b feared.

### Compared with adb

| | adb (Android) | CoreDevice (iOS) |
|---|---|---|
| Screenshot | `adb exec-out screencap` | ✅ `capture screenshot` |
| Tap / swipe / key | `adb shell input tap x y` | ❌ **not exposed** — needs the `universalhidservice` RPC |
| Install / launch | ✅ | ✅ |
| Files | ✅ | ✅ |
| Shell | ✅ | ❌ (no general shell; `process`/`spawnexecutable` are narrower) |
| Auth model | USB debugging authorisation | device pairing — **no Apple ID** (§6g) |
| Transport | USB / TCP | **Wi-Fi tunnel** or USB |

## 7. Open questions

1. Does a **CarPlay display** appear in `info displays` while CarPlay is connected? If yes, we get
   scriptable car-screen screenshots — a significant testing upgrade.
2. What is the actual framing on port 52631 (INFERRED media stream)? Needs a working §5 capture.
3. Is the video a raw H.264 elementary stream, or wrapped (RTP-like, or CMSampleBuffer-ish)?
4. What does `startvideooutput` do that `startmediastream` doesn't — external display support?
5. Is `viewdevicescreen` a capability gate (permission) rather than a data path?

## Reproducing

```sh
export DEVELOPER_DIR=/Applications/Xcode-beta.app/Contents/Developer
D=COREDEVICE-ID-REDACTED

xcrun devicectl device info details --device $D | grep -iE "Tunnel|Transport"
xcrun devicectl device info details --device $D | grep -oE "com\.apple\.[a-zA-Z0-9._]+" | sort -u
xcrun devicectl device info displays --device $D
ifconfig | awk '/^[a-z]/{i=$1} /inet6 fd/{print i, $2}'      # find the utun
netstat -an -p tcp | grep fd15                                # connections over the tunnel
lsof -nP -i6TCP | grep -E "52613|52631"                       # which process owns them
nm -u /Library/Apple/System/Library/PrivateFrameworks/RemotePairing.framework/Versions/A/RemotePairing \
  | grep -iE "quic|pre_shared_key|raw_public_key"              # prove it is TLS-PSK
```
