# Linux port — handoff

Written 2026-08-25, closing out the session that removed the engine's last portability wall
(userspace TCP/IP) and evaluated ffmpeg software decode for the Linux client. Live state (which
device is attached, whether a daemon runs) should be re-checked, not assumed.

## State

Branch `rendering-resolution-switch`, 68 commits ahead of main, **nothing pushed**. HEAD is
`80c6a12` "Userspace TCP/IP over the tunnel (lwIP): the engine runs non-root". The engine builds
static (`make STATIC=1`, 4.0 MB, no CoreFoundation, no installed deps). `deps/lwip` is vendored
but gitignored like the other deps — a fresh clone needs
`git clone --branch STABLE-2_2_1_RELEASE https://github.com/lwip-tcpip/lwip.git deps/lwip`.

## Where the port stands

The engine (`host-c/`) is now portable and unprivileged. All three engine seams are closed:

| Seam | Resolution |
|---|---|
| usbmux/lockdown/TLS | libimobiledevice (works against open-source usbmuxd on Linux) |
| Plists / CoreFoundation | libplist everywhere; CF is gone from the build |
| Tunnel interface (root/utun) | lwIP userspace TCP/IP — `RPLAY_USERSPACE_NET=1`, no root, no TUN |

Verified non-root against the phone on 2026-08-24: RSD enumeration, screencaptureservice
screenshot (1170x2532), installation_proxy (301 apps), AFC, DDI mounter — the whole control path
over lwIP. Details in `doc/LIBIMOBILEDEVICE-MIGRATION.md` ("Userspace TCP/IP" section).

Still open on the engine:
- **Phase 2 — media.c over lwIP.** The live-video path (RTP over UDP) still uses kernel sockets,
  so mirroring needs the root utun for now. Converting media.c to lwIP UDP sockets makes video
  fully no-root. The SDK/agent path is screenshot-based and already needs nothing.
- **syslog streaming** mixes a kernel client fd and a tunnel fd in one `select()`; in userspace
  mode the tunnel fd is an lwIP fd (>= 768) and `select` won't cover both. Needs a small pump.

The remaining seams are **client-side** (the macOS app is Swift/AppKit): video decode/display and
input capture. That is a new thin client, not a change to existing code — which is what the
ffmpeg question below is about.

## ffmpeg software decode: evaluated, works — with one required extra

Question: can a Linux client use ffmpeg's software HEVC decoder on this stream? **Yes.**
Tested 2026-08-25 with ffmpeg 8.1.2 against real captures of the device's own stream.

**The stream** (what the engine serves on TCP 9877): bare Annex-B HEVC, no container, no
timestamps, VPS/SPS/PPS in-band (cached and re-sent to a joining client), **one IDR per session**
— every later frame references the chain, so a client must never drop an access unit before
decode. HEVC **Main profile, 8-bit 4:2:0 full-range** (`yuvj420p`), 1184x2576, level 5.0.
H.264 can also be negotiated (`stream_info` says which).

**Correctness.** `reference/captures/apple_video_REFERENCE.h265` is a real 601-frame capture in
which every slice NAL carries the RVRA active-rect trailer (measured 601/601). ffmpeg decodes
601/601 frames with `-err_detect explode` and **zero warnings** — the trailer sits past
`rbsp_slice_trailing_bits`, so a conformant decoder ignores it by spec. `gop-reproducer.h265`
also decodes clean. (`screen3.h265`, an old ad-hoc capture at the repo root, shows a few
"invalid NALU" warnings — dirty capture, not a stream property.)

**Performance** (M-series Mac, `-benchmark`, 1184x2576):

| Threads | Speed | Effective fps |
|---|---|---|
| default | 70.8x realtime | ~1770 |
| 2 | 23.8x realtime | ~595 |
| 1 | 13.5x realtime | ~338 |

Screen content is cheap to decode. Even a single thread has 5-6x headroom at 60 fps, so a 2016
Intel Mac or a modest Linux box is fine. For live mirroring use `threads=1` (or slice threading):
frame-threading adds N frames of latency and is not needed at these speeds.

**The one thing ffmpeg will not do for you: the active-rect trailer.** Under motion (RVRA) the
encoder drops the coded picture below the SPS size and squeezes the whole screen into the
top-left corner of the same 1184x2576 frame; the appended trailer
(`[w:u16be][h:u16be][00...][session tag]`, tiers 1184x2576 / 1088x1920 / 720x1280) is the only
wire signal. ffmpeg decodes the frame happily either way — but the renderer must port
`parseActiveRectTrailer` (HEVCStream.swift:355, ~25 lines) and crop/upscale the active rect, or
motion frames render squeezed into a corner. See `doc/RVRA-AND-PORTABILITY.md`.

**Client shape that follows.** Read TCP 9877 → split Annex-B (AnnexBParser is ~40 lines) →
assemble access units → strip/record trailer → `avcodec_send_packet`/`receive_frame`
(feed the decoder directly; do not run the file demuxer on a live socket) → crop to active rect →
scale → display (SDL2 texture is the obvious choice, and SDL also answers the input-capture
seam). Control is the JSON API on 9876 — already portable, already exercised by rplayhub-sdk.

## Suggested next steps, in order

1. Phase 2: media.c → lwIP UDP (finishes no-root video; also what TestFlight needs).
2. Spike the Linux/portable client: ffmpeg + SDL2 consuming 9877 + 9876. The decode side is
   proven above; the spike is really about the render/input loop.
3. Push the branch (68 commits, nothing pushed).

## Kickoff text for a fresh session

Paste this to start the next session:

> Continue rplay-hub on branch `rendering-resolution-switch` (68 commits ahead of main, nothing
> pushed). Read `doc/LINUX-PORT-HANDOFF.md` first — it has the current state. Summary: the C
> engine is fully portable and runs non-root via lwIP userspace TCP/IP (`RPLAY_USERSPACE_NET=1`;
> control path verified against the phone); ffmpeg software HEVC decode of our stream is
> evaluated and works (601/601 frames clean, 13.5x realtime single-thread — the client just has
> to port the ~25-line active-rect trailer parser). Remaining: (1) convert `host-c/media.c`
> (RTP/UDP video) to lwIP so live video is also no-root, (2) a portable ffmpeg+SDL2 client spike
> for the Linux port, (3) push the branch. The daemon needs sudo to (re)start — ask me rather
> than trying. Task for this session: <fill in>.
