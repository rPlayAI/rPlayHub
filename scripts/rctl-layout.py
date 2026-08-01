#!/usr/bin/env python3
"""Work out RCTL's byte layout from the capture, then fit its fields to units.

Two passes, both measured rather than guessed:

1. A column-wise byte census over all RCTL packets. Bytes that never change are
   structure; bytes that change slowly are the high half of a counter; bytes that
   change constantly are the low half. This reveals field boundaries without
   assuming any alignment.
2. A least-squares fit of each field against the quantities measured from the RTP
   in the same capture, so a correlation becomes a slope with a unit.

    python3 scripts/rctl-layout.py logs/devicehub.pcap
"""
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0])


def packets(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        end = "<" if hdr[:4] == b"\xd4\xc3\xb2\xa1" else ">"
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            sec, usec, caplen, _ = struct.unpack(end + "IIII", ph)
            data = f.read(caplen)
            if len(data) < caplen:
                return
            yield sec + usec / 1e6, data


def udp(path):
    for ts, frame in packets(path):
        if len(frame) < 44 or struct.unpack("<I", frame[:4])[0] != 30:
            continue
        ip = frame[4:]
        if ip[6] != 17:
            continue
        rest = ip[40:]
        if len(rest) < 8:
            continue
        yield ts, ip[8:24], ip[24:40], rest[8:]


def collect(path):
    rctl, rtp = [], []
    for ts, src, dst, p in udp(path):
        if len(p) >= 4 and (p[0] >> 6) == 2 and 200 <= p[1] <= 223:
            off = 0
            while off + 4 <= len(p):
                if (p[off] >> 6) != 2:
                    break
                pt = p[off + 1]
                total = (struct.unpack(">H", p[off + 2:off + 4])[0] + 1) * 4
                if total <= 0 or off + total > len(p):
                    break
                if pt == 204 and p[off + 8:off + 12] == b"RCTL":
                    rctl.append((ts, p[off:off + total], src, dst))
                off += total
        elif len(p) >= 12 and (p[0] >> 6) == 2 and not (200 <= p[1] & 0x7F <= 223):
            rtp.append((ts, struct.unpack(">H", p[2:4])[0],
                        struct.unpack(">I", p[4:8])[0], bool(p[1] & 0x80), len(p)))
    return rctl, sorted(rtp)


def fit(x, y):
    """Least squares y = a*x + b; returns (a, b, max abs residual)."""
    n = len(x)
    mx, my = sum(x) / n, sum(y) / n
    vx = sum((v - mx) ** 2 for v in x)
    if vx == 0:
        return 0.0, my, 0.0
    a = sum((u - mx) * (v - my) for u, v in zip(x, y)) / vx
    b = my - a * mx
    resid = max(abs(v - (a * u + b)) for u, v in zip(x, y))
    return a, b, resid


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/devicehub.pcap"
    rctl, rtp = collect(path)
    bodies = [s for _, s, _, _ in rctl]
    ts_r = [t for t, _, _, _ in rctl]
    n = len(bodies)
    print(f"=== {path}: {n} RCTL packets, {len(bodies[0])} bytes each ===\n")

    # --- pass 1: column-wise byte census ---
    print("byte census (offset from start of the RTCP APP packet)")
    print(f"{'off':>4}  {'distinct':>8}  {'first':>5}  {'changes':>7}  role")
    print("-" * 62)
    for off in range(len(bodies[0])):
        col = [b[off] for b in bodies]
        distinct = len(set(col))
        changes = sum(1 for a, b in zip(col, col[1:]) if a != b)
        if off < 4:
            role = "RTCP header (V/P/subtype, PT=204, length)"
        elif off < 8:
            role = "SSRC"
        elif off < 12:
            role = "name 'RCTL'"
        elif distinct == 1:
            role = f"CONSTANT 0x{col[0]:02x}"
        elif changes < n * 0.1:
            role = f"slow ({changes} changes) — high byte of a counter"
        elif changes > n * 0.8:
            role = f"fast ({changes} changes) — low byte"
        else:
            role = f"medium ({changes} changes)"
        print(f"{off:>4}  {distinct:>8}  0x{col[0]:02x}   {changes:>7}  {role}")

    # --- reference quantities from RTP ---
    cum_b = cum_p = cum_f = 0
    times, cb, cp, cf = [], [], [], []
    for t, seq, tsr, marker, ln in rtp:
        cum_b += ln
        cum_p += 1
        cum_f += 1 if marker else 0
        times.append(t)
        cb.append(cum_b)
        cp.append(cum_p)
        cf.append(cum_f)

    def at(t, cum):
        lo, hi = 0, len(times)
        while lo < hi:
            mid = (lo + hi) // 2
            if times[mid] <= t:
                lo = mid + 1
            else:
                hi = mid
        return cum[lo - 1] if lo else 0

    t0 = ts_r[0]
    refs = {
        "elapsed seconds":      [t - t0 for t in ts_r],
        "cumulative RTP bytes": [at(t, cb) for t in ts_r],
        "cumulative RTP pkts":  [at(t, cp) for t in ts_r],
        "cumulative RTP frames":[at(t, cf) for t in ts_r],
    }
    print(f"\nRTP in this capture: {cum_p} packets, {cum_b} bytes, {cum_f} frames "
          f"over {times[-1] - times[0]:.1f}s "
          f"({cum_b * 8 / (times[-1] - times[0]) / 1e6:.2f} Mbit/s, "
          f"{cum_f / (times[-1] - times[0]):.1f} fps)")

    # --- pass 2: fit each aligned field ---
    print("\nfield fits (only those matching a measured quantity closely)")
    print(f"{'field':<12} {'range':>22}  matches")
    print("-" * 78)
    seen = set()
    for width, fmts in ((2, (">H", "<H")), (4, (">I", "<I"))):
        for off in range(12, len(bodies[0]) - width + 1):
            for fmt in fmts:
                vals = [struct.unpack(fmt, b[off:off + width])[0] for b in bodies]
                if len(set(vals)) < 3:
                    continue
                for rname, rvals in refs.items():
                    a, b_, resid = fit(rvals, vals)
                    spread = max(vals) - min(vals)
                    if spread == 0 or resid > 0.02 * spread:
                        continue
                    tag = f"+{off} {'u16' if width == 2 else 'u32'}{'be' if '>' in fmt else 'le'}"
                    key = (tag, rname)
                    if key in seen:
                        continue
                    seen.add(key)
                    print(f"{tag:<12} {min(vals):>10}..{max(vals):<10}  "
                          f"{a:.4g} per {rname} (resid {resid:.3g})")


if __name__ == "__main__":
    main()
