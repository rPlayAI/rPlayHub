# Apple Device Hub screen-stream capture — findings (2026-07-28)

Captured a real Xcode **Device Hub → View Screen** session against iPhone 13 Pro / iOS 27 over the
CoreDevice tunnel (`tcpdump -i utun8`, cleartext), and decoded it (RemoteXPC/HTTP2 → XPC → mediaBlob;
RTP/RTCP). This is the ground truth for our hand-built `startmediastream` offer.

## The offer diff — our mediaBlob is byte-for-byte Apple's EXCEPT two fields

Codec banks (HEVC pt123 + AVC pt100, ResEntries, feature strings `FLS;SW:1;` / `FLS;VRAE:0;SW:1;`),
all 10 bitrate tiers, decoder name `Viceroy 1.7.0`, options, `clientSupportedFeatures=140`, `timeout=20`,
mode 5 — **identical**. Only VideoSettings (protobuf f5) differs:

| f5 field | Apple | ours | fix |
|---|---|---|---|
| **f7 `ltrpEnabled`** | **1** | 0 | **set to 1** |
| **f10 `fecEnabled`** | *absent* | 1 | **remove it** |

So: in `screen.py` `_media_blob_video`, emit `f7=1` and drop the `_fv(10,1)`. (Host identity in
`endpointInfo` was `Mac14,13 / 2205.3.5.1 / 25F84` — cosmetic.)

## What Apple actually requests vs the device's answer

- Offer **options** are exactly ours: AccessNetworkType=1, TransportProtocolType=2,
  CoreDeviceVideoDisplayMode=`DisplayByID`, VideoStreamForDisplayID=1, clientSessionID. **Apple does NOT
  put VideoWidth/Height/HDRMode/resolution in the offer** — the *device* chooses them in the answer.
- Device **answer** streamConfig (video): `IsltrpEnabled=True`, `RateAdaptationEnabled=True`,
  CustomWidth×Height=**1184×2544** (macroblock-padded native), VideoResolution=12, **Framerate=60**,
  TX/RXMaxBitrate=6 Mbps, TX/RXMinBitrate=333 kbps, payload **100 (AVC)** with `VRAE:0;SW:1;FLS`,
  KeyFrameInterval=0, RTCPSendInterval=1.0, RTCPTimeoutInterval=20.0.

## RTCP the receiver (Device Hub) sends — NO PLI, NO FIR

Full breakdown over ~20 s: `SR:28  RR:15  SDES:43  APP:884`. **Zero PSFB/RTPFB** (no PLI, no FIR — ever).
The APP packets are two kinds:

1. **LTR-ACK** — app-id `0x00000005`, 16 bytes, 594 pkts (~30/s):
   ```
   80 cc 00 03 <receiverSSRC:4> 00 00 00 05 <ackedTimestamp:4>
   ```
   `<ackedTimestamp>` is a monotonic per-frame value (samples 0x1900,0x1db0,0x2260,0x2710 — Δ≈1200).
   This is the loss-recovery: receiver acks the frames it has so the encoder re-references an acked
   frame instead of emitting a new IDR.
2. **RCTL** — app-id `'RCTL'`, 32 bytes, 290 pkts (~15/s) — rate-control feedback (pmd3
   `ScreenStreamServer._build_rctl_packet` already RE'd this; pairs with `RateAdaptationEnabled=True`).

Plus standard **RR** (~1/s keepalive; device times out at RTCPTimeoutInterval=20 s) + **SDES** + **SR**.

## Answers to the concrete questions

- **Real bitrate tiers / specific resolution?** Tiers are IDENTICAL to ours (our RE was right). Resolution
  is NOT requested — the device picks 1184×2544@60 (padded native). No offer change needed for either.
- **What enables PLI/FIR?** Nothing — Apple never uses them. Recovery is **LTR** (`ltrpEnabled=1`) +
  **LTR-ACK over RTCP APP**. Our PLI/FIR are ignored because the device isn't running that path. Drop them.
- **Second-IDR trigger?** There isn't a periodic one. With LTR the encoder holds long-term refs (acked by
  LTR-ACK) and recovers against them — "1 IDR + P-frames" is correct/expected. IDR only on stream start.
- **Lifetime?** `timeout=20` (== ours). The device also enforces `RTCPTimeoutInterval=20s` with
  `RTCPSendInterval=1s` — send RR ~1/s or the encoder stalls; release with `stopmediastream` to free the slot.

## The fix (smooth video)

1. **Offer:** `f7 ltrpEnabled=1`, remove `f10 fecEnabled`.
2. **Send RTCP as the receiver:** RR (~1/s, have it) + **LTR-ACK** (the 16-byte APP above, per received
   frame, `data` = that frame's RTP timestamp) + optionally **RCTL** for rate adaptation. **Do NOT send
   PLI/FIR.**
3. Keep the reorder buffer (done). LTR-ACK replaces keyframe requests as the recovery path.

Tooling used (in `carplay-dev/scripts/coredevice-host/`, copy over if useful):
`capture-devicehub.sh` (find tunnel utun + tcpdump) and `decode_devicehub.py` (pcap → XPC → mediaBlob
protobuf walk + RTCP breakdown). Re-run any time to diff a fresh session.

---

# Independent confirmation, and the renderer bug the capture did *not* explain

The capture above was decoded a second time from scratch (`scripts/decode-remotexpc.py` against
`logs/devicehub.pcap`), without reference to the findings above, and every measured claim came out
the same. Recording that here because the two decodes are the only ground truth we have.

Confirmed independently:

| claim | how it was re-measured |
|---|---|
| offer differs in exactly two fields | protobuf walk of both blobs, diffed field by field: `[5.7]` Apple 1 / ours 0, `[5.10]` ours 1 / Apple absent. Everything else — codec banks, `ResEntry` lists, `FLS;SW:1;` / `FLS;VRAE:0;SW:1;`, all ten bitrate tiers, `Viceroy 1.7.0` — identical |
| `clientSupportedFeatures=140`, `timeout=20`, mode 5 | read off the two `mediastreamstart` invocations |
| no PLI, no FIR, ever | RTCP census over the whole capture: PT 76 (APP) ×884, 72 (SR) ×28, 73 (RR) ×15. Nothing else |
| LTR-ACK is per-frame, keyed by RTP timestamp | all 594 acked values were matched against the video RTP timestamps: **594 of 594** were timestamps of frames just received, no repeats, receiver→device, receiver's own SSRC |

Two things the capture also settles that are easy to get wrong:

* `timeout: 20` in the invocation is **not** the session lifetime. Apple sends 20 and mirrors for
  minutes. It pairs with the device's `RTCPTimeoutInterval=20s` / `RTCPSendInterval=1s`: the
  stream lives as long as a Receiver Report arrives about once a second. We already send RR at
  1.0 s, so this was never our problem.
* `sessionEventChannel` (a UUID) is in Apple's invocation and absent from ours. Streaming works
  without it, so it is not on the critical path, but it is the only remaining key we do not send.

## What was actually causing the artifacting

The offer and RTCP work above was necessary, but it was not the whole story, and it is worth being
precise about which fault caused what. Two separate defects looked identical on screen:

1. **No LTR-ACK** (fixed per the findings above). Real packet loss smears until the session ends,
   because the encoder never re-anchors.
2. **The app dropped frames before decoding them.** This one showed up even at 0% measured loss,
   which is what made it confusing — the engine's own recording decoded perfectly in ffmpeg while
   the app rendered snow from the same bytes.

Defect 2 was isolated with `app/tools/decodecheck`, which runs the app's real `AnnexBParser` and
`HEVCStream` over a file with no device and no GUI. On `build/diagnose/live.h265` — a capture ffmpeg
decodes cleanly — it reports every NAL round-tripping byte-identically and **VideoToolbox decoding
380 of 380 access units with zero failures**, at chunk sizes down to 4096 bytes. So the parser, the
access-unit assembly and the decoder were all innocent, which left only the display layer.

`AVSampleBufferDisplayLayer` conflates decoding with display: when it judges a frame late or out of
order it discards it, and it discards it *before* the decoder sees it. On ordinary video that is
invisible, because the next keyframe repairs the gap. This stream contains exactly one IDR and
never another, so every discarded frame corrupts all following frames permanently.

The fix (`app/rPlayHub/VideoDecoder.swift`) separates the two stages:

* **Decode** is mandatory and unconditional. Every access unit goes through an explicit
  `VTDecompressionSession`, in order, so the reference chain is never broken.
* **Display** is best-effort. `VideoLayer` keeps only the newest decoded picture and shows it via
  its IOSurface; older undisplayed pictures are simply skipped. Skipping there is free, because the
  decoder has already consumed them.

This is also the arrangement the ports need: `core/hwdecoder.h` is the common decoder interface
(VideoToolbox here, ffmpeg/SDL on Linux and Windows) and it hands back frames, not sample buffers.
`AVSampleBufferDisplayLayer` has no equivalent on any other platform, so nothing about the old path
was portable.

## Regression tests that need no device

* `swiftc -O -o build/decodecheck app/tools/decodecheck/main.swift app/rPlayHub/HEVCStream.swift \
   app/rPlayHub/VideoDecoder.swift -framework AVFoundation -framework VideoToolbox \
   -framework CoreMedia -framework QuartzCore` then `./build/decodecheck build/diagnose/live.h265 4096`
* `RTCPSession.send_ltr_ack(6400)` with SSRC `0x2b3d7837` emits
  `80cc00032b3d78370000000500001900` — byte-identical to the packet in the capture.
* Replaying the capture's video RTP through the engine's ack logic reproduces **all 594** of the
  frames Apple acknowledged and misses none (it adds 7 leading frames, which Apple skips only
  because its decoder had not started yet).

---

# Correction: PLI and FIR are NOT ignored — one bug was hiding three

Earlier sections of this document state that this device ignores PLI and FIR, and that a stream
carries exactly one IDR which can never be refreshed. **That is wrong**, and a lot of design was
built on top of it. The record is corrected here rather than edited above, so the mistake and its
consequences stay visible.

The device discards RTCP that arrives from an SSRC it has not associated with the stream. Field 5.1
of the offer is the receiver's SSRC — the device echoes it back in its answer as `RemoteSSRC` — and
we were generating that field and our RTCP SSRC as two independent random numbers. So everything we
sent was thrown away silently:

| we sent | what we concluded | what was actually happening |
|---|---|---|
| PLI, FIR | "the device ignores RTCP feedback" | discarded: unregistered SSRC |
| LTR-ACK | "acks are working" (`ltr_acked` was climbing) | discarded: same cause |

`ltr_acked` counting up proved only that we were *sending*. That was the assumption worth checking
and it went unchecked for a long time.

With one SSRC used in both the offer and every RTCP packet, a keyframe now arrives after a single
request:

    keyframe arrived after 1 request(s) (pli=3 fir=3)

## What that changes

* **The stream is refreshable.** "One IDR per session" was a symptom, not a property. The engine now
  requests a keyframe every few seconds (`RPLAY_KEYFRAME_EVERY_S`, default 3), so corruption from
  the encoder's tight bitrate is flushed instead of persisting for the rest of the session. Measured
  effect: a swipe still artefacts, then recovers, rather than staying broken.
* **A late-joining viewer can recover.** Previously terminal — it had missed the only keyframe.
* **The bitrate ceiling is unchanged.** This makes the picture *repair*, not become sharp. The
  device still negotiates 1184×2544 at ~4 Mbps whatever we ask, and Device Hub gets the same. For a
  genuinely clean picture the CoreMediaIO capture path (`app/rPlayHub/USBMirror.swift`) is the
  answer, since it carries no such budget.

## The lesson worth keeping

Three separate mechanisms appeared broken in three different ways, and all three were one wire-level
identity bug. Each apparent failure produced a plausible explanation that was wrong, and those
explanations then drove real design decisions. What eventually found it was reading Apple's own
captured answer field by field and noticing the same number in three places — not reasoning about
behaviour.

---

# Correction: the stream does not use long-term references, so LTR-ACK is not the recovery path

Earlier sections say recovery works by long-term references — "With LTR the encoder holds long-term
refs (acked by LTR-ACK) and recovers against them", and "LTR-ACK replaces keyframe requests as the
recovery path". **The bitstream says otherwise.** Recorded here rather than edited above, so the
mistake and what it cost stay visible.

`scripts/hevc-refs.py` parses SPS, PPS and slice segment headers as far as the reference picture
sets, derives POC, and checks every reference against the pictures a decoder would be holding. Run
over four recordings — Apple's own session and three of ours, all from the same device:

| stream | pictures | `long_term_ref_pics_present_flag` | pictures using an LTR |
|---|---:|---:|---:|
| `build/devicehub-recording.h265` (Apple's) | 601 | **0** | **0** |
| `build/diagnose/live.h265` | 382 | **0** | **0** |
| `screen3.h265` | 328 | **0** | **0** |
| `build/viewer-capture.h265` | 455 | **0** | **0** |

`long_term_ref_pics_present_flag` is an **SPS** field, so this is not a sampling accident: with it
clear, no picture anywhere in that session may reference a long-term picture. Zero of 1,766 coded
pictures do. This holds for Apple's stream too, which is the one that looks clean — so LTR is not
what makes Device Hub better, because Device Hub is not getting LTR either.

`ltrpEnabled=1` in the offer and `IsltrpEnabled=True` in the answer therefore describe a capability
that is negotiated and then never exercised. Reading the offer was not enough; the encoder's actual
output is the only thing that settles it.

## What the encoder really does

`scripts/hevc-refstruct.py` prints the prediction structure. It is the same on both sides:

- The SPS declares 11 short-term reference picture sets, retaining progressively more pictures, up
  to 11 back. The extra ones are **held but never marked `used_by_curr_pic`** — the encoder keeps a
  deep pool it could switch to, and in these captures never does.
- Every picture predicts from **−1 and −2 only**. Nothing reaches further back, in Apple's stream or
  ours.
- POC is contiguous in all four recordings: no picture is missing from what reaches the decoder.

## What this rules out

- **Acknowledging a frame we do not hold cannot cause drift here.** The guard in
  `host-c/media.c:ra_frame_cb` exists to prevent the device predicting from a reference we lack.
  Nothing predicts from a long-term reference, so there is no such reference to get wrong. The guard
  is harmless; it is simply not load-bearing, and the comment above it describes a mechanism that is
  not running.
- **LTR-ACK is not a correctness mechanism in this mode.** Keep sending it — Apple does, ~41/s — but
  nothing about the picture depends on it.
- With references at −1/−2 and one IDR per session, a single bad picture propagates until the next
  IDR. That is why corruption tracks the keyframe interval. It is a consequence of the prediction
  structure, not of LTR.

## The lesson, again

This is the same shape of error as the PLI/FIR correction above: a plausible mechanism was inferred
from a negotiated flag and a packet census, and then designed against. What settled it was decoding
the bitstream Apple's own receiver was fed. The offer says what is *possible*; only the slice
headers say what is *happening*.

---

# RCTL, decoded

`doc/RENDERING-HANDOFF.md` lists RCTL as the one feedback Device Hub sends that we never have, and
says the layout must come from a capture because `~/devicehub.pcap` no longer exists. **It does
exist** — `logs/devicehub.pcap`, the same capture the rest of this document was decoded from. No new
session was needed.

290 RCTL packets over 14.5 s. It is an RTCP APP packet, always sent **alone in its own UDP
datagram**, never compounded with SR/RR/SDES:

```
80 cc 00 07 <ssrc:4> 'RCTL' 85 00 00 04 <+16:2> 00 00 00 00 <+22:2> <+24:2> <+26:2> <+28:2> <+30:2>
```

All fields big-endian. Bytes 12..15 are constant `85 00 00 04`; bytes 18..21 are always zero.

| off | field | meaning | evidence |
|---|---|---|---|
| +12 | `85 00 00 04` | constant tag | never varies in 290 packets |
| +16 | u16 | media time of the newest video frame received, in **256-tick units** of the 24 kHz video clock | slope 1/256 vs video RTP timestamp, r=1.0000, rms 0.30 |
| +22 | u16 | **ms since the last video packet arrived** | slope 0.983 vs measured idle time, r=0.98, rms 2.2 ms |
| +24 | u16 | local clock in **1/1024 s** ("binary milliseconds"), wraps | slope 1024.002/s vs wall clock, r=1.000000, rms 0.29 |
| +26 | u16 | **unexplained** | best correlate r=0.59; ruled out: jitter, frame gap, packet backlog, byte counts, field +22, delta(+24) |
| +28 | u16 | cumulative video RTP **packets** received, lagging ~20 ms | 199/290 exact against the count as of 20 ms earlier |
| +30 | u16 | **target bitrate in units of 100 bps** | 60000 = 6 Mbps = the negotiated `TX/RXMaxBitrate` exactly |

Cadence is a **free-running 50 ms timer** (20/s), not frame-driven: intervals mean 50.16 ms, std
5.9 ms, and only 112 of 290 land within 5 ms of a frame (frame-driven would be nearly all).

The first packet is all zeros except +22 and +24, so a sender can start cold.

`+30` is the interesting one: it is the receiver telling the encoder what rate to aim at, and it
carries exactly the negotiated ceiling. Together with `RateAdaptationEnabled=True` in the answer,
this is the closed loop we have never closed — we send no RCTL at all.

Tools: `scripts/decode-rctl.py` (census and dump), `scripts/analyze-rctl.py` (correlation),
`scripts/rctl-layout.py` (byte census and fits), `scripts/rctl-fields.py` (the field battery above).

**A trap worth recording:** the capture carries two RTP streams — video on pt=100 and *audio* on
pt=101 at 100 pkt/s. Treating them as one blurred every fit and made +16 and +24 look like
unrelated counters. They separate cleanly on payload type, and the video clock is 24 kHz (per-frame
timestamp deltas of 400 and 800 ticks, i.e. 60 fps nominal with frequent drops to 30).

## Follow-up: "maybe Apple uses a special LTR that references the IDR"

This hypothesis came back, so here is the refutation in a form that does not depend on trusting our
own parser. Three independent lines, all from `reference/captures/devicehub-iphone13-ios27.pcap`:

1. **Our slice-header parser** (`scripts/hevc-refstruct.py`): reference deltas used across the whole
   stream are −1 (600 times) and −2 (593 times). Nothing else, ever.
2. **The SPS decoded-picture-buffer capacity**: `sps_max_dec_pic_buffering = 12 pictures`. The
   decoder is only required to hold twelve. A picture at POC 90 therefore *cannot* reference the IDR
   at POC 0 — it was evicted seventy-eight frames earlier, and no conforming encoder may assume
   otherwise. This also explains the 11 declared short-term sets: −1 through −11 is exactly what a
   12-picture buffer allows.
3. **ffmpeg, asked directly.** Feed it the parameter sets, the IDR, and then one later picture, and
   it names the reference it cannot find:

   ```
   picture POC 46 -> "Could not find ref with POC 45"
   picture POC 90 -> "Could not find ref with POC 89"
   ```

   Not POC 0. An implementation with no connection to ours reports the immediately preceding
   picture, which is what the parser said.

**And a general argument that makes the whole question moot.** We do not implement reference
management — the decoder does. If this stream used a reference scheme peculiar to Apple,
VideoToolbox (Apple's own decoder) would implement it, and would have produced a clean picture from
these bytes. It produced the *same garbled picture as ffmpeg*, pixel for pixel. So no reference
scheme, special or otherwise, can explain a difference between our app and Device Hub: both run
VideoToolbox, and VideoToolbox garbles these bytes.

The reference structure is not where the fault is. It has now been checked far past the point of
reasonable doubt, and should not be checked again without new evidence.
