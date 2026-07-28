# host/ — Python experiment harness

Our own host-side implementation of Apple's CoreDevice stack — usbmux → lockdown → tunnel → RSD →
screen/HID — built bottom-up and verified live. Dependency-light: stdlib + `cryptography` only.
pymobiledevice3 was used ONLY as an offline oracle for wire constants (never a runtime dependency).

**This is a harness, not a shipping artifact.** The shipping implementation is C (`../host-c/`, the
portable protocol core) and Swift (the macOS app with the Device Hub-style View Screen). Python is
where a wire format gets proven against a real phone cheaply, before it is committed to in C or
Swift. Keep it that way — it is much faster to debug a protocol here.

**Verified live:** iPhone 12 Pro (iOS 26.5.2) over USB; iPhone 13 Pro (iOS 27) over WIFI.
Screenshot ✅ · tap/swipe ✅ (Safari launched) · HEVC screen record ✅ (decodes; glitchy pending
RTCP feedback). No live view yet — video goes to a file.

## Files

The flat modules are the verified originals. They work; `tunnel_up.py` is the entry point.

| file | layer | what |
|---|---|---|
| `usbmux.py`     | 0   | usbmuxd unix-socket client: list devices (USB **and Network/wifi**), open pipes, read pair record |
| `lockdown.py`   | 1   | QueryType/GetValue/StartSession(+OpenSSL TLS)/StartService |
| `tunnel.py`     | 2   | CoreDeviceProxy `CDTunnel` handshake → live IPv6 tunnel |
| `tunnel_up.py`  | 3a  | **(root)** utun bring-up + splice; dispatches `{shot,tap,swipe,record}` |
| `xpc.py`        | 3   | XPC-object + RemoteXPC-wrapper codec (self-tested: `python3 xpc.py`) |
| `remotexpc.py`  | 3   | RemoteXPC channel over HTTP/2 — the transport every `coredevice.*` service speaks |
| `rsd.py`        | 3   | RSD service enumeration (peer_info → 85 services) |
| `coredevice.py` | 4   | CoreDevice invoke envelope; `screenshot()` (screencaptureservice) |
| `hid.py`        | 4   | touch/swipe injection (universalhidservice) — needs the media-stream auth gate |
| `screen.py`     | 4   | HEVC screen record: offer negotiation + RTP/HEVC depacketize (+ reorder) → Annex-B `.h265` |
| `probe.py`      | 0-2 | layer-by-layer reachability check against an attached device |
| `mirror.py`     | 5   | **the live engine**: permanent media stream (holds the HID gate open) + Annex-B HEVC broadcast on :9877 + JSON control API on :9876. Contract in `../app/api/PROTOCOL.md`. |

Run (root):
`sudo python3 tunnel_up.py [udid] {shot out.png | tap fx fy | swipe fx0 fy0 fx1 fy1 | record out.h265 secs}`

## `rplayhub/` — a partial refactor, deliberately halted

Started to turn the flat scripts into a reusable spine, then stopped once the shipping languages
were settled as Swift + C. It imports cleanly and changes nothing about the flat modules above.
Its value now is as **design that carries over to C and Swift**, not as Python to finish.

| module | state |
|---|---|
| `errors.py` | one exception hierarchy |
| `wire/` (`xpc`, `remotexpc`, `_cbor`) | moved verbatim — frozen codecs, change only with a live re-verify |
| `transport/` | `Transport` ABC + `TunnelLink`/`TunnelParams`; `usbmux_transport` built; `remotepairing`, `relay` are documented stubs |
| `net/` | `tun.py` (macOS verified, Linux written-but-unrun) + `pump.py` |
| `rsd.py` | `ServiceCatalog` — cached peer_info |
| broker / session / server / CLI | not written |

Three ideas in it are worth carrying into C and Swift:

1. **The transport seam.** A `Transport` returns a `TunnelLink`: a byte stream of raw IPv6 packets
   plus the addresses to route into it. Everything above is transport-agnostic, so usbmux,
   RemotePairing, and a relay are interchangeable and each is one file.
2. **The tun platform seam.** `recv_packet`/`send_packet` normalize away the fact that macOS utun
   prefixes each packet with a 4-byte address family and Linux `IFF_NO_PI` does not, so nothing
   above is platform-specific.
3. **Failure signalling in the pump.** The original `splice()` had a real bug: when the tunnel died
   one direction returned and the other raised inside a daemon thread, so a dead tunnel still
   looked up. Now either direction ending marks the pump failed exactly once and fires a callback,
   which is what a reconnect supervisor needs.

## Transport paths — where a product hooks in

Everything from the tunnel up is identical no matter how bytes reach the phone. Only the bottom
transport and its pairing differ.

### Path A — through Apple's usbmuxd (what we use today), Mac-tethered

```
our code ──unix socket──▶ macOS usbmuxd ──USB or WIFI──▶ iPhone
                          (Apple's daemon does discovery + transport)
```

`usbmux.connect(udid, port)` has usbmuxd relay a TCP stream to the phone, over the cable or over
wifi if the phone is paired with this Mac with wifi-sync on (`ConnectionType=Network`). The tunnel,
RSD, and services all ride inside that relayed stream; the utun only routes IPv6 into the tunnel
socket. **Limits:** needs Apple's usbmuxd, so it does not port off macOS, and needs pairing with
*this* Mac. The wifi `Network` entry is intermittent because iOS sleeps it, so the code waits it out.

### Path B — direct RemotePairing door (not built)

```
host ──wifi, direct TCP──▶ iPhone   (_rp-tunnel._tcp / _remotepairing._tcp, e.g. :49152/:65081)
   └─ RemotePairing handshake (SRP / Curve25519 / ChaCha20) → CoreDevice tunnel → RSD → screen+HID
```

The phone advertises those services on the LAN over mDNS, and connecting is plain TCP with no
usbmuxd anywhere. Needs the handshake (RPPairing framing + OPACK + SRP pair-setup / Curve25519
pair-verify → ChaCha20 stream), mapped in `../doc/REMOTEPAIRING-PROTOCOL.md`. For the C core, the
pairing kit is vendored at `../deps/AccessorySDK/` — `Support/PairingUtils.c` in particular.

### Hook points

Because we own the whole stack, a relay can be inserted at any layer:

1. **Transport swap (bottom):** replace Path A with Path B so we pair the phone ourselves and cut
   Apple's daemon out. Cleanest for portability.
2. **usbmux `Connect` relay:** relay each usbmux TCP connection to a remote usbmuxd. Entitlement-free
   but still Apple-usbmuxd-dependent and intermittent.
3. **CoreDevice tunnel relay:** relay the raw IPv6 tunnel bytes to a remote phone — one splice point,
   carries everything above transparently. Cleanest for "the phone is on another network".
4. **RSD service relay:** relay individual `coredevice.*` connections, for per-service policy
   (e.g. screen + HID only).

## Wire notes (ground truth)

- **usbmux**: unix socket `/var/run/usbmuxd`, `<u32 len><u32 ver=1><u32 type=8 PLIST><u32 tag>` + XML
  plist. `ReadPairRecord` → the stored pair record (no root needed). The same `Connect` works for USB
  and Network devices. `PortNumber` goes in network byte order.
- **lockdown**: `<u32 be len><XML plist>`. `StartSession{HostID,SystemBUID}` → `EnableSessionSSL` →
  TLS with HostCertificate/HostPrivateKey (SECLEVEL=0). Then `StartService` works, else
  `SessionInactive`.
- **CoreDeviceProxy** (`com.apple.internal.devicecompute.CoreDeviceProxy`): the lockdown SSL session
  IS the auth. `b"CDTunnel" + <u16 be len> + JSON`; handshake returns
  `{serverAddress, serverRSDPort, clientParameters}`; then raw IPv6 packets.
- **RemoteXPC**: HTTP/2 (`PRI * HTTP/2.0`), stream 1 = root, 3 = reply; XPC wrappers (magic
  0x29B00B92 / payload 0x42133742 v5, little-endian, 4-byte aligned). `receive()` MUST skip
  empty-dict `{}` acks — the device sends one before the real reply.
- **HID auth gate:** touch reaches UIKit only while a media stream runs, so open
  `screen.open_stream` before injecting. A permanent View Screen stream makes this free.
- **Video:** displayservice `startmediastream`; the offer is a protobuf mediaBlob inside a binary
  plist, zlib **level 9** (the device rejects other levels). RTP/HEVC (RFC 7798) over UDP →
  depacketize → Annex-B `.h265`. RTP reorder done; RTCP PLI/RR deferred, so remaining artifacts are
  packet loss.
