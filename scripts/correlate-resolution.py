#!/usr/bin/env python3
"""Find where the per-frame active resolution is signalled on the wire.

avconferenced tells its decoder, per frame, what resolution is actually coded:

    ActiveVideoResolution { Width = 1184; Height = 2576 }
    ActiveVideoResolution { Width = 1088; Height = 1920 }
    ActiveVideoResolution { Width =  720; Height = 1280 }

The SPS says 1184x2576 for the whole session and never changes, and full- and reduced-resolution
frames have structurally identical slice headers. So this is out-of-band, which is why ffmpeg,
VideoToolbox, every build and every configuration reconstruct the same mosaic from these bytes:
none of them is told, because the information is not in the bitstream.

We must therefore recover it from the RTP stream ourselves. This pairs a capture of the wire with
Apple's own per-frame ground truth from the same session and tests every candidate carrier:

  * the RTP header extension profile field (0x9011 / 0x9211 / 0x9001 -- three values, and there
    are three resolutions, which is the leading hypothesis)
  * the extension's two u16 fields
  * the RTP marker bit, payload type, and per-frame packet/byte counts

    python3 scripts/correlate-resolution.py build/paired-YYYYmmdd-HHMMSS
"""
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import importlib.util

_d = importlib.util.spec_from_file_location(
    "d", __file__.rsplit("/", 1)[0] + "/rtp-depacketize.py")
D = importlib.util.module_from_spec(_d); _d.loader.exec_module(D)
_v = importlib.util.spec_from_file_location(
    "v", __file__.rsplit("/", 1)[0] + "/decode-vtcap.py")
V = importlib.util.module_from_spec(_v); _v.loader.exec_module(V)


def apple_frames(path):
    """Apple's per-frame ground truth: (active_resolution, payload_size)."""
    out = []
    for t, p in V.records(path):
        if t != V.REC_DECODE_INPUT:
            continue
        d = V.parse_decode(p)
        m = re.search(r"ActiveVideoResolution =\s*\{\s*Height = (\d+);\s*Width = (\d+);",
                      d["frame_options"])
        out.append(((int(m.group(2)), int(m.group(1))) if m else None, len(d["data"])))
    return out


def wire_frames(path):
    """Per-frame wire facts, grouped by the extension's frame index."""
    frames = defaultdict(lambda: {"profiles": Counter(), "pkts": 0, "bytes": 0,
                                  "count_field": None, "marker": 0, "ts": None})
    for pkt in D.pcap_packets(path):
        if len(pkt) < 12 or (pkt[0] >> 6) != 2:
            continue
        pt = pkt[1] & 0x7F
        if 72 <= pt <= 95 or pt != 100:
            continue
        cc = pkt[0] & 0x0F
        off = 12 + 4 * cc
        if not ((pkt[0] >> 4) & 1):
            continue
        profile = struct.unpack(">H", pkt[off:off + 2])[0]
        words = struct.unpack(">H", pkt[off + 2:off + 4])[0]
        ext = pkt[off + 4:off + 4 + 4 * words]
        off += 4 + 4 * words
        if len(ext) < 4:
            continue
        cnt = struct.unpack(">H", ext[0:2])[0]
        idx = struct.unpack(">H", ext[2:4])[0]
        f = frames[idx]
        f["profiles"][profile] += 1
        f["pkts"] += 1
        f["bytes"] += len(pkt) - off
        f["count_field"] = cnt
        f["marker"] += 1 if (pkt[1] & 0x80) else 0
        f["ts"] = struct.unpack(">I", pkt[4:8])[0]
    return [frames[i] for i in sorted(frames)]


def main():
    d = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else None
    if not d:
        sys.exit("usage: correlate-resolution.py <build/paired-DIR>")
    apple = apple_frames(f"{d}/decoder-input.vtc")
    wire = wire_frames(f"{d}/session.pcap")
    print(f"Apple frames: {len(apple)}   wire frames: {len(wire)}")

    res_counts = Counter(r for r, _ in apple)
    print(f"resolutions Apple reported: "
          f"{ {f'{w}x{h}': n for (w, h), n in res_counts.items() if w} }")
    if len(res_counts) < 2:
        print("\nOnly one resolution in this capture -- swipe harder and re-run;"
              " the reduced modes appear only under heavy motion.")
        return

    # The two lists need aligning: the wire capture and the interposer start at different moments.
    # Align on payload size, which is identical in both and effectively unique per frame.
    wsz = [f["bytes"] for f in wire]
    asz = [s for _, s in apple]
    best, bestoff = -1, 0
    for off in range(-len(asz) + 10, len(wsz) - 10):
        n = min(len(asz), len(wsz) - off) - max(0, -off)
        if n < 30:
            continue
        a0 = max(0, -off)
        hits = sum(1 for k in range(n)
                   if abs(asz[a0 + k] - wsz[off + a0 + k]) <= 8)
        if hits > best:
            best, bestoff = hits, off
    n = min(len(asz), len(wsz) - bestoff) - max(0, -bestoff)
    print(f"alignment: offset {bestoff}, {best}/{n} frames matching by payload size")
    if best < n * 0.6:
        print("  !! weak alignment -- the two captures may not overlap; re-run them together")
        return

    a0 = max(0, -bestoff)
    pairs = [(apple[a0 + k][0], wire[bestoff + a0 + k]) for k in range(n)]

    print("\n=== candidate carriers, tested against Apple's ground truth ===")
    # 1. extension profile
    table = defaultdict(Counter)
    for res, w in pairs:
        for prof in w["profiles"]:
            table[prof][res] += 1
    print("\nRTP extension PROFILE vs active resolution:")
    clean = True
    for prof, c in sorted(table.items()):
        tot = sum(c.values())
        top, topn = c.most_common(1)[0]
        pure = 100 * topn / tot
        if pure < 95:
            clean = False
        print(f"  0x{prof:04x}: {tot:>5} frames -> "
              + ", ".join(f"{w}x{h}:{k}" for (w, h), k in c.most_common())
              + f"   ({pure:.0f}% pure)")
    print("  => PROFILE IDENTIFIES THE RESOLUTION" if clean else
          "  => profile does NOT determine resolution on its own")

    # 2. the extension's count field, and 3. packets per frame
    for name, get in (("extension field 0 (packet count)", lambda w: w["count_field"]),
                      ("packets per frame", lambda w: w["pkts"])):
        t2 = defaultdict(Counter)
        for res, w in pairs:
            t2[get(w)][res] += 1
        amb = sum(1 for v, c in t2.items() if len(c) > 1)
        print(f"\n{name}: {len(t2)} distinct values, {amb} of them ambiguous"
              + ("  <- not a carrier" if amb else "  <- UNAMBIGUOUS"))


if __name__ == "__main__":
    main()
