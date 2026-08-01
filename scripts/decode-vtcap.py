#!/usr/bin/env python3
"""Read a vtcapture file — what a process fed its VideoToolbox decoder.

Produced by tools/vtcapture/vtcapture.dylib loaded into Device Hub (or into any
process, for validation). Prints the decoder configuration and a per-frame
table, and can dump the elementary stream so it can be diffed against ours or
decoded independently by ffmpeg.

    python3 scripts/decode-vtcap.py cap.vtc                  # summary
    python3 scripts/decode-vtcap.py cap.vtc --frames 40      # first 40 frames
    python3 scripts/decode-vtcap.py cap.vtc --annexb out.h265  # elementary stream
"""
import argparse
import struct
import sys
from collections import Counter

REC_SESSION_CREATE, REC_DECODE_INPUT, REC_FORMAT, REC_NOTE = 1, 2, 3, 4

CODECS = {0x61766331: "avc1 (H.264)", 0x68766331: "hvc1 (HEVC)",
          0x68657631: "hev1 (HEVC)", 0x61766333: "avc3 (H.264)"}

# HEVC NAL types we care about naming; the rest print as their number.
HEVC_NAL = {32: "VPS", 33: "SPS", 34: "PPS", 35: "AUD", 39: "SEI-prefix",
            40: "SEI-suffix", 19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA",
            1: "TRAIL_R", 0: "TRAIL_N"}
H264_NAL = {7: "SPS", 8: "PPS", 5: "IDR", 1: "non-IDR", 6: "SEI", 9: "AUD"}


class Reader:
    def __init__(self, blob):
        self.b, self.o = blob, 0

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.o)[0]; self.o += 4; return v

    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.o)[0]; self.o += 8; return v

    def i64(self):
        v = struct.unpack_from("<q", self.b, self.o)[0]; self.o += 8; return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4; return v

    def f64(self):
        v = struct.unpack_from("<d", self.b, self.o)[0]; self.o += 8; return v

    def blob(self, n):
        v = self.b[self.o:self.o + n]; self.o += n; return v

    def string(self):
        return self.blob(self.u32()).decode("utf-8", "replace")


def records(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"VTC1":
        sys.exit(f"{path}: not a vtcapture file (magic {data[:4]!r})")
    off = 4
    while off + 8 <= len(data):
        rtype, rlen = struct.unpack_from("<II", data, off)
        off += 8
        if off + rlen > len(data):
            print(f"[truncated record at {off}, {rlen} bytes wanted]", file=sys.stderr)
            break
        yield rtype, data[off:off + rlen]
        off += rlen


def parse_format(payload):
    r = Reader(payload)
    f = {"id": r.u32(), "codec": r.u32(), "width": r.u32(), "height": r.u32(),
         "nal_len_size": r.u32()}
    n = r.u32()
    f["param_sets"] = [r.blob(r.u32()) for _ in range(n)]
    f["extensions"] = r.string()
    return f


def parse_decode(payload):
    r = Reader(payload)
    d = {"t": r.f64(), "session": r.u64(), "pts_v": r.i64(), "pts_ts": r.i32(),
         "dts_v": r.i64(), "dts_ts": r.i32(), "flags": r.u32(),
         "not_sync": r.u32(), "fmt": r.u32()}
    d["frame_options"] = r.string()          # per-frame decode options, "" when none
    d["data"] = r.blob(r.u32())
    return d


def parse_session(payload):
    r = Reader(payload)
    s = {"t": r.f64(), "session": r.u64(), "fmt": r.u32(), "which": r.u32()}
    n = r.u32()
    s["args"] = [(r.string(), r.string()) for _ in range(n)]
    return s


def walk_nals(data, nal_len_size, hevc):
    """Split a length-prefixed (AVCC/HVCC) sample into NAL units."""
    out, off = [], 0
    while off + nal_len_size <= len(data):
        n = int.from_bytes(data[off:off + nal_len_size], "big")
        off += nal_len_size
        if n <= 0 or off + n > len(data):
            out.append(("TRUNCATED", len(data) - off))
            break
        hdr = data[off]
        if hevc:
            t = (hdr >> 1) & 0x3F
            name = HEVC_NAL.get(t, str(t))
        else:
            t = hdr & 0x1F
            name = H264_NAL.get(t, str(t))
        out.append((name, n))
        off += n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--frames", type=int, default=20)
    ap.add_argument("--annexb", metavar="FILE",
                    help="write the elementary stream as Annex-B")
    args = ap.parse_args()

    formats, decodes, sessions = {}, [], []
    for rtype, payload in records(args.path):
        if rtype == REC_FORMAT:
            f = parse_format(payload)
            formats[f["id"]] = f
        elif rtype == REC_DECODE_INPUT:
            decodes.append(parse_decode(payload))
        elif rtype == REC_SESSION_CREATE:
            sessions.append(parse_session(payload))

    print(f"=== {args.path} ===")
    print(f"{len(sessions)} session creations, {len(formats)} formats, "
          f"{len(decodes)} decode calls")

    for s in sessions:
        which = "CreateWithOptions" if s["which"] else "Create"
        print(f"\n--- VTDecompressionSession{which} at t={s['t']:.3f}s "
              f"(format #{s['fmt']}) ---")
        for label, value in s["args"]:
            if value:
                print(f"  {label}: {value}")

    for fid, f in sorted(formats.items()):
        codec = CODECS.get(f["codec"],
                           struct.pack(">I", f["codec"]).decode("ascii", "replace"))
        print(f"\n--- format #{fid}: {codec} {f['width']}x{f['height']}, "
              f"NAL length prefix {f['nal_len_size']} bytes, "
              f"{len(f['param_sets'])} parameter sets ---")
        hevc = "HEVC" in codec
        for i, ps in enumerate(f["param_sets"]):
            t = (ps[0] >> 1) & 0x3F if hevc else ps[0] & 0x1F
            name = (HEVC_NAL if hevc else H264_NAL).get(t, str(t))
            print(f"  [{i}] {name:<10} {len(ps):>4} B  {ps[:32].hex()}")
        if f["extensions"]:
            print(f"  extensions: {f['extensions'][:600]}")

    if not decodes:
        print("\nNo decode calls captured.")
        return

    hevc = any("HEVC" in CODECS.get(f["codec"], "") for f in formats.values())
    nls = next(iter(formats.values()))["nal_len_size"] if formats else 4

    t0 = decodes[0]["t"]
    span = decodes[-1]["t"] - t0
    total = sum(len(d["data"]) for d in decodes)
    sizes = sorted(len(d["data"]) for d in decodes)
    print(f"\n--- decoder input: {len(decodes)} frames over {span:.2f}s "
          f"({len(decodes) / span:.1f} fps) ---")
    print(f"  total {total} B, mean {total / len(decodes):.0f} B, "
          f"median {sizes[len(sizes) // 2]} B, peak {sizes[-1]} B, "
          f"{total * 8 / span / 1e6:.2f} Mbit/s")
    print(f"  keyframes (sync samples): {sum(1 for d in decodes if not d['not_sync'])}")
    print(f"  decode flags seen: "
          f"{dict(Counter(d['flags'] for d in decodes))}")
    opts = Counter(d.get("frame_options", "") for d in decodes)
    print(f"  per-frame options seen: "
          f"{ {k if k else '<none>': v for k, v in opts.items()} }")

    gaps = [b["t"] - a["t"] for a, b in zip(decodes, decodes[1:])]
    if gaps:
        gs = sorted(gaps)
        print(f"  inter-frame arrival: median {gs[len(gs) // 2] * 1000:.1f} ms, "
              f"p95 {gs[int(len(gs) * 0.95)] * 1000:.1f} ms, "
              f"max {gs[-1] * 1000:.1f} ms")

    print(f"\n  {'#':>4} {'t(s)':>8} {'bytes':>8} {'pts':>12} {'sync':>5}  NAL units")
    for i, d in enumerate(decodes[:args.frames]):
        nals = walk_nals(d["data"], nls, hevc)
        desc = " ".join(f"{n}:{ln}" for n, ln in nals[:8])
        if len(nals) > 8:
            desc += f" +{len(nals) - 8} more"
        print(f"  {i:>4} {d['t'] - t0:>8.3f} {len(d['data']):>8} "
              f"{d['pts_v']:>12} {'Y' if not d['not_sync'] else '':>5}  {desc}")

    if args.annexb:
        with open(args.annexb, "wb") as out:
            for f in formats.values():
                for ps in f["param_sets"]:
                    out.write(b"\x00\x00\x00\x01" + ps)
            for d in decodes:
                off = 0
                data = d["data"]
                while off + nls <= len(data):
                    n = int.from_bytes(data[off:off + nls], "big")
                    off += nls
                    if n <= 0 or off + n > len(data):
                        break
                    out.write(b"\x00\x00\x00\x01" + data[off:off + n])
                    off += n
        print(f"\nwrote Annex-B elementary stream to {args.annexb}")


if __name__ == "__main__":
    main()
