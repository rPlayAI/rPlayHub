# How this works as processes on macOS

Everything below was observed on this machine (macOS 26.5.2, Xcode-beta 27) rather than taken from
documentation. Commands to re-derive each fact are included, because the details move between
releases and second-hand descriptions of Apple's daemon layout age badly.

## Apple's side

```
  ┌──────────────────────────────┐
  │ DeviceHub.app                │  user: rplayai          UNPRIVILEGED
  │ Xcode.app / devicectl        │  entitled com.apple.private.coredevice.client
  └───────────┬──────────────────┘
              │ XPC to Mach services
              ▼
  ┌──────────────────────────────┐  ┌────────────────────────────────┐
  │ CoreDeviceService.xpc        │  │ remotepairingd.xpc             │  user
  │ CoreDevice.framework         │  │ RemotePairing.framework        │
  └───────────┬──────────────────┘  └────────────────────────────────┘
              │
              ▼
  ┌──────────────────────────────┐  ┌────────────────────────────────┐
  │ remoted                      │  │ usbmuxd                        │
  │ /usr/libexec/remoted         │  │ MobileDevice.framework          │
  │ user: ROOT                   │  │ user: _usbmuxd                  │
  │ creates the utun             │  │ unix socket /var/run/usbmuxd    │
  └───────────┬──────────────────┘  └────────────────┬───────────────┘
              │                                      │
              ▼                                      ▼
        utun8  fd94:21d2:b154::2/64            USB / wifi to the iPhone
```

### The privilege boundary is one line

**Only `remoted` is root, and the only thing it does that needs root is create the `utun`.**
Everything above it — service discovery, screen capture, HID — runs unprivileged and talks over
XPC. That is the entire reason Device Hub can be an ordinary app.

```sh
ps -axo user,pid,comm | grep -E "remoted|usbmuxd|remotepairingd|DeviceHub"
#   root      355  /usr/libexec/remoted
#   _usbmuxd  381  .../MobileDevice.framework/.../usbmuxd
#   rplayai 1727  .../RemotePairing.framework/.../remotepairingd
#   rplayai      .../DeviceHub.app/Contents/MacOS/DeviceHub
```

### How they call each other

`remoted` is a launchd **system** daemon publishing Mach endpoints. Clients look them up by name
and send XPC messages; there is no socket involved on this hop:

```sh
launchctl print system/com.apple.remoted | grep '"com.apple'
#   com.apple.remoted.coredevice          <- the one CoreDevice clients use
#   com.apple.remoted.virtualization
#   com.apple.remoted.compute-platform
#   com.apple.remoted.watchdog
#   com.apple.remoted.control
#   com.apple.remoted
```

`usbmuxd` is different: it is reached through a **unix socket**, not Mach.

```sh
ls -l /var/run/usbmuxd
#   srw-rw-rw-  1 root  daemon  /var/run/usbmuxd
```

Note the mode: **world read/write**. Any unprivileged process can connect, which is why our own
code enumerates devices, reads pair records and opens lockdown sessions with no privileges at all.
(A *sandboxed* process cannot — verified: identical binary, identical user, `CONNECTED` without the
sandbox entitlement and `EPERM` with it. See `app/DISTRIBUTION.md`.)

`CoreDeviceService` and `remotepairingd` are `XPCService` bundles inside their frameworks, so
launchd starts them **as the calling user**, on demand:

```
/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/XPCServices/CoreDeviceService.xpc
/Library/Apple/System/Library/PrivateFrameworks/RemotePairing.framework/Versions/A/XPCServices/remotepairingd.xpc
```

### The tunnel is the same shape as ours

```sh
ifconfig | grep -A3 utun8
#   inet6 fd94:21d2:b154::2 prefixlen 64   mtu 16000
```

A ULA `fd..::2` on a point-to-point `utun` at MTU 16000 — indistinguishable from the tunnels our
engine negotiates. Apple's daemon and our engine are doing the same thing; the difference is only
which process holds the privilege.

## Our side

```
  ┌──────────────────────────────┐
  │ rPlayHub.app                 │  user: you              UNPRIVILEGED
  │ Swift + AppKit, VideoToolbox │  never run this as root
  └───────────┬──────────────────┘
              │ loopback TCP, not XPC:
              │   127.0.0.1:9877  Annex-B HEVC video
              │   127.0.0.1:9876  newline-delimited JSON control
              ▼
  ┌──────────────────────────────┐
  │ the engine  (rplayhubd)      │  user: ROOT
  │ host/mirror.py today         │  creates the utun, owns the device session
  │ host-c/ + core/ in future    │
  └───────────┬──────────────────┘
              │ usbmux → lockdown+TLS → CoreDeviceProxy → utun → RSD → coredevice.*
              ▼
        the iPhone
```

Same split as Apple's, with two deliberate differences:

- **Loopback TCP instead of XPC.** XPC would tie us to macOS; TCP ports work identically on Linux
  and Windows, and let anything be a client — `ffplay tcp://127.0.0.1:9877` renders the video
  without our app involved, which is how we isolate engine faults from app faults.
- **We implement the protocol rather than calling Apple's daemons.** We cannot call them: the
  entitlements that authorize a CoreDevice client (`com.apple.private.coredevice.client`) require
  Apple's signing authority. Implementing it ourselves means the privilege lands on us, which is
  why our engine needs root exactly where `remoted` does.

Installed as a launchd daemon, ours sits in the same place theirs does:

```sh
sudo ./scripts/install-daemon.sh          # /Library/LaunchDaemons/com.rplay.rplayhubd.plist
     ./scripts/install-daemon.sh --status
```

## Why the daemon shape matters beyond packaging

The device permits **one media stream at a time**, negotiated with a lifetime, and it holds that
slot for the remainder of the lifetime if the client disappears without calling `stopmediastream`.
An engine that dies untidily therefore blocks every later session. launchd gives a defined
lifecycle and a clean SIGTERM on unload, and the engine releases the session on SIGTERM, SIGINT,
SIGHUP and `atexit`. That is a correctness property, not tidiness.

## Where to look when this is not enough

`reference/binaries/RE-MAP.md` maps protocol layers to binaries. The ones that matter here:

| question | binary |
|---|---|
| what a CoreDevice client may ask for | `CoreDeviceUtilities` — all 89 `com.apple.coredevice.feature.*` |
| RemoteXPC framing, transport | `CoreDevice` |
| the pairing handshake, QUIC/TLS-PSK tunnel | `RemotePairing`, and `remoted` for the device side |
| RSD, service discovery | `RemoteServiceDiscovery.macos` |
| **RTP/RTCP, jitter buffer, `RtcpPSFB*`** | `AVConference` — **not yet extracted**, dyld-cache only |

These are Swift, so `class-dump` yields nothing; use `dyld_info -exports` plus `swift demangle`,
and chase `Codable` `encode(to:)` for message layouts. Useful without any decompiler at all:

```sh
strings -a CoreDeviceUtilities | grep -oE "com\.apple\.coredevice\.(feature|action)\.[A-Za-z.]+" | sort -u
```

That is how `getmediastreamserverstatus` and `action.mediastreamstatus` were found — identifiers we
had been guessing at.

`AVConference` is the outstanding gap. It holds the RTCP feedback implementation, and we now know
this device **ignores PLI and FIR** (8 of each, measured, no keyframe produced), so its actual
keyframe mechanism is unknown and that binary is where the answer lives. It is the only one with no
on-disk copy:

```sh
ipsw dyld extract /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e \
  /System/Library/PrivateFrameworks/AVConference.framework/Versions/A/AVConference --output reference/binaries
```

---

# The actual goal: Linux/Windows, headless, for AI agents

macOS is where this was built because the protocol could be verified against Apple's own stack.
The target is agents driving real iOS devices from infrastructure — CI, device farms, cloud —
where there is no Mac, no display, and no human.

That changes what matters. **The JSON control API is the product; the GUI is a debugging tool.**
An agent needs `list_devices`, `take_screenshot`, `tap`, `swipe` — not a window.

## The detail that decides the headless design

**Touch is only delivered while a media stream is running.** The device routes HID to UIKit only
for the duration of a `startmediastream` session; with no stream, taps are accepted and silently
discarded. This is device-side behaviour, so it is true on every platform.

The consequence for a headless agent host is specific and non-obvious:

> You must negotiate and keep a video stream open in order to tap, **but you never have to decode
> it.** Receive the RTP, drop it on the floor, keep the session alive.

That removes the single biggest porting obstacle for the agent use case: no VideoToolbox, no
ffmpeg, no display surface, no `hwdecoder` backend. A headless host needs the transport, RSD,
`startmediastream`, RTCP receiver reports, and HID — nothing that decodes a frame. Screenshots come
from `screencaptureservice`, which is a separate service returning PNG and needs no stream at all.

So the port splits cleanly:

| capability | needs a decoder | notes |
|---|---|---|
| `list_devices`, `take_screenshot` | no | screencaptureservice returns PNG |
| `tap`, `swipe` | no | but the media stream must be **running** |
| live video for a human | yes | ffmpeg/SDL on Linux, Media Foundation or ffmpeg on Windows |

## What ports as-is, and what does not

| layer | Linux | Windows | state |
|---|---|---|---|
| usbmux | `usbmuxd` from libimobiledevice, same wire protocol, same socket path | Apple Mobile Device Service, same protocol over **TCP 127.0.0.1:27015** | client code is shared; only the endpoint differs |
| lockdown + TLS | OpenSSL | OpenSSL | portable |
| CoreDeviceProxy / CDTunnel | pure protocol | pure protocol | portable, already in C (`host-c/cdhost.c`) |
| **tun interface** | `/dev/net/tun` | **no native TUN** — needs Wintun or an OpenVPN-style signed driver | **the blocker** |
| RSD / RemoteXPC / XPC | portable C | portable C | `core/rp_xpc.c`, `core/rp_http2.c` — both byte-identical to the device-verified Python |
| media stream negotiation | portable | portable | protobuf-in-bplist offer, zlib level 9 |
| RTP depacketize + RTCP | portable C | portable C | `core/rp_video_stream.c` |
| HID | portable | portable | reports built by hand; see `host/hid.py` |
| decode + display | ffmpeg/SDL | Media Foundation or ffmpeg | **not needed headless** |

### The tun interface is the only real blocker, and it should be deleted

Everything below the tunnel already runs unprivileged — verified on two phones: usbmux, lockdown
with TLS, pair record, `StartService`, and the full CDTunnel handshake all complete as an ordinary
user. **Creating the `utun` is the only privileged step in the entire pipeline.**

A userspace TCP/IP stack over the tunnel socket removes it everywhere at once:

- no root on Linux, no driver on Windows, no privileged helper anywhere
- containers work — no `NET_ADMIN`, no `/dev/net/tun` mount, which is what makes cloud and CI
  deployment realistic rather than awkward
- it is also what would make a sandboxed Mac App Store build possible

Cost: UDP (trivial — it carries inbound RTP) plus a TCP client good enough for RSD and the
per-service HTTP/2 channels. The tunnel is point-to-point with both addresses known, so there is no
neighbour discovery and no routing. pymobiledevice3's `--userspace` tunnel is a working reference.

**For the agent/cloud goal this is the highest-leverage single piece of work in the project.**

## Transport reliability, and why usbmux is a dead end for the goal

Observed repeatedly, worth planning around:

| transport | after a device reboot | stability |
|---|---|---|
| USB | registers immediately | stable for hours |
| wifi (`ConnectionType=Network`) | absent for minutes, sometimes needs an unlock | iOS sleeps the entry; it comes and goes mid-session |

So **with usbmux as the only transport, USB is the reliable choice** — which is fine for a device
farm, where cables are normal, but it is not a solution for the project's goal.

The deeper problem is that usbmux is a macOS/Windows convenience, not the device's own door. On
2026-07-28 the iPhone 13 vanished from usbmuxd after a reboot while Apple's `remoted` held a live
tunnel to it the whole time (`utun8`, MTU 16000) and Device Hub mirrored it happily. Our engine was
blocked; nothing was wrong with the phone.

Meanwhile the device was advertising its own door on the LAN:

```
_remotepairing._tcp  →  6C4EACE0-…._remotepairing._tcp.local
                        Christinas-iPhone-2.local.:49152
_apple-mobdev2._tcp  →  fe:5e:b0:95:f1:eb@fe80::…-supportsRP-26     ("supportsRP")
_remoted._tcp        →  ncm
```

That is standard mDNS, which Avahi resolves identically on Linux. **RemotePairing is therefore not
an optimisation over usbmux — it is the only path that exists on a Linux host**, and it is the path
that was working on this Mac while ours was not. Today's failure was a preview of exactly what a
Linux port hits on day one.

(Measurement note: an earlier pass concluded the phone advertised nothing. That was wrong — the
shell had no `timeout` binary, so `timeout dns-sd …` produced empty output that was misread as
absence. Browse by backgrounding `dns-sd` and killing it, not with `timeout`.)

## Remote devices over the internet

The tunnel is a stream of raw IPv6 packets, so a relay needs exactly one splice point and
everything above it — RSD, screen, HID — rides across untouched:

```
  ┌─ where the phone is ──────┐            ┌─ where the agent runs ───────┐
  │ rplayhub agent            │            │ rplayhub host                │
  │  usbmux or RemotePairing  │◀── TLS ───▶│  relay transport → RSD → …   │
  └───────────────────────────┘            └──────────────────────────────┘
```

Design notes and the open questions are in `host/rplayhub/transport/relay.py`. The parts that need
deciding before writing code: authentication (this carries full device control, so a bare TCP
socket is not acceptable), and IPv6 address collisions when several relayed devices land on one
host, since each phone picks its own tunnel addresses.

Making a relayed phone visible to **Xcode** is a separate and harder problem — Xcode talks to
Apple's daemons, not to us, so the device has to be injected into Apple's own discovery. Two
candidate mechanisms are written up in `relay.py`; neither is verified.

## Ordering for the agent goal

1. **Userspace TCP/IP stack.** Removes root, the Windows driver, and the container obstacle in one
   piece of work. Everything else gets easier.
2. **Finish the C engine**: HTTP/2 and XPC are done and verified; RSD, `startmediastream` and HID
   remain. That is the headless host.
3. **RemotePairing** — direct wifi with no usbmuxd, which removes the last Apple-daemon dependency
   and is what makes a Linux host self-sufficient.
4. **Relay** for remote devices.

Note what is *not* on that list: video decode. It is needed for a human watching, not for an agent
acting.
