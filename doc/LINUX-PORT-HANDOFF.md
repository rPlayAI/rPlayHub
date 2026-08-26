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

## ffmpeg software decode: conformant, fast — and WRONG under motion (RVRA)

**CORRECTED 2026-08-25 (later session).** This section originally concluded "yes, ffmpeg works,
just port the trailer parser and crop." That is the same trap `doc/RENDERING-HANDOFF.md` documents
the macOS app falling into for a week: every integrity check passes while the picture is wrong.
`-err_detect explode` measures bitstream conformance, not pixel correctness. The authoritative
document is `doc/RVRA-AND-PORTABILITY.md` — read it before planning any non-VideoToolbox video
path. What remains true and what does not:

- **True**: the stream parses cleanly, the trailer is spec-invisible, performance numbers hold.
- **False**: that crop/upscale is sufficient. RVRA is *reference resampling* — when the encoder
  downshifts, references coded at the old size must be rescaled before prediction. Standard HEVC
  has no such mechanism; ffmpeg predicts from the unscaled reference and produces **garbage from
  the first downshift onward, with zero warnings and exit 0**. Re-verified empirically this
  session by dumping ffmpeg-decoded frames of apple_video_REFERENCE.h265: frame 44 (full tier) is
  clean; frames 46-70 (through and *after* the first downshift episode, including frames back at
  full tier) are a garbled mosaic. Recovery is incidental — fresh screen content slowly paints
  over the damage (frame 160 is mostly healed) — not guaranteed, since the session has one IDR.
  Only VideoToolbox with the private RVRA properties (`VideoResolutionAdaptationType=3` +
  `NegotiationDetails` + per-frame `ActiveVideoResolution`) decodes all 601 frames clean, which
  is why the macOS app uses the hardware decoder and why a portable client cannot simply swap in
  ffmpeg. Turning RVRA off in the offer was tried and the device ignores it (see the RVRA doc).

The measurements below stand as facts about conformance and speed; they are necessary but not
sufficient for a working Linux client.

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

**The trailer crop is still required** (`[w:u16be][h:u16be][00...][session tag]`, tiers
1184x2576 / 1088x1920 / 720x1280, the only wire signal of a downshift; parser is
HEVCStream.swift:355, ~25 lines) — but it fixes only the geometry of a downshifted frame, not
the pixels, which are wrong for the RVRA reason above.

**The client spike EXISTS: `client-c/rplay-view.c`** (one file, libavcodec + SDL2, `make` in
client-c/). TCP 9877 → Annex-B split → access units on the first-slice flag → keyframe gating →
trailer strip → single-thread decode → SDL source-crop to the active rect → letterboxed scale.
Codec is asked over 9876 (`stream_info`), overridable with `--codec`. `--check` decodes headless
and prints stats; `-f capture.h265 -r fps` plays a file. Its plumbing is verified offline
(apple_video_REFERENCE.h265 → 601/601 access units decoded, 601/601 trailers, 0 decoder errors;
gop-reproducer.h265 → 117/117) — **but it inherits the RVRA limitation: pictures are garbled
from the first downshift until content refresh papers over it.** It is the right harness for
testing the portable options below, not a shippable viewer.

**The portable options** (detailed in `doc/RVRA-AND-PORTABILITY.md`): turning RVRA off in the
offer is ruled out (device ignores the token); hiding frames while below full tier is the leading
candidate but is only viable if our sessions stop downshifting 6-12x more often than Device
Hub's (why they do is unexplained and is the highest-value open question — the built-but-never-run
`sudo ./scripts/rvra-bitrate-all.sh` experiment probes the bitrate hypothesis); implementing
RVRA reference resampling on top of ffmpeg is correct-but-research; a two-tier product
(VideoToolbox on macOS, degraded elsewhere) is the fallback. Note that hiding below-full-tier
frames is not enough by itself: after the tier returns to full, the decode is still damaged until
enough content refreshes, so the hold must extend until the picture is provably clean (e.g.,
compare against a slow full re-anchor, or request a keyframe via PLI on tier-up — the engine
already knows how to send PLI).

## Suggested next steps, in order

1. Answer "why do our sessions downshift so much more than Apple's?" — run
   `sudo ./scripts/rvra-bitrate-all.sh` with the phone attached (needs the user for sudo). If
   the downshift rate can be brought to Device Hub's ~5%, the hide-during-downshift strategy
   becomes a product and the Linux client is unblocked without touching RVRA.
2. Live-verify the engine's userspace video path (non-root, `RPLAY_USERSPACE_NET=1`), phone
   attached; rplay-view is a fine sink for the test as long as garbled-under-motion is expected.
3. Configure a git remote and push the branch (nothing pushed anywhere).
4. Engine: the syslog select() pump for userspace mode; client: PLI-on-tier-up + hold-last-clean
   experiment in rplay-view.

## Kickoff text for a fresh session

Paste this to start the next session:

> Continue rplay-hub on branch `rendering-resolution-switch` (no git remote is configured, so
> nothing is pushed anywhere). Read `doc/LINUX-PORT-HANDOFF.md` first, then
> `doc/RVRA-AND-PORTABILITY.md` — RVRA is the single blocker for portable video. Summary: the C
> engine is fully portable and non-root via lwIP userspace TCP/IP (`RPLAY_USERSPACE_NET=1`), now
> including the live-video path (media.c rides lwIP UDP — code-complete, builds clean, NOT yet
> live-verified); a client harness exists at `client-c/rplay-view.c` (ffmpeg + SDL2; plumbing
> verified against the reference captures, but software decode is garbled from the first RVRA
> downshift on — only VideoToolbox with the private RVRA properties decodes this stream clean,
> so the harness is for experiments, not shipping). Remaining: (1) the RVRA downshift-rate
> question (`sudo ./scripts/rvra-bitrate-all.sh`, needs the user), (2) live-verify the userspace
> video path, (3) set a remote and push, (4) syslog pump; client hold-last-clean + PLI
> experiment. The daemon needs sudo to (re)start — ask me rather than trying; the
> userspace-mode engine needs no sudo. Task for this session: <fill in>.
