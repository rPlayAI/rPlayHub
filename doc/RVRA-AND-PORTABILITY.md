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

**2. Hide frames while below full resolution — now the leading candidate**, since option 1 is
ruled out. Portable, and the detection already exists — the
trailer gives the tier per frame, before decode. Hold the last good picture during a downshift and
resume at full size. Evidence it recovers: in a non-RVRA decode, frames go clean again once the
encoder returns to full tier. Cost is a brief freeze during fast swipes rather than garbage.

**3. Implement RVRA reference resampling** in a custom decoder. Correct, and a research project.

**4. Two-tier product** — VideoToolbox on macOS, option 2 elsewhere.

## A method note

`strings` returns **nothing** for `reference/binaries/AVConference` — zero lines, including symbols
`KICKOFF.md` quotes from that very file. An earlier search here therefore "established" that RVRA
was absent from AVConference, which was a false negative that nearly closed off this whole line of
inquiry. The strings live in `__cstring` and need `otool -s __TEXT __cstring` to reach.

A tool reporting no matches is not evidence until it has been shown to report *something*.
