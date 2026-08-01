#!/usr/bin/env python3
"""Decode the RTCP feedback Device Hub sends, RCTL in particular.

RCTL is the one packet Device Hub sends that we never have. It is an RTCP APP
packet with app-id 'RCTL', and it is how the receiver drives the encoder's rate
control (it pairs with RateAdaptationEnabled=True in the device's answer).

Reads a pcap captured on the CoreDevice tunnel utun (DLT_NULL, cleartext IPv6)
and prints every distinct RCTL body plus a field-by-field guess at the layout.

    python3 scripts/decode-rctl.py logs/devicehub.pcap
"""
import struct
import sys
from collections import Counter, defaultdict

DLT_NULL = 0


def packets(path):
    """Yield (ts, link-layer payload) from a classic pcap."""
    with open(path, "rb") as f:
        hdr = f.read(24)
        magic = hdr[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            sys.exit(f"not a classic pcap: magic {magic.hex()}")
        link = struct.unpack(end + "I", hdr[20:24])[0]
        if link != DLT_NULL:
            sys.exit(f"expected DLT_NULL (utun), got linktype {link}")
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
    """Yield (ts, src, dst, sport, dport, payload) for UDP over IPv6/IPv4."""
    for ts, frame in packets(path):
        if len(frame) < 4:
            continue
        af = struct.unpack("<I", frame[:4])[0]
        ip = frame[4:]
        if af == 30 and len(ip) >= 40:            # AF_INET6
            nxt = ip[6]
            src, dst = ip[8:24], ip[24:40]
            rest = ip[40:]
        elif af == 2 and len(ip) >= 20:           # AF_INET
            ihl = (ip[0] & 0xF) * 4
            nxt = ip[9]
            src, dst = ip[12:16], ip[16:20]
            rest = ip[ihl:]
        else:
            continue
        if nxt != 17 or len(rest) < 8:            # UDP
            continue
        sport, dport, ulen, _ = struct.unpack(">HHHH", rest[:8])
        yield ts, src, dst, sport, dport, rest[8:ulen - 8 + 8]


def rtcp_subpackets(payload):
    """Walk a compound RTCP packet; yield (pt, subtype, body-with-header)."""
    off = 0
    while off + 4 <= len(payload):
        b0 = payload[off]
        if (b0 >> 6) != 2:                        # version must be 2
            return
        pt = payload[off + 1]
        length = struct.unpack(">H", payload[off + 2:off + 4])[0]
        total = (length + 1) * 4
        if total <= 0 or off + total > len(payload):
            return
        yield pt, b0 & 0x1F, payload[off:off + total]
        off += total


def is_rtcp(payload):
    """RTP and RTCP both say version 2 — gate on the payload type range."""
    return len(payload) >= 4 and (payload[0] >> 6) == 2 and 200 <= payload[1] <= 223


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "logs/devicehub.pcap"
    census = Counter()
    apps = defaultdict(list)

    for ts, src, dst, sport, dport, payload in udp_datagrams(path):
        if not is_rtcp(payload):
            continue
        for pt, subtype, sub in rtcp_subpackets(payload):
            census[pt] += 1
            if pt == 204 and len(sub) >= 12:      # APP
                name = sub[8:12]
                apps[name].append((ts, subtype, sub, src, dst, sport, dport))

    print(f"=== {path} ===")
    names = {200: "SR", 201: "RR", 202: "SDES", 203: "BYE", 204: "APP",
             205: "RTPFB", 206: "PSFB", 207: "XR"}
    print("RTCP census: " + "  ".join(
        f"{names.get(pt, pt)}({pt}):{n}" for pt, n in sorted(census.items())))

    for name, pkts in sorted(apps.items()):
        label = name.decode("ascii", "replace") if any(name) else f"0x{int.from_bytes(name, 'big'):08x}"
        lens = Counter(len(p[2]) for p in pkts)
        t0, t1 = pkts[0][0], pkts[-1][0]
        rate = len(pkts) / (t1 - t0) if t1 > t0 else 0
        print(f"\n--- APP '{label}'  count={len(pkts)}  "
              f"lengths={dict(lens)}  rate={rate:.1f}/s ---")
        report_app(pkts)


def report_app(pkts):
    """Print the first few bodies, then which byte offsets actually vary."""
    _, subtype, first, *_ = pkts[0]
    print(f"subtype(RC)={subtype}  ssrc=0x{struct.unpack('>I', first[4:8])[0]:08x}")

    for ts, _, sub, *_ in pkts[:4]:
        print(f"  t={ts - pkts[0][0]:7.3f}  {sub.hex()}")

    body_len = len(first)
    if any(len(p[2]) != body_len for p in pkts):
        print("  (mixed lengths — skipping the field walk)")
        return

    # Everything after the 12-byte APP header (V/P/RC, PT, len, SSRC, name).
    print(f"\n  offset  u32be           distinct  behaviour")
    for off in range(12, body_len, 4):
        vals = [struct.unpack(">I", p[2][off:off + 4])[0] for p in pkts]
        distinct = len(set(vals))
        head = vals[0]
        if distinct == 1:
            note = "CONSTANT"
        else:
            deltas = [b - a for a, b in zip(vals, vals[1:])]
            if all(d > 0 for d in deltas):
                note = f"monotonic up, delta {min(deltas)}..{max(deltas)}"
            elif all(d >= 0 for d in deltas):
                note = f"non-decreasing, delta {min(deltas)}..{max(deltas)}"
            else:
                note = f"varies {min(vals)}..{max(vals)}"
        print(f"  +{off:<3}    0x{head:08x} {head:>10}  {distinct:>8}  {note}")


if __name__ == "__main__":
    main()
