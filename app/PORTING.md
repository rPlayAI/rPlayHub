# Porting — macOS now, Linux and Windows next

We will run on Linux and Windows, so the work is to keep every OS dependency behind a named seam
and implement that seam per platform. There are only four seams. Everything else — the CoreDevice
protocol, RSD, RemoteXPC, the HEVC depacketizer, the control API — is portable and should never
learn which OS it is on.

## The four seams

### 1. Tunnel interface — the hard one

The CoreDevice tunnel is raw IPv6 packets. To reach RSD and the services with ordinary sockets,
those packets need to be on a real interface with a route. That is privileged, and it is different
everywhere.

| platform | mechanism | state |
|---|---|---|
| macOS | `AF_SYSTEM` / `com.apple.net.utun_control`, 4-byte address-family prefix per packet | working — `host/rplayhub/net/tun.py` `MacUtun` |
| Linux | `/dev/net/tun`, `IFF_TUN \| IFF_NO_PI` (no prefix), `iproute2` for address and route | written, **never run** — `LinuxTun` in the same file |
| Windows | no native TUN. Needs **Wintun** (a DLL and a driver) or the OpenVPN TAP driver; both mean shipping a signed driver | not started |

Interface: `configure(params)`, `recv_packet(timeout)`, `send_packet(packet)`, `close()`. The
per-platform packet prefix is normalized inside the implementation so the packet pump never sees it.

**This seam can be deleted, and probably should be.** A userspace TCP/IP stack over the tunnel
socket removes the interface, the route, the root requirement and the Windows driver — on all three
platforms at once. It is not an escape hatch; on the evidence it is the best available design:

- Everything below the tunnel already runs unprivileged (verified live on two phones: usbmux,
  lockdown + TLS, pair record, `StartService`, CDTunnel handshake — all as a normal user).
  **Creating the `utun` is the only privileged step in the whole pipeline.**
- Windows has no native TUN, so the alternative there is shipping a signed driver.
- It is also half of what a Mac App Store build would need (see `DISTRIBUTION.md`).

Cost: UDP (trivial — it carries the inbound RTP) plus a TCP client good enough for RSD and the
per-service HTTP/2 channels: handshake, sequencing, retransmit, windowing, teardown. No neighbour
discovery — the tunnel is point-to-point and we own both addresses. pymobiledevice3's `--userspace`
tunnel is a working reference.

### 2. Device discovery and transport

| platform | mechanism |
|---|---|
| macOS | Apple's `usbmuxd` on `/var/run/usbmuxd` — working |
| Linux | `usbmuxd` from libimobiledevice, same wire protocol and same socket path |
| Windows | Apple Mobile Device Service, same protocol over TCP `127.0.0.1:27015` instead of a unix socket |

Only the endpoint changes, so the client code is shared. Our `Transport` abstraction already covers
this, and the **RemotePairing** transport removes the dependency on any of these daemons entirely —
which is the real portability answer, since it is just TCP plus crypto.

### 3. Video decode and display

| platform | decode | display | state |
|---|---|---|---|
| macOS | **VideoToolbox** (hardware), driven by the display layer | `AVSampleBufferDisplayLayer` in an `NSWindow` | working |
| Linux | **ffmpeg `libavcodec`** (hardware via VA-API when available) | SDL2, GTK4, or Wayland/EGL | not started |
| Windows | ffmpeg, or Media Foundation / D3D11VA | D3D11 swapchain or SDL2 | not started |

**macOS uses VideoToolbox and links no ffmpeg at all.** `AVSampleBufferDisplayLayer` drives the
hardware decoder itself, given a format description and length-prefixed samples, which avoids owning
a `VTDecompressionSession` and a round trip through `CVImageBuffer`. An explicit session is only
needed when the pixels themselves are wanted — recording, capture, analysis, a Metal renderer — and
`refs/rplay/src/receiver/hwdecoder.m` is the reference for that.

**ffmpeg is the Linux backend**, deliberately kept in the plan rather than treated as a stopgap:
`libavcodec` gives HEVC decode across every distro and hardware combination we would otherwise have
to special-case, and it is already what verifies our streams (`ffprobe`, `ffplay`) on any platform.

**The interface is `core/hwdecoder.h`**, adopted byte-identical from `~/rplay` and
`~/carplay-dev` — the two projects already share it unchanged, which is the best evidence available
that the boundary is right. `hw_decode(decoder, window, data, size, flags)` takes packed frame
bytes, which is what `rp_pack_length_prefixed()` (VideoToolbox) or `rp_pack_annexb()` (ffmpeg)
produces. Everything above it — NAL splitting, codec classification, access-unit assembly, the
keyframe gate — is already implemented once, portably, in `core/rp_video_stream.c`. Feed it Annex-B HEVC; the engine already emits exactly that, parameter sets first. Note
that the decoded frame is **larger than the screen** — 16-pixel alignment padding with no conformance
window — so every backend must crop to the device size, and every input backend must map clicks
inside that cropped rectangle.

Two hard-won rules from the cloned code that are platform-independent and must survive porting:
when the display layer is not ready, **drop the frame rather than flushing** (flushing caused a lag
bug), and read the real frame dimensions **off the first decoded frame** rather than trusting the
format description.

### 4. Input capture

Turning a mouse event into a normalized 0..1 coordinate is the only portable part; capturing it is not.

| platform | mechanism |
|---|---|
| macOS | `NSWindow.sendEvent` override, then translate |
| Linux | SDL/GTK event loop, or evdev for global capture |
| Windows | Win32 window messages (`WM_LBUTTONDOWN`, `WM_MOUSEMOVE`, …) |

The routing is the same everywhere — capture a pointer event, turn it into a device coordinate,
send it — so only capture and send are per-platform. The maths between them is implemented once in
`core/rp_geometry.c`: `rp_fit_rect`, `rp_normalize_point` (which takes the y-axis convention as a
parameter, since AppKit unflipped is bottom-left while SDL and Win32 are top-left), and
`rp_fractions_to_hid`. Note it normalizes against the **device** size, not the coded video size —
they differ, see seam 3.

Our send path is `universalhidservice`, not rplay's iAP HID, so the reports differ even though the
coordinates do not.

### 5. Developer Disk Image mount — portable, not yet ours

iOS 17+ mounts a *personalized* DDI per boot before the developer services answer (see
`doc/DEVICEHUB-PARITY.md`, "What a user must do"). On a Mac, Xcode or Device Hub does it. Off a
Mac nobody will, so the port needs a mount module: read the phone's identity and nonce over
`mobile_image_mounter.shim.remote` (classic plist relay, already spoken from C), build the TSS
request from the image's `BuildManifest.plist`, POST it to Apple's signing server, then
`ReceiveBytes` + `MountImage` with the returned manifest. pymobiledevice3 has all of it working
from Linux and Windows and is the crib. The base image files are Apple's and fetchable; nothing
in the flow is macOS-specific. Developer Mode and the trust pairing remain the user's job on
every platform.

## Ordering

1. macOS View Screen, against the Python engine — proves the whole loop.
2. Move the engine into C so Linux and Windows have something to link. The seams above are the
   headers to define first.
3. Linux: `LinuxTun` is written but unrun, and libimobiledevice's usbmuxd is a drop-in, so a
   headless engine should come up quickly; the display backend is the real work.
4. Windows: decide TUN driver versus userspace stack **before** starting. That choice shapes
   everything else.

## Things that will bite

- **Privilege.** macOS and Linux need root to create an interface; Windows needs an installed
  driver. Any GUI has to either run elevated or talk to a small privileged helper. Decide which
  before building the app, because it changes the process model.
- **HID auth gate.** Touch only reaches UIKit while a media stream is running. This is device-side
  behaviour, so it is true on every platform: the engine must hold the stream open, which it does.
- **The tunnel's 16000-byte MTU** with IPv6 packet reframing from a byte stream — already handled in
  the pump, but any reimplementation must reframe using the IPv6 Payload Length field rather than
  assuming one read is one packet.
