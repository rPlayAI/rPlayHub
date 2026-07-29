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
3. **Its RTCP feedback, RCTL included.** We send RR, PLI, FIR and LTR-ACK. RCTL is the only
   feedback Device Hub sends that we never have, and it is how the encoder picks its rate. Not
   present in `~/carplay-dev` (those greps were all `prctl`), so the layout must come from a
   capture. `~/devicehub.pcap` no longer exists.

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
