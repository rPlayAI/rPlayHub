# core/ — the portable video path

Everything here is plain C with no platform dependencies, because Linux and Windows must share it
rather than reimplement it. The split:

```
  engine (Annex-B HEVC/H.264 over TCP)
        │
        ▼
  rp_video_stream.c      PORTABLE — NAL splitting, codec classification, parameter-set caching,
                         access-unit assembly, keyframe gating
        │  hw_decode(decoder, window, data, size, flags)
        ▼
  hwdecoder.h            THE PLATFORM SEAM — one header, one implementation per platform
        ├── macOS    VideoToolbox + AVSampleBufferDisplayLayer
        ├── Linux    ffmpeg / libavcodec (+ VA-API), SDL or Wayland
        └── Windows  Media Foundation / D3D11VA, or ffmpeg
```

## Why hwdecoder.h and not an abstraction of our own

It is already the common interface across `~/rplay` and `~/carplay-dev`: the two headers are
**identical**, while the implementations differ completely (419 lines vs 1087). Two independent
projects sharing a contract unchanged is the best evidence available that the boundary is in the
right place, so we adopt it as-is rather than inventing a rival.

`hw_decode(void *decoder, void *window, const char *data, int size, int flags)` takes packed frame
bytes — which is exactly what `rp_pack_length_prefixed()` or `rp_pack_annexb()` produces.

Parts of the interface are AirPlay-era and not ours: the `YOUCAST_PROTOCOL` enum lists LeLink,
GCast and Miracast, and `hw_create_video_decoder` takes OS/model strings. `kProtocolIOSUsbMirroring`
is the closest existing value. Trimming that is a deliberate later step — keep the header
compatible while both projects still share it.

## What is portable and what is not

Portable, and therefore here: the Annex-B parser, the H.264/HEVC differences (1-byte vs 2-byte NAL
headers, SPS/PPS vs VPS/SPS/PPS, IDR type 5 vs IRAP 16-23), access-unit assembly from the
first-slice flag, the keyframe gate, and the crop/letterbox geometry.

Not portable, and therefore behind `hwdecoder.h`: the decoder, the display surface, and input
capture.

## What is implemented

| file | what |
|---|---|
| `rp_video_stream.{h,c}` | Annex-B splitting, H.264/HEVC classification, parameter-set caching, access-unit assembly, keyframe gating |
| `rp_geometry.{h,c}` | crop fraction, letterbox fit, y-axis convention, fractions → HID/pixels |
| `hwdecoder.h` | the platform decoder seam, adopted byte-identical from `~/rplay` and `~/carplay-dev` |
| `test_rp_video_stream.c` | unit tests, plus a pass over real recordings |

Not here yet: the per-platform `hwdecoder` implementations, and the Swift app still carries its own
copy of this logic (`app/rPlayHub/HEVCStream.swift`, `MirrorView.swift`) pending the swap to this
core over a bridging header.

## Build and test

```
make -C core test
```

The synthetic cases prove the code does what was intended; the real-recording pass proves the
intention was right. Both codecs are checked against actual device output:

```
real HEVC recording: 329 pictures, 1 keyframes, 333 NALs
real H.264 recording: 328 pictures, 11 keyframes, 361 NALs
```

Those match `ffprobe` independently (328 frames for each), and the HEVC keyframe count of **1 in
ten seconds** is the measured fact behind the black-window and recording problems.
