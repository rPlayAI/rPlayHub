# Linux port — handoff

Written 2026-08-25; updated the same day by the session that converted media.c to lwIP and wrote
the portable client spike. Live state (which device is attached, whether a daemon runs) should be
re-checked, not assumed.

## State

Branch `rendering-resolution-switch`, 71 commits ahead of main, **nothing pushed — the repo has
no git remote configured**, so pushing first needs a destination. HEAD is "Portable client spike:
rplay-view (ffmpeg software decode + SDL2)". The engine builds static (`make STATIC=1`, 4.3 MB,
no CoreFoundation, no installed deps). `deps/lwip` is vendored but gitignored like the other
deps — a fresh clone needs
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

**Phase 2 — media.c over lwIP — is CODE-COMPLETE but not live-verified.** With
`RPLAY_USERSPACE_NET=1` the RTP/RTCP socket and the negotiation channel now ride the same lwIP
stack as the control path (usernet.c grew a small UDP API with an opaque peer blob; burst
headroom is sized in lwip-engine/lwipopts.h — PBUF pool, tcpip mbox, UDP recvmbox — replacing
the kernel SO_RCVBUF fight). Both `make` and `make STATIC=1` build clean. **Not yet run against
the phone** — it was not attached during that session. First live check: start the engine
non-root with `RPLAY_USERSPACE_NET=1`, start mirroring, watch `stream_info` for rtp_packets
climbing and viewer loss at 0.

Still open on the engine:
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

**The client spike EXISTS: `client-c/rplay-view.c`** (one file, libavcodec + SDL2, `make` in
client-c/). It implements exactly the shape above: TCP 9877 → Annex-B split → access units on
the first-slice flag → keyframe gating → trailer strip → single-thread decode → SDL source-crop
to the active rect → letterboxed scale. Codec is asked over 9876 (`stream_info`), overridable
with `--codec`. `--check` decodes headless and prints stats; `-f capture.h265 -r fps` plays a
file. Verified offline: apple_video_REFERENCE.h265 → 601/601 decoded, 601/601 trailers,
0 errors; gop-reproducer.h265 → 117/117 with the 720x1280 tier; windowed playback runs. **Not
yet run against a live 9877** (same reason: no phone attached). Input capture is future work —
SDL is also the answer to that seam.

## Suggested next steps, in order

1. Live-verify both new pieces at once, phone attached: engine non-root with
   `RPLAY_USERSPACE_NET=1`, then `client-c/rplay-view` against it. Compare loss/discontinuities
   with a kernel-mode run.
2. Configure a git remote and push the branch (71 commits, nothing pushed anywhere).
3. Engine: the syslog select() pump for userspace mode; client: input capture via SDL events →
   whatever input injection the engine grows.

## Kickoff text for a fresh session

Paste this to start the next session:

> Continue rplay-hub on branch `rendering-resolution-switch` (71 commits ahead of main; no git
> remote is configured, so nothing is pushed anywhere). Read `doc/LINUX-PORT-HANDOFF.md` first —
> it has the current state. Summary: the C engine is fully portable and non-root via lwIP
> userspace TCP/IP (`RPLAY_USERSPACE_NET=1`), now including the live-video path (media.c rides
> lwIP UDP — code-complete, builds clean, NOT yet live-verified); the portable client exists at
> `client-c/rplay-view.c` (ffmpeg + SDL2, verified against the reference captures, NOT yet run
> against a live 9877). Remaining: (1) live-verify engine + client with the phone attached,
> (2) set a git remote and push, (3) syslog pump for userspace mode and client input capture.
> The daemon needs sudo to (re)start — ask me rather than trying; the userspace-mode engine
> needs no sudo. Task for this session: <fill in>.
