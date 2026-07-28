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
