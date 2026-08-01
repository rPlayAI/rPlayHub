#!/usr/bin/env python3
"""Identify the fields of Device Hub's RCTL packet by correlating them with the
RTP stream captured alongside them.

decode-rctl.py shows RCTL exists and is 32 bytes. This works out what the 20
payload bytes mean: it tries u16/u32 at every offset in both endiannesses, then
scores each candidate against quantities measured from the video RTP in the same
pcap (bytes received, packets, frames, loss, arrival time). A field that tracks a
measured quantity to within a constant factor is identified; one that does not is
reported as unexplained rather than guessed at.

    python3 scripts/analyze-rctl.py logs/devicehub.pcap
"""
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from importlib import import_module

_rctl = import_module("decode-rctl".replace("-", "_")) if False else None

# --- pcap / UDP (kept standalone so this script runs on its own) --------------

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


def udp_datagrams(path):
    for ts, frame in packets(path):
        if len(frame) < 44:
            continue
        if struct.unpack("<I", frame[:4])[0] != 30:      # AF_INET6
            continue
        ip = frame[4:]
        if ip[6] != 17:                                   # UDP
            continue
        src, dst = ip[8:24], ip[24:40]
        rest = ip[40:]
        if len(rest) < 8:
            continue
        sport, dport, ulen, _ = struct.unpack(">HHHH", rest[:8])
        yield ts, src, dst, sport, dport, rest[8:]


def is_rtcp(p):
    return len(p) >= 4 and (p[0] >> 6) == 2 and 200 <= p[1] <= 223


def is_rtp(p):
    return len(p) >= 12 and (p[0] >> 6) == 2 and not (200 <= p[1] & 0x7F <= 223)


# --- collect -----------------------------------------------------------------

def collect(path):
    rctl, ltr, rtp = [], [], []
    for ts, src, dst, sport, dport, p in udp_datagrams(path):
        if is_rtcp(p):
            off = 0
            while off + 4 <= len(p):
                if (p[off] >> 6) != 2:
                    break
                pt = p[off + 1]
                total = (struct.unpack(">H", p[off + 2:off + 4])[0] + 1) * 4
                if total <= 0 or off + total > len(p):
                    break
                sub = p[off:off + total]
                if pt == 204 and len(sub) >= 12:
                    if sub[8:12] == b"RCTL":
                        rctl.append((ts, sub))
                    elif sub[8:12] == b"\x00\x00\x00\x05":
                        ltr.append((ts, struct.unpack(">I", sub[12:16])[0]))
                off += total
        elif is_rtp(p):
            seq = struct.unpack(">H", p[2:4])[0]
            tsr = struct.unpack(">I", p[4:8])[0]
            marker = bool(p[1] & 0x80)
            rtp.append((ts, seq, tsr, marker, len(p)))
    return rctl, ltr, rtp


# --- candidate fields --------------------------------------------------------

def candidates(pkts):
    """Every u16/u32 view of the 20 payload bytes, both endiannesses."""
    body_len = len(pkts[0][1])
    out = {}
    for width, fmts in ((2, (">H", "<H")), (4, (">I", "<I"))):
        for off in range(12, body_len - width + 1):
            for fmt in fmts:
                vals = [struct.unpack(fmt, s[off:off + width])[0] for _, s in pkts]
                if len(set(vals)) == 1:
                    continue
                tag = f"+{off:<2} {'u16' if width == 2 else 'u32'}{'be' if '>' in fmt else 'le'}"
                out[tag] = vals
    return out


def pearson(a, b):
    n = len(a)
    if n < 3:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((x - mb) ** 2 for x in b)
    if va <= 0 or vb <= 0:
        return 0.0
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    return cov / (va * vb) ** 0.5


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/devicehub.pcap"
    rctl, ltr, rtp = collect(path)
    if not rctl:
        sys.exit("no RCTL packets in " + path)

    t0 = min(rctl[0][0], rtp[0][0] if rtp else rctl[0][0])
    print(f"=== {path} ===")
    print(f"RCTL {len(rctl)}  LTR-ACK {len(ltr)}  RTP {len(rtp)}   "
          f"span {rctl[-1][0] - rctl[0][0]:.1f}s")

    # Reference quantities measured from the RTP stream, sampled at each RTCP time.
    rtp_sorted = sorted(rtp)
    cum_bytes, cum_pkts, cum_frames = [], [], []
    b = p = fr = 0
    times = []
    for ts, seq, tsr, marker, ln in rtp_sorted:
        b += ln
        p += 1
        if marker:
            fr += 1
        times.append(ts)
        cum_bytes.append(b)
        cum_pkts.append(p)
        cum_frames.append(fr)

    def at(t, cum):
        lo, hi = 0, len(times)
        while lo < hi:
            mid = (lo + hi) // 2
            if times[mid] <= t:
                lo = mid + 1
            else:
                hi = mid
        return cum[lo - 1] if lo else 0

    ts_r = [t for t, _ in rctl]
    refs = {
        "elapsed_ms": [(t - t0) * 1000 for t in ts_r],
        "cum_rtp_bytes": [at(t, cum_bytes) for t in ts_r],
        "cum_rtp_packets": [at(t, cum_pkts) for t in ts_r],
        "cum_rtp_frames": [at(t, cum_frames) for t in ts_r],
    }
    # Per-interval rates
    for name in ("cum_rtp_bytes", "cum_rtp_packets", "cum_rtp_frames"):
        c = refs[name]
        refs[name.replace("cum_", "d_")] = [0] + [y - x for x, y in zip(c, c[1:])]

    cand = candidates(rctl)
    print(f"\n{len(cand)} varying field candidates; correlating against "
          f"{len(refs)} measured quantities\n")
    print(f"{'field':<12} {'first':>12} {'last':>12} {'distinct':>8}  best match")
    print("-" * 78)

    for tag, vals in sorted(cand.items()):
        best, bestr = None, 0.0
        for rname, rvals in refs.items():
            r = abs(pearson(vals, rvals))
            if r > bestr:
                best, bestr = rname, r
        note = f"{best} r={bestr:.4f}" if bestr > 0.90 else \
               f"(none; best {best} r={bestr:.2f})"
        print(f"{tag:<12} {vals[0]:>12} {vals[-1]:>12} "
              f"{len(set(vals)):>8}  {note}")

    # The wrap-aware check for a millisecond clock, which correlation misses.
    print("\n--- millisecond-clock check (correlation cannot see a wrap) ---")
    for tag, vals in sorted(cand.items()):
        if "u16" not in tag:
            continue
        unwrapped, acc, prev = [], 0, vals[0]
        for v in vals:
            if v < prev - 30000:
                acc += 65536
            unwrapped.append(v + acc)
            prev = v
        drift = [(u - unwrapped[0]) - (t - ts_r[0]) * 1000 for u, t in zip(unwrapped, ts_r)]
        span = max(drift) - min(drift)
        if span < 200:
            print(f"{tag}: tracks wall clock in MILLISECONDS "
                  f"(max drift {span:.0f} ms over {ts_r[-1] - ts_r[0]:.1f}s)")


if __name__ == "__main__":
    main()
