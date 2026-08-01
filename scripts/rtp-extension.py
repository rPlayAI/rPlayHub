#!/usr/bin/env python3
"""Decode the RTP header extension this device puts on every video packet.

`core/rp_rtp.h` says only "Every packet from this device carries an extension
header. Skipping it wrongly shifts the payload and nothing parses." So we learned
to step over it correctly and never looked inside. ffmpeg does not look either.
It turns out to carry the one thing a receiver most wants and cannot otherwise
know: how many packets this access unit consists of, stated in every packet of
that access unit, plus a global frame index.

That is an authoritative completeness signal. It does not depend on the marker
bit, on sequence arithmetic, or on waiting for the next frame to start -- all
three of which this project has had bugs in.

    python3 scripts/rtp-extension.py capture.pcap
"""
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import importlib.util
_s = importlib.util.spec_from_file_location(
    "d", __file__.rsplit("/", 1)[0] + "/rtp-depacketize.py")
D = importlib.util.module_from_spec(_s)
_s.loader.exec_module(D)


def extensions(path):
    """Yield (pt, seq, ts, marker, profile, ext_bytes, payload) for RTP packets."""
    for pkt in D.pcap_packets(path):
        if len(pkt) < 12 or (pkt[0] >> 6) != 2:
            continue
        pt = pkt[1] & 0x7F
        if 72 <= pt <= 95:                  # RTCP after the marker bit is masked
            continue
        cc = pkt[0] & 0x0F
        off = 12 + 4 * cc
        ext = b""
        profile = None
        if (pkt[0] >> 4) & 1:
            if off + 4 > len(pkt):
                continue
            profile = struct.unpack(">H", pkt[off:off + 2])[0]
            words = struct.unpack(">H", pkt[off + 2:off + 4])[0]
            ext = pkt[off + 4:off + 4 + 4 * words]
            off += 4 + 4 * words
        yield (pt, struct.unpack(">H", pkt[2:4])[0],
               struct.unpack(">I", pkt[4:8])[0],
               bool(pkt[1] & 0x80), profile, ext, pkt[off:])


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "reference/captures/devicehub-iphone13-ios27.pcap"
    rows = list(extensions(path))
    print(f"=== {path} ===")

    bypt = defaultdict(list)
    for r in rows:
        bypt[r[0]].append(r)
    for pt, rs in sorted(bypt.items()):
        withext = sum(1 for r in rs if r[4] is not None)
        print(f"payload type {pt}: {len(rs)} packets, {withext} with an extension")

    vid = max(bypt, key=lambda p: sum(len(r[6]) for r in bypt[p]))
    rs = [r for r in bypt[vid] if r[4] is not None]
    print(f"\nvideo is payload type {vid}; {len(rs)} packets carry extensions")
    print(f"extension profiles: "
          f"{ {f'0x{p:04x}': n for p, n in Counter(r[4] for r in rs).items()} }")
    print(f"extension lengths: {dict(Counter(len(r[5]) for r in rs))} bytes")

    # Group by the declared frame index and verify the declared packet count.
    frames = defaultdict(lambda: {"pkts": [], "profiles": Counter()})
    for pt, seq, ts, m, prof, ext, pay in rs:
        if len(ext) < 4:
            continue
        cnt = struct.unpack(">H", ext[0:2])[0]
        idx = struct.unpack(">H", ext[2:4])[0]
        f = frames[idx]
        f["pkts"].append((seq, ts, m, cnt))
        f["profiles"][prof] += 1

    idxs = sorted(frames)
    print(f"\nframe index runs {idxs[0]}..{idxs[-1]} over {len(idxs)} frames, "
          f"gaps: {sum(1 for a,b in zip(idxs,idxs[1:]) if b!=a+1)}")
    print("  (it does not start at zero -- the device's screen encoder is already")
    print("   running when the session joins, so this is a global frame counter)")

    bad = [(i, len(f["pkts"]), f["pkts"][0][3]) for i, f in frames.items()
           if len(f["pkts"]) != f["pkts"][0][3]]
    inconsistent = [i for i, f in frames.items()
                    if len({p[3] for p in f["pkts"]}) != 1]
    print(f"\nfield 0 (u16) vs packets actually present:")
    print(f"  frames where they disagree : {len(bad)}" +
          (f"  {bad[:8]}" if bad else "   <- field 0 IS the packet count"))
    print(f"  frames where the count differs between their own packets: "
          f"{len(inconsistent)}")

    # Profile semantics: which frames use which profile.
    prof_by_size = defaultdict(Counter)
    for i, f in frames.items():
        for p, n in f["profiles"].items():
            prof_by_size[p][len(f["pkts"])] += 1
    print("\nprofile vs frame size (packets per frame):")
    for p, sizes in sorted(prof_by_size.items()):
        s = sorted(sizes)
        print(f"  0x{p:04x}: frames of {s[0]}..{s[-1]} packets "
              f"({sum(sizes.values())} frames)")

    print("\nsample, one line per frame:")
    print(f"  {'idx':>6} {'pkts':>5} {'declared':>9} {'rtp ts':>9} {'profile':>8}")
    for i in idxs[:6] + idxs[len(idxs)//2:len(idxs)//2+3] + idxs[-3:]:
        f = frames[i]
        print(f"  {i:>6} {len(f['pkts']):>5} {f['pkts'][0][3]:>9} "
              f"{f['pkts'][0][1]:>9} "
              f"{'/'.join(f'0x{p:04x}' for p in f['profiles']):>8}")

    print("""
What this gives a receiver that nothing else does:

  * the total packet count of an access unit, present in EVERY packet of it, so
    completeness is known from the first packet rather than inferred from the
    marker bit or from the next frame starting;
  * a frame index, so a frame the encoder never sent is distinguishable from one
    lost in transit.

Neither core/rp_rtp.c nor core/rp_rtp_assembler.c nor ffmpeg reads it.""")


if __name__ == "__main__":
    main()
