# RVRA — why this stream only decodes on Apple hardware, and what to do about it

Written 2026-08-02. This is the single biggest obstacle to running rPlayHub on Linux or Windows,
and it is not a porting problem. Read this before planning the C port's video path.

## The problem in one comparison

The same 601 frames, from the same capture, decoded two ways:

| decoder | result |
|---|---|
| ffmpeg (`-f hevc`) | **garbage from the first resolution downshift onward**, and **zero warnings, exit 0** |
| VideoToolbox with RVRA enabled | clean, all 601 frames |

ffmpeg is not failing to parse. It parses fine and predicts wrongly, which is why nothing complains.
Roughly 5% of frames in that capture sit below full resolution (29 of 601), but they cluster
exactly during swipes — the moments you are looking at the screen.

## What RVRA is

From `AppleVideoDecoder`'s own error strings (`/System/Library/Video/Plug-Ins/
AppleVideoDecoder.bundle`):

```
AppleAVD::RVRAScaler returned error:%d
checkRVRAScalingRatio returned error:%d
Rejecting RVRA scaling ratios beyond 16x! inWidth:%d inHeight:%d outWidth:%d outHeight:%d
RVRA_FIRST_BACKUP_BUFF_INDEX / RVRA_SECOND_BACKUP_BUFF_INDEX
allocRVRAMemory / CreateUncompressedPixelBufferAttributesDictionaryRVRA()
kAppleAVDEnableRVRA / kVASetEnableRVRA
RVRAInLoopChromaFilter()
```

A hardware **scaler with backup buffers**, taking in/out dimensions and refusing ratios beyond 16×.
That is reference resampling: when the encoder changes active resolution mid-sequence, references
coded at the old size are rescaled before prediction. Standard HEVC has no such mechanism — VVC's
RPR is the nearest published analogue — so a conforming decoder predicts without rescaling and
smears.

**The acronym is never spelled out** in any binary examined. `VideoResolutionAdaptationType` is the
VideoToolbox key and the offer carries `VRA`/`MVRA`/`RVRA1` together, so "Video Resolution
Adaptation" is almost certainly the core of it. The leading `R` is a guess and is left as one.

## How the device is told to do it

RVRA is an **encoder capability negotiated in our offer**, not a decoder preference. Apple's full
feature-list string, recovered from `AVConference`'s `__cstring` section:

```
FLS;VRA:0;MVRA:0;RVRA1:1;AS:2;MS:-1;LTR;CABAC;CR:3;LF:-1;PR;CH1:4;CH:4;FA:5;
AR:667/375,375/667;XR:3/2,2/3;
```

next to `_VideoTransmitter_h264HwEncoderSupportsRVRA1`. Three related tokens: `VRA:0`, `MVRA:0`,
and `RVRA1:1` — the last explicitly **on**.

Our HEVC bank sends `FLS;SW:1;` (`core/rp_media_offer.c`), which carries no RVRA token at all, so
the device applies its default. Observed behaviour says that default is on.

Apple's other strings, for reference:

```
FLS;SW:1;          the HEVC bank -- what we copied
FLS;VRAE:0;SW:1;   the AVC bank -- note VRAE, a different token, explicitly 0
```

## What the decoder side needs (if RVRA stays on)

Three things must all be true, and missing any one produces the mosaic:

1. **Strip the per-frame trailer** from the last slice NAL (see `doc/RENDERING-HANDOFF.md`).
2. **Pass the size per frame** as `ActiveVideoResolution` in `frameOptions`.
3. **Enable RVRA on the session** —
   `VTDecompressionSessionSetProperty(s, "VideoResolutionAdaptationType", 3)`, *plus*
   `NegotiationDetails` and `DecoderUsage`.

Measured, so it is not re-litigated:

- `ActiveVideoResolution` on an ordinary session changes **0 of 401** frames. It reads as "the key
  is inert" and means "RVRA is off, so the key is discarded".
- `VideoResolutionAdaptationType = 3` **alone is not enough**. Dropping `NegotiationDetails` and
  `DecoderUsage` while keeping it made **355 of 399** frames differ, and the output was garbled —
  even though the property returned `noErr`. *(The test disabled both together, so which of the two
  matters is not established.)*
- We send `NegotiationDetails = "RVRA1:0;SW:1;FLS"`. Apple's own captured session sends
  `"VRAE:0;SW:1;FLS"`. Both work; why is not established.

## The portable options

**1. Turn RVRA off at the source — TRIED, DOES NOT WORK.**

Tested 2026-08-02 against an iPhone 13 Pro on iOS 27. The app builds the offer (it links
`rp_media_offer`), so this is set on the app rather than the daemon:

```sh
RPLAY_HEVC_FEATURES="FLS;VRA:0;MVRA:0;RVRA1:0;SW:1;" \
  build/dd/Build/Products/Debug/rPlayHub.app/Contents/MacOS/rPlayHub
```

The offer was confirmed to carry the string — `rp_media_offer.c` prints `[offer] HEVC features:`
once at negotiation, precisely so a negative result cannot be confused with the override silently
not applying. It did apply, and the encoder adapted anyway. Under swipes:

```
22:22:21  1088x1920 -> 1184x2576 -> 1088x1920 -> 720x1280 -> 1088x1920 -> 720x1280
22:22:22  1088x1920 -> 720x1280  -> 1088x1920 -> 1184x2576
```

Ten changes in two seconds, indistinguishable from the baseline. **The device ignores the token.**

Note also that our default offer, `FLS;SW:1;`, carries no RVRA token at all and behaves the same,
so "absent" and "explicitly 0" both produce adaptation. Whatever drives it is not this string.

Not yet tried: whether adaptation is bitrate-driven instead. The encoder adapted while sending
1.7 Mbit/s against a negotiated 6 Mbps ceiling, so it is not simply running out of headroom, but
RCTL's target is a separate lever from the feature list and has not been varied on its own.

**The experiment is built and has not been run** (2026-08-02). One command, with the phone unlocked
on the home screen:

```sh
sudo ./scripts/rvra-bitrate-all.sh
```

It restarts the daemon five times — baseline, `--rctl 0`, `--rctl 800000`, `--rctl 20000000`, and a
raised `streamConfig` ceiling — driving identical synthetic swipes through the control API each
time, and prints a table of downshift rate per condition. It did not run here because the iOS 27
phone dropped off wifi mid-session and restarting `cdhost` needs a password.

Two things about the design are worth keeping even if the result is negative:

- **`--rctl 800000` is a positive control, and it must be read before anything else.** Starving the
  target should make downshifting *worse*. If it does not, the device is not acting on RCTL at all,
  and "we asked for more bits and nothing changed" is not evidence that bitrate is irrelevant — it
  is evidence the knob never reached the encoder. Without that row the whole experiment repeats the
  `RVRA1:0` mistake in a new variable. `ceiling-high` travels in `streamConfig` rather than RTCP and
  is the independent second attempt for exactly that case.
- **The motion is synthetic.** Adaptation is provoked by movement, so a hand swipe that is a little
  faster on the second run produces more downshifts for reasons unrelated to the variable under
  test. The runs also include a still phase, because "no downshifts" means nothing without knowing
  whether the phone was moving at the time.

The measurement reads the per-frame trailer off the daemon's Annex-B broadcast on `:9877` — no
decoder, no viewer app, since a decoder that keeps up would be a confounder. The parser is the same
one as `HEVCStream.parseActiveRectTrailer`, and it can be checked against a recorded capture before
any null result is believed:

```sh
python3 scripts/rvra-bitrate.py --self-test build/devicehub-recording.h265
#   604 NALs, 601 carry a trailer, 6 tier changes, {'1184x2576': 572, '1088x1920': 20, '720x1280': 9}
```

That 601 independently reproduces the 601/601 figure this document already quotes.

**2. Hide frames while below full resolution — the leading candidate**, since option 1 is ruled
out. Portable, and the detection already exists — the trailer gives the tier per frame, before
decode. Hold the last good picture during a downshift and resume at full size. Evidence it
recovers: in a non-RVRA decode, frames go clean again once the encoder returns to full tier. Cost
is a freeze during fast swipes rather than garbage.

**Whether that cost is acceptable depends entirely on how often the encoder downshifts, and our
sessions downshift far more than Apple's.** Measured over three captures with
`scripts/rvra-bitrate.py --self-test`:

| capture | kB/frame | below full tier | episodes | longest |
|---|---|---|---|---|
| Device Hub's own session | 7.0 | **4.8%** | 2 | 16 frames |
| ours (`screen3.h265`) | 13.8 | **29.6%** | 6 | 25 frames |
| ours (`ours-ffmpeg.h265`) | 16.2 | **58.3%** | 19 | 40 frames |

At Apple's rate option 2 is a product: two brief holds in twenty seconds. At ours it is a picture
that is frozen more than half the time, which is not one. So "fall back to option 2" is not the
safe default it looks like — it is contingent on closing that gap.

**Read carefully, because this is not a controlled comparison.** These are different sessions over
different content, and motion provokes adaptation, so heavier swiping during our captures would
raise the bit rate *and* the downshift rate together and explain both columns at once. That
confounder is exactly why the instrument drives synthetic swipes.

What it does bear on is option 1's replacement hypothesis. **Our stream already spends 2.3× the
bits per frame that Apple's does and downshifts twelve times as often.** Rate starvation does not
predict that, which makes the bitrate lever a weaker candidate than it looked — worth running
because the run is cheap and the answer is architectural, but worth expecting to fail.

The question these numbers raise is the one nobody has asked yet: **why does our session adapt so
much more than Device Hub's on the same phone?** Whatever the answer, it lives in what we
negotiate, not in what the decoder does afterwards — and unlike the bitrate ceiling, it is a
difference already measured rather than a lever guessed at. If our session could be made to behave
like Apple's, option 2 stops being a compromise.

**3. Implement RVRA reference resampling** in a custom decoder (patch ffmpeg's hevcdec). Correct,
and the ground-truth harness that makes it tractable exists (2026-08-26):

```sh
swiftc -O -o build/groundtruth app/tools/groundtruth/main.swift \
    app/rPlayHub/HEVCStream.swift app/rPlayHub/VideoDecoder.swift app/rPlayHub/AppBuild.swift \
    -framework AVFoundation -framework VideoToolbox -framework CoreMedia \
    -framework QuartzCore -framework CoreVideo
./build/groundtruth reference/captures/apple_video_REFERENCE.h265 gt.y4m gt-index.tsv
ffmpeg -r 60 -i reference/captures/apple_video_REFERENCE.h265 -fps_mode passthrough \
    -strict -1 -f yuv4mpegpipe candidate.y4m
python3 scripts/compare-decodes.py gt.y4m candidate.y4m reference/captures/apple_video_REFERENCE.h265
```

`groundtruth` runs the app's real HEVCStream+VideoDecoder path (RVRA session, per-frame
ActiveVideoResolution) over a capture and dumps every picture as full-range y4m; the comparator
scores any candidate decode per frame with ffmpeg's psnr filter, annotated by RVRA tier. Measured
baseline for stock ffmpeg against the 601-frame reference: **frames 0-44 bit-exact** (hardware and
software agree perfectly until the first downshift — the pipeline itself is proven), then ~10-11 dB
through every downshift episode, partial content-refresh recovery, never exact again. A patch is
converged when every episode row reads `ok`/`bit-exact`. Two empirical handles for fitting Apple's
scaler: skip blocks in post-downshift frames expose the scaler's output verbatim (no residual), and
frames 0-44 being bit-exact means any divergence is attributable to the resampling path alone.

**4. Two-tier product** — VideoToolbox on macOS, option 2 elsewhere.

## A method note

`strings` returns **nothing** for `reference/binaries/AVConference` — zero lines, including symbols
`KICKOFF.md` quotes from that very file. An earlier search here therefore "established" that RVRA
was absent from AVConference, which was a false negative that nearly closed off this whole line of
inquiry. The strings live in `__cstring` and need `otool -s __TEXT __cstring` to reach.

A tool reporting no matches is not evidence until it has been shown to report *something*.

The same shape turned up twice more on 2026-08-02, both times in `build/`, which is untracked
scratch and therefore exactly what someone reaches for by name:

- **`build/apple-notrailer.h265` is byte-identical to `build/devicehub-recording.h265`** — same
  md5, and all 601 trailers present. A file whose name asserts the thing it does not have.
- **`build/ref-stripped.h265` is stripped only at full resolution.** The 29 frames coded *below*
  full tier — 20 at 1088x2576's step down, 9 at 720x1280, the exact frames where adaptation
  happens — are byte-identical to the original, trailer and all. As a control for "does stripping
  change the picture" it excludes every frame where the answer could differ.

Neither is referenced by any script or document here, so nothing published rests on them. Delete
them rather than fixing them; regenerate from a fresh capture if a control is needed.
