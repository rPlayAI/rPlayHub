# Rendering artifacts — handoff

Status as of 2026-07-28. Written to start a fresh session without repeating a day of work.

## The symptom

The mirrored screen is clean when still. A swipe produces artifacts ("snow"), worse the faster the
swipe. With a keyframe request every 3 s the picture repairs itself within a few seconds; at 15 s
it stays broken five times longer, which is how recovery time was confirmed to track the keyframe
interval rather than anything else.

Device Hub, on the same phone, is reported to show no such artifacts.

## What has been eliminated, by measurement

Every item below was measured, not reasoned about. Numbers are from a single live session of
67,591 packets that included deliberate fast swipes.

| eliminated | evidence |
|---|---|
| packet loss | `0 lost` |
| out-of-order delivery | `0 late`, `0 dup` — the tunnel does not reorder at all |
| length/framing errors | `0 malformed` (declared NAL lengths checked against bytes present) |
| our depacketizer | a second, independent implementation agrees exactly (see below) |
| decode | `0 failed`, decoded frames == acknowledged frames, 1:1 |
| display | `0 not shown` — no picture skipped between decode and screen |
| the loopback proxy | removed; app takes RTP directly, daemon reports `rtp_packets: 0` |
| encoder starvation | **we receive MORE than Device Hub does** — see below |
| receive buffer overflow | `SO_RCVBUF` read back; 4 MB requested and granted in full |

### Per-frame sizes — the measurement that killed the last theory

|  | mean | peak |
|---|---|---|
| Device Hub (its own capture) | 6,947 B | 119,368 B |
| ours, live | 8,949–10,512 B | 138,572 B |

An earlier claim that we were being starved ("0.0117 vs 0.0185 bits/pixel") was **wrong**. It came
from dividing total bytes by an *assumed* frame rate. Counting frames directly reverses it.

### The independent depacketizer

`core/rp_rtp_assembler.{h,c}` is a C port of the Miracast receiver
(`reference/DisplayNoteMiracastReceiver`, itself stagefright, Apache 2.0), extended for HEVC —
vendored the way Cafari vendored `ts_packetizer.cc`. Adopting that lineage fixed audio glitches in
Cafari that had survived every local fix, so it was a fair thing to try here.

It differs from `core/rp_rtp.c` in three ways: a gap is waited out in TIME (100 ms) rather than
until the reorder buffer fills; a declared loss raises a discontinuity so fragments never span a
gap; an access unit ends on the marker bit. **It produced the same rendering.** Its `late` counter
also read 0, proving the tunnel never reorders — so the old buffer-fill behaviour had never been
discarding anything either.

Select with `RPLAY_ASSEMBLER=miracast`. Legacy remains the default.

## The one unexplained fact

**Device Hub's own captured stream decodes with visible artifacts at frames 60 and 180, while its
window looked clean to the user at the time.**

Both cannot be true of the same bytes unless something differs in how they are decoded or
presented. This was never resolved and is now the most important open thread, because everything
between the wire and our screen has been accounted for.

Note the comparison has never been simultaneous — Device Hub and rPlayHub were judged in separate
sessions, by eye, at different times. A side-by-side screen recording during identical motion would
settle whether the two really differ, and that has not been done.

## Next step: capture from avconference

Every measurement so far is our pipeline measuring itself. What has never been observed is what
Apple's own stack does with the same bytes. In order of what it would settle:

1. **What avconference feeds its decoder.** If those bytes differ from ours, the difference is
   upstream of decode and can be diffed directly.
2. **What its decoder emits.** If the bytes match ours but its pictures are clean, the fault is in
   our VideoToolbox usage — a narrow search.
3. ~~**Its RTCP feedback, RCTL included.**~~ **Done — see `doc/DEVICEHUB-CAPTURE-FINDINGS.md`,
   "RCTL, decoded".** The capture had not been lost: it is `logs/devicehub.pcap`, the same one the
   rest of that document came from. RCTL is fully decoded except one field, and the layout is in the
   doc. No device was needed.

## Ruled out since: long-term references

**LTR is not used by this encoder at all**, in Apple's stream or ours. `long_term_ref_pics_present_flag`
is 0 in every SPS, and zero of 1,766 coded pictures across four recordings reference a long-term
picture. Since that is an SPS flag, it holds for whole sessions, not just the sampled frames. Every
picture predicts from **−1 and −2 only**, and POC is contiguous in all four — no picture is missing
from what reaches the decoder.

So LTR-ACK is not the recovery path, and acknowledging a frame we do not hold cannot be causing the
artifacts, because nothing predicts from a long-term reference. `doc/DEVICEHUB-CAPTURE-FINDINGS.md`
carries the full correction; the belief it replaces had driven real design.

What the structure does explain is the *persistence*: with references at −1/−2 and one IDR per
session, a single bad picture propagates until the next keyframe. That is why recovery time tracks
the keyframe interval — a property of the prediction structure, not of LTR.

**The open question is now narrower: what makes a picture bad in the first place, given the
elementary stream is complete, contiguous and byte-correct?** The leading hypothesis — and it is a
hypothesis, not a measurement — is that the encoder's rate control is running open-loop because we
send no RCTL while `RateAdaptationEnabled=True`. It fits every observation, including the one that
killed encoder starvation: an open-loop encoder overshooting explains *more* bits arriving and a
worse picture at the same time. Testing it means sending RCTL, which the decode above now makes
possible.

## Traps that already cost time

- **`pkill` skips `stopmediastream`.** The device keeps the session and the next launch cannot
  acquire it, which looks like a hang. Worth a signal handler.
- **`RPLAY_RTP_FORWARD` perturbs what it measures.** Mirroring RTP to loopback took a stream
  holding 0.00% loss to 7.9–10.6% loss: one extra syscall per packet on the receive thread is
  enough to miss the next datagram. Never compare a mirrored run against an unmirrored one.
- **Payload type is not the codec.** A live HEVC session arrives on payload type 100 — the number
  our own offer uses for its H.264 bank. The device assigns numbers in its answer. The payload
  *structure* decides (HEVC 48/49, H.264 24/28), which is what the depacketizers key on. An hour
  was lost to concluding "the device chose H.264" from the number alone.
- **`RPLAY_CODEC=hevc` is worse.** Offering a single bank produced 2.33% loss and zero decoded
  frames. Leave it on AUTO.
- **Change one variable per run.** The assembler and the keyframe interval were changed together;
  the result ("worse") was unattributable until they were separated, and the cause turned out to
  be the keyframe interval alone.

## Environment variables

```sh
RPLAYHUB_VIDEO=direct|proxy|auto   # how the player gets video (default auto)
RPLAY_NO_VIDEO_PROXY=1             # daemon: do not host the Annex-B fan-out
RPLAY_ASSEMBLER=miracast           # use the vendored port instead of rp_rtp.c
RPLAY_KEYFRAME_EVERY_S=3           # keyframe request cadence; 15 matches Device Hub
RPLAY_CODEC=hevc|h264              # force one codec bank (both are worse than AUTO)
RPLAY_RTP_FORWARD=5004             # mirror RTP to loopback — PERTURBS, see above
```

`scripts/independent-receiver.sh [port] [payload-type] [H265|H264]` decodes the mirror with ffmpeg.

## Fixes made today that are worth keeping regardless

Real defects, none of which turned out to be the cause:

- RTP **padding was never stripped** — padding bytes were copied into NALs as video. Absent from
  both the C and the Python, which is why comparing them agreed: they shared the omission.
- **Fragment continuity** was unchecked; a continuation could be welded onto a different NAL.
- A fragmented NAL too large to reassemble **emitted its truncated self** as if whole.
- The **RTP marker bit was ignored**; frame boundaries were inferred from the next picture's first
  slice, holding every frame back by one, with a 20 ms idle timer as backstop.
- **`late` was never reported**, so "0 lost" was read as "nothing discarded" when the two differ.
- The proxy was removed from the video path entirely.
- Decode/present now follows rplay's `hwdecoder.m`: no destination attributes (native format),
  `DisplayImmediately`, drop rather than queue when the layer is not ready.

## The receive path is exonerated, and the artefacts are rate-bound

Established by replaying `reference/captures/devicehub-iphone13-ios27.pcap` offline, which is
repeatable in a way no live run was (`tools/pcapreplay/`, `scripts/replay-rtp-to-ffmpeg.sh`):

| check | result |
|---|---|
| capture completeness | 3849 of 3849 video packets, **0 sequence gaps, 0 duplicates** |
| our shipped C depacketizer (`rp_rtp.c`) | 4,182,507 B |
| our vendored Miracast assembler | identical md5 |
| a Python depacketizer written from RFC 7798 | identical md5 |
| **ffmpeg's own RTP/HEVC depacketizer** | **all 603 common NALs byte-identical** |
| byte accounting | 4,178,937 NAL content + 1,154 reconstructed FU headers = 4,180,091 emitted, **exact** |
| ffmpeg decode | **zero errors**, 601 frames |
| VideoToolbox decode | 600 pictures, **zero failures**, same pictures as ffmpeg |
| the IDR (119,364 B over ~110 fragments) | **decodes pixel-perfect** |
| frames 1-30 | **decode pixel-perfect** |

Four depacketizers, one of them written by people with no connection to this project, agree byte for
byte. Depacketization, decode and display are done as suspects and should not be reopened.

**What settles the diagnosis is a user observation, not a counter: a slow swipe never artefacts, a
fast one always does.** That is a clean discriminator. Packet loss, a depacketizer defect or a
reference error does not care how fast a finger moves; an encoder that cannot afford the bits does.

The arithmetic agrees. The device negotiates a 6 Mbps ceiling at 1184x2576, which at ~40 fps is
about 18 kB per frame for 3.05 megapixels -- roughly 0.05 bits/pixel if every pixel changed. During
the captured swipe the encoder ran at 49-88% of that ceiling and its largest P-frame ever was
65,524 B. It is pushing, and it is still far short of what a full-screen swipe needs, so it updates
the picture in pieces. **The mosaic is the encoder's own reconstruction, faithfully decoded** -- not
corruption. It heals once motion stops.

Checked and rejected along the way: a 64 kB frame ceiling. Only one P-frame lands near 65535 (next
largest 61,726) and the IDR is 119,364 B, so nothing is being clipped at a u16 boundary.

### What remains unexplained

Device Hub displayed *this* capture's session cleanly -- watched closely, and repeatedly since. Its
own packets decode to the mosaic above. Both cannot be true of the same bytes, and the capture is
provably complete, so **Device Hub was probably not displaying this RTP stream**, or was not the
only client when it was captured.

`scripts/capture-and-compare.sh` settles it: it records the packets and the Device Hub window
*simultaneously*, refuses to run with rPlayHub up (a second viewer splits the encoder budget), and
checks tcpdump's kernel drop count. Comparing the same instant in `decoded/` and `window/` needs no
memory and no eye.

## The operating point, not the bandwidth (2026-07-31)

A fresh Device Hub capture over **WiFi**, with rPlayHub quit so the device had one client, is
**clean** — including mid-swipe at its largest frames. The old capture, same phone, same app, is
garbled. Both are genuinely Device Hub: both carry RCTL and neither carries PLI or FIR, which is
Device Hub's RTCP signature and not ours.

| | old capture (garbled) | new capture, WiFi (clean) |
|---|---|---|
| total bitrate | **2.30 Mbit/s** | **1.47 Mbit/s** |
| frame rate | 41.3 fps | 18.2 fps |
| bytes per frame | 6,768 | **9,972** |
| P-frame p95 | 27,418 | **44,590** |
| IDR | 119,364 B | 57,224 B |

**The clean session used 36% less bandwidth.** It spent it on half as many, complete frames rather
than twice as many starved ones. So this was never a bandwidth shortage — it is the encoder's
frame-rate-versus-quality operating point. At ~41 fps it cannot finish a 1184x2576 picture in its
per-frame budget and emits partial updates, which is the mosaic; at ~18 fps each picture completes.

This also disposes of the "we receive MORE than Device Hub" measurement that killed the starvation
theory: receiving more bits spread across too many frames is *worse*, not better. The metric that
matters is bytes per frame, not bits per second.

**Confounded, and worth one more run.** The new capture changed transport (WiFi) *and* guaranteed a
single client at the same time — two variables, which is the trap this document already warns
about. The single-variable rerun is: `sudo ./scripts/capture-swipe-rates.sh` over **USB**, rPlayHub
quit. If USB reproduces ~41 fps with thin frames while WiFi gives ~18 fps with fat ones, the
transport selects the operating point, and the levers to reach for are the offer's
`AVCMediaStreamNegotiatorAccessNetworkType`, the bitrate tiers, and RCTL's target-bitrate field
(`+30`, in units of 100 bps — see DEVICEHUB-CAPTURE-FINDINGS).

`scripts/capture-swipe-rates.sh` drives slow and fast swipes synthetically over the identical pixel
path so only speed differs, and segments the capture by phase.

## RETRACTED (2026-07-31): there was no decode regression

An earlier version of this section claimed the fault had been found: that `decodecheck` was built
before commit 45d6ec9 changed the decoder's pixel format, and that the current 420f path garbled
what the old BGRA path decoded cleanly.

**That was wrong, and the mistake is worth recording because it is the same one this document warns
about elsewhere.** The comparison used *different frames*. The old build wrote `frame-0150`, a
static moment, and the new one wrote `live-00197`, taken mid-swipe. Frame 150 is clean in every
build; frame 197 is garbled in every build.

Bisected properly, with the pixel format forced to BGRA so the same frame renders in all of them:

| build | frame 150 |
|---|---|
| pre-4c76549 | 2,601,723 B |
| 4c76549 | 2,601,723 B |
| 45d6ec9 + BGRA | 2,601,723 B |
| c938696 + BGRA | 2,601,723 B |
| HEAD + BGRA | 2,601,723 B |

Byte-identical throughout. And frame 197 from the known-clean pre-4c76549 build is garbled too.
The decode path has not changed behaviour at all.

The pixel-format change (BGRA to native 420f) is real but inert: it only decided which frames the
PNG writers could render, and a writer that silently fails on 420f is what let the frame mismatch go
unnoticed. `decodecheck`'s own writer still cannot render 420f — use `RPLAYHUB_LIVE_FRAMES`, which
goes through CoreImage and handles any format.

## Where this actually leaves the investigation

Unchanged, and now confirmed on our own stream rather than only Device Hub's:

* the capture is complete (0 lost, 0 gaps, the device's own per-frame packet count agrees);
* four depacketizers agree byte for byte, ffmpeg's among them;
* every decoder and every build produces the same pictures from those bytes, with zero errors;
* those pictures are garbled during fast motion and clean when still.

So the garbling is in the encoded content, for both apps. Our stream is not the poorer of the two:
P median 9,160 B against Device Hub's 4,432 B, 6% of frames under 2 kB against their 39%.

The one measured difference in what the two receivers *send* remains: Device Hub sends RCTL ~20/s
and never a PLI; we send PLI and no RCTL. RCTL is decoded (see DEVICEHUB-CAPTURE-FINDINGS) and has
never been implemented or tested.

## Superseded — see the retraction above

### (original claim, kept for the record)
FOUND (2026-07-31): the harness was stale, and the fault is in the 420f decode path

**It reproduces offline, from a file, with no device.**

```
./build/decodecheck-new build/validate-ours-20260731-182204/video-trimmed.h265
```

`/tmp/offline/live-00197.png` comes out catastrophically garbled, every time. No phone, no Wi-Fi,
no swipe, no display layer.

### Why this took four days

| harness | pixel format | verdict it gave |
|---|---|---|
| `build/decodecheck`, built **Jul 28 12:58** | **BGRA**, `planar=false` | "197 pixel-perfect pictures, parse/assemble/decode all clean" |
| same sources rebuilt **Jul 31** | **420f**, `planar=true` | **garbage** |

`app/rPlayHub/VideoDecoder.swift` was changed on Jul 31 (commit 45d6ec9, "Adopt rplay's decode and
presentation path") to stop asking for BGRA and take the decoder's native output instead. The
harness binary was never rebuilt. **Every "decode is innocent" verdict in this document came from a
binary testing a configuration the app no longer uses.**

That is why the bug looked unfalsifiable: ffmpeg decoded the bytes cleanly, the harness decoded them
cleanly, and every counter read zero — because the bitstream was never the problem. It arrives
complete and correct; we mishandle the decoder's `420f` output.

### What this also retires

- The stream is fine. Ours is *richer* than Device Hub's: P median 9,160 B vs 4,432 B, 6% of frames
  under 2 kB vs their 39%, both decoding with zero ffmpeg errors.
- Presentation is not the cause. The live dump (`RPLAYHUB_LIVE_FRAMES=<dir>`) writes pictures
  straight from the decoder's `CVPixelBuffer`, before the display layer sees them, and they are
  already garbled (`/tmp/live/live-00138.png`).
- `29,609 of 53,664 not shown` is real and worth fixing, but it is a consequence, not the cause.

### Next step

Bisect the Jul 31 commit offline, one variable at a time:
1. put `kCVPixelBufferPixelFormatTypeKey = BGRA` back in `imageBufferAttributes` and re-run the same
   file — if it goes clean, it is isolated to that line;
2. otherwise the decoder specification changed in the same commit ("Require, not merely enable"
   hardware acceleration), which selects a different decoder.

New tools: `build/decodecheck-new` (rebuilt from current sources — **rebuild it before trusting
it**), and `RPLAYHUB_LIVE_FRAMES` / `RPLAYHUB_LIVE_STRIDE` / `RPLAYHUB_LIVE_LIMIT` in the app, which
dump live decoder output through CoreImage so any pixel format renders.

**Lesson:** `decodecheck` existed to be the trustworthy oracle, and it silently became the least
trustworthy thing in the investigation the moment the code it tests changed underneath it. It should
be rebuilt by its own script every run, never used from a previous build.

## A deterministic reproducer, at last (2026-07-31)

`reference/captures/gop-reproducer.h265` — 1.05 MB, 117 pictures, one GOP taken from a live
rPlayHub session. **No device, no network, no app, no display layer.**

```
ffmpeg -i reference/captures/gop-reproducer.h265 out-%03d.png
   frame   1  (the IDR)   pixel-perfect
   frame 108              catastrophically garbled
```

The IDR is intra-coded and, extracted and decoded *on its own*, is flawless. Decoded as the head of
its own GOP, the picture degrades progressively to garbage by frame ~108. ffmpeg and VideoToolbox do
this identically. Four days of needing the phone to see this bug — that requirement is gone.

### The code, read line by line, is not the cause

| checked | result |
|---|---|
| `rtp_payload()` — CSRC list, extension header, padding | correct; padding properly excluded |
| HEVC FU reassembly — header reconstruction `(p[0]&0x81) \| (type<<1)`, continuity, overflow | correct |
| HEVC AP parsing — 2-byte PayloadHdr skip, per-NAL sizes | correct |
| `drain()` / reorder window | correct; and arrival order == sequence order here, proven by the Python depacketizer (which sorts) matching the C (which does not) byte for byte |
| ffmpeg's own depacketizer over the same packets | **byte-identical NALs** |
| slice structure | 117 pictures, one slice each, `first_slice_segment_in_pic_flag=1` throughout, no dependent slices |
| parameter sets | all 7 copies byte-identical; POC contiguous; every reference resolves |

So the NALs are right, their order is right, the IDR is perfect — and the sequence still degrades.

### Real defects found while looking (all latent, none the cause)

* **`media.c`** reads the RTP marker bit from the *arriving* packet but signals end-of-frame against
  whatever `rp_rtp_feed` just *drained*. With a reorder queue those differ. This is the exact case
  `ra_frame_cb`'s comment calls "precisely the case the old path got wrong" — and the old path is
  the default. Inert here only because this tunnel never reorders.
* **`HEVCStream.rebuildFormatIfPossible()`** begins `guard format == nil else { return }`, so the
  format description is built once and never rebuilt. New parameter sets with each IDR are stored
  and ignored. Inert only because all 7 copies are byte-identical.
* **No discontinuity signal** reached the decoder on declared loss — now fixed (`on_discontinuity`,
  `signalDiscontinuity()`, immediate PLI).

### Where to go next

1. **Iterate on the reproducer.** Bisect *which picture* first diverges by decoding N frames for
   increasing N; the first frame that differs from its standalone decode localises the fault to one
   access unit.
2. **Check CTU coverage per slice.** If a slice codes fewer CTUs than the picture contains, the
   encoder is sending partial-picture updates and the stale regions are expected — which would make
   this a compositing question, not a corruption one.
3. **`scripts/trace-avconference-decode.d`** — what avconference actually feeds its decoder. Still
   the only never-observed stage.

### Depacketizer and LTR-ACK, verified on OUR capture (not just Apple's)

A gap worth naming: the ffmpeg-depacketizer comparison had only ever been run against Device Hub's
capture. `validate-capture.sh`'s "independent depacketizer" is `scripts/rtp-depacketize.py`, written
in this project after reading `rp_rtp.h` — not independent enough to settle anything.

Re-run properly, `scripts/replay-rtp-to-ffmpeg.sh` over `build/ours-20260731-220543.pcap`, aligned at
the first parameter set (ffmpeg discards the leading slices it has no PPS for):

    594 common NALs, 0 differing -- BYTE-IDENTICAL to ffmpeg's own RTP stack

And the reassembled sequence, checked against the device's own frame index from the RTP header
extension:

| check | result |
|---|---|
| frame index monotonic across packets in sequence order | YES |
| exactly one RTP timestamp per frame | YES |
| timestamps strictly ascending with frame index | YES |
| LTR-ACKs naming a real frame timestamp | **594/594** |
| duplicate LTR-ACKs | 0 |
| frames sent 595 / acked 594 / unacked 1 | correct (the last frame was incomplete at capture end) |

Our acknowledgements are as correct as Apple's, which the capture showed at 594/594 as well. Neither
the depacketizer nor the acknowledgement path is the fault, on our own stream, by a check that does
not depend on any code written here.

# ROOT CAUSE FOUND (2026-08-01): the encoder drops resolution and says so out of band

**The device codes a downscaled screen into the top-left sub-rectangle of the 1184x2576 frame, and
signals the active rectangle per frame outside the bitstream. We decode every frame as if it were
full size.**

Captured from `avconferenced` (the real receiver -- Device Hub only configures the session), the
per-frame dictionary passed to `VTDecompressionSessionDecodeFrameWithOptions`:

```
ActiveVideoResolution { Width = 1184; Height = 2576 }   625 frames
ActiveVideoResolution { Width = 1088; Height = 1920 }    53 frames
ActiveVideoResolution { Width =  720; Height = 1280 }    30 frames
ContentAnalyzerCropRectangle { X = 0; Y = 0; ... }
```

Twenty-three switches in 682 frames. The SPS says 1184x2576 for the whole session and never
changes; full- and reduced-resolution frames have structurally identical slice headers. Decoding a
720x1280 frame and cropping to its top-left rectangle shows a correctly scaled home screen.

## Why this defeated every measurement

Nothing was wrong with the receive path, and every check that said so was right:

* the capture is complete -- the device's own per-frame packet count agrees;
* four depacketizers produce byte-identical NALs, ffmpeg's among them;
* ffmpeg, VideoToolbox, every build and every pixel format reconstruct **the same** pictures --
  because none of them is told the resolution changed. The information is not in the bitstream;
* the IDR is pixel-perfect standalone -- it is coded at full resolution;
* every counter reads zero -- nothing is lost or malformed. We decode at the wrong size;
* Apple's own decoder input "decodes to garbage" when replayed as a plain elementary stream --
  that discards the per-frame options that make it decodable;
* artifacts track swipe speed because the resolution drops under motion, which is also why a slow
  swipe is clean and why AirPlay, at 5-10x the bitrate, never does this.

## What it cost, and the lesson

Four days, and five wrong conclusions in the final session alone -- display layer, a "stale harness
regression" (mismatched frame numbers), "the device is responding" (RCTL was never on the wire),
LTR, and decoder configuration. Every one came from interpreting a number before checking what
produced it.

The two things that actually moved it were both from asking a better question rather than running a
better test: "who actually reads the socket" (it was `avconferenced`, never Device Hub -- three
instrumentation attempts had targeted the wrong process), and "read every line of the code" (which
found the hook was watching `VTDecompressionSessionDecodeFrame` while Apple calls
`...DecodeFrameWithOptions`).

## The remaining work

The active resolution must be recovered from the wire, since it is not in the bitstream. The
leading candidate is the RTP header extension **profile** field, which takes exactly three values
(0x9011, 0x9211, 0x9001) against exactly three resolutions.

    sudo ./scripts/capture-paired.sh 25          # wire + Apple's ground truth, same session
    python3 scripts/correlate-resolution.py build/paired-<stamp>

The correlator aligns the two by payload size and reports whether the profile determines the
resolution. Once the carrier is proven, the fix is: read it in `core/rp_rtp.c`, plumb the active
rectangle alongside the NALs, and crop the decoded picture to it before display -- which is exactly
what `ContentAnalyzerCropRectangle { X = 0, Y = 0 }` says Apple does.
