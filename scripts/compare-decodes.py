#!/usr/bin/env python3
"""Score a candidate decode against the VideoToolbox RVRA ground truth, frame by frame.

    compare-decodes.py <groundtruth.y4m> <candidate.y4m> <capture.h265> [--per-frame]

The ground truth comes from `build/groundtruth` (app/tools/groundtruth), the one decode of this
stream known to be correct. The candidate is whatever decoder is under test -- stock ffmpeg to
demonstrate the RVRA failure, a patched ffmpeg to measure convergence:

    ffmpeg -i capture.h265 -strict -1 -f yuv4mpegpipe candidate.y4m

PSNR is computed by ffmpeg's own psnr filter (no bespoke pixel code); this script annotates each
frame with its RVRA tier from the capture's trailers and summarizes where the two decodes agree
and where they diverge. The reading that matters: a conformant-but-RVRA-blind decoder is
bit-exact until the first downshift and collapses there; a correctly patched one stays high
through every tier switch.
"""
import re
import subprocess
import sys
import tempfile

TIERS = [(1184, 2576), (1088, 1920), (720, 1280)]


def frame_tiers(path):
    """Per-frame active size from the trailer on each slice NAL (HEVCStream.parseActiveRectTrailer)."""
    data = open(path, "rb").read()
    starts, i = [], 0
    while i + 3 <= len(data):
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, 3)); i += 3; continue
            if i + 4 <= len(data) and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, 4)); i += 4; continue
        i += 1
    tiers = []
    for idx, (s, cl) in enumerate(starts):
        e = starts[idx + 1][0] if idx + 1 < len(starts) else len(data)
        nal = data[s + cl:e]
        if not nal or (nal[0] >> 1) & 0x3F >= 32:      # parameter set, not a slice
            continue
        found = None
        n = len(nal)
        for (w, h) in TIERS:
            pat = bytes([w >> 8, w & 0xFF, h >> 8, h & 0xFF])
            for j in range(n - 5, max(1, n - 24) - 1, -1):
                if nal[j:j + 4] == pat and nal[j + 4] == 0:
                    found = (w, h); break
            if found:
                break
        tiers.append(found or TIERS[0])
    return tiers


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    gt, cand, capture = sys.argv[1:4]
    per_frame = "--per-frame" in sys.argv

    with tempfile.NamedTemporaryFile(suffix=".log") as stats:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", gt, "-i", cand,
             "-lavfi", f"psnr=stats_file={stats.name}", "-f", "null", "-"],
            check=True)
        lines = open(stats.name).read().splitlines()

    psnr = []
    for line in lines:
        m = re.search(r"psnr_avg:(\S+)", line)
        psnr.append(float("inf") if m.group(1) == "inf" else float(m.group(1)))

    tiers = frame_tiers(capture)
    n = min(len(psnr), len(tiers))
    if len(psnr) != len(tiers):
        print(f"note: {len(psnr)} scored frames vs {len(tiers)} slices in the capture; using {n}")

    # Episodes: maximal runs at the same tier.
    episodes = []
    start = 0
    for i in range(1, n + 1):
        if i == n or tiers[i] != tiers[start]:
            episodes.append((start, i - 1, tiers[start]))
            start = i

    def fmt(v):
        return "exact" if v == float("inf") else f"{v:6.2f}"

    print(f"{'frames':>11}  {'tier':>9}  {'min':>6}  {'mean':>6}   verdict")
    for (a, b, (w, h)) in episodes:
        seg = psnr[a:b + 1]
        finite = [v for v in seg if v != float("inf")]
        mn = min(seg)
        mean = (sum(finite) / len(finite)) if finite else float("inf")
        verdict = ("bit-exact" if mn == float("inf")
                   else "ok" if mn >= 40 else "degraded" if mn >= 30 else "BROKEN")
        print(f"{a:5d}-{b:<5d}  {w}x{h:<5}  {fmt(mn)}  {fmt(mean)}   {verdict}")

    first_bad = next((i for i in range(n) if psnr[i] < 40), None)
    worst = min(range(n), key=lambda i: psnr[i])
    print(f"\nframes compared: {n};  first frame under 40 dB: "
          f"{first_bad if first_bad is not None else 'none'};  "
          f"worst: frame {worst} at {fmt(psnr[worst])} dB")

    if per_frame:
        for i in range(n):
            print(f"{i}\t{tiers[i][0]}x{tiers[i][1]}\t{fmt(psnr[i])}")


if __name__ == "__main__":
    main()
