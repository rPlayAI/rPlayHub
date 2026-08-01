#!/usr/bin/env python3
"""Put a decoded packet-frame next to the Device Hub window at the same instant.

Given a directory from scripts/capture-correlated.sh, this maps each decoded
frame back to the wall-clock time its last RTP packet arrived (the marker bit
ends an access unit, and the pcap timestamps it), then finds the window still
captured nearest that moment.

That is the comparison the whole investigation reduces to. Device Hub's own
packets decode to badly garbled pictures; Device Hub's window looked clean. One
of those observations is about the wrong thing, and only a photograph taken at
the same instant can say which.

    python3 scripts/correlate-capture.py build/corr-YYYYmmdd-HHMMSS
    python3 scripts/correlate-capture.py build/corr-... --frame 330
"""
import argparse
import os
import struct
import subprocess
import sys


def pcap_marker_times(path):
    """Wall-clock time of each video frame, taken from the packet carrying the
    RTP marker bit (which is what ends an access unit)."""
    out = []
    with open(path, "rb") as f:
        hdr = f.read(24)
        end = "<" if hdr[:4] == b"\xd4\xc3\xb2\xa1" else ">"
        # Find the busiest RTP stream first, the same way pcapreplay does:
        # by volume, never by payload number.
        pkts = []
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                break
            sec, usec, caplen, _ = struct.unpack(end + "IIII", ph)
            d = f.read(caplen)
            if len(d) < caplen:
                break
            if len(d) < 44 or struct.unpack(end + "I", d[:4])[0] != 30:
                continue
            ip = d[4:]
            if ip[6] != 17:
                continue
            rest = ip[40:]
            if len(rest) < 8:
                continue
            ulen = struct.unpack(">H", rest[4:6])[0]
            p = rest[8:ulen] if 8 <= ulen <= len(rest) else rest[8:]
            if len(p) < 12 or (p[0] >> 6) != 2:
                continue
            pt = p[1] & 0x7F
            if 72 <= pt <= 95:          # RTCP, after the marker bit is masked off
                continue
            pkts.append((sec + usec / 1e6, struct.unpack(">I", p[8:12])[0],
                         pt, bool(p[1] & 0x80), len(p)))

    vol = {}
    for t, ssrc, pt, m, n in pkts:
        vol[(ssrc, pt)] = vol.get((ssrc, pt), 0) + n
    if not vol:
        sys.exit("no RTP found in the capture")
    key = max(vol, key=vol.get)
    for t, ssrc, pt, m, n in pkts:
        if (ssrc, pt) == key and m:
            out.append(t)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--frame", type=int, default=None,
                    help="decoded frame number (1-based, as written by ffmpeg)")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    d = a.dir.rstrip("/")
    times = pcap_marker_times(f"{d}/session.pcap")
    shots = sorted(x for x in os.listdir(f"{d}/shots") if x.endswith(".jpg"))
    shot_times = [float(x) for x in open(f"{d}/shots/times.txt")]
    n = min(len(shots), len(shot_times))
    shots, shot_times = shots[:n], shot_times[:n]

    print(f"{len(times)} decoded frames, {len(shots)} window stills")
    if not times or not shots:
        sys.exit("nothing to correlate")
    print(f"packets span {times[-1]-times[0]:.1f}s, stills span "
          f"{shot_times[-1]-shot_times[0]:.1f}s "
          f"({len(shots)/(shot_times[-1]-shot_times[0]):.1f} stills/s)")

    frames = [a.frame] if a.frame else None
    if not frames:
        # Default: the frames most likely to be garbled are the small ones
        # during motion, so offer a spread across the middle of the capture.
        frames = [int(len(times) * f) for f in (0.4, 0.55, 0.7, 0.85)]

    for fr in frames:
        if not (1 <= fr <= len(times)):
            print(f"frame {fr} out of range 1..{len(times)}")
            continue
        t = times[fr - 1]
        j = min(range(len(shot_times)), key=lambda k: abs(shot_times[k] - t))
        dt = (shot_times[j] - t) * 1000
        dec = f"{d}/decoded/f-{fr:04d}.png"
        win = f"{d}/shots/{shots[j]}"
        out = a.out or f"{d}/compare-{fr:04d}.png"
        if not os.path.exists(dec):
            print(f"frame {fr}: {dec} missing")
            continue
        # Scale the decoded frame to the still's height so they sit side by side.
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-i", dec, "-i", win,
             "-filter_complex",
             "[0:v]scale=-1:800[a];[1:v]scale=-1:800[b];[a][b]hstack",
             out], check=False)
        print(f"frame {fr:>4}  t={t-times[0]:6.2f}s  nearest still {shots[j]} "
              f"({dt:+.0f} ms)  -> {out}")

    print("\nLeft = what the packets decode to. Right = what Device Hub showed.")
    print("If the left is garbled and the right is clean at the same instant,")
    print("Device Hub is not displaying this RTP stream.")


if __name__ == "__main__":
    main()
