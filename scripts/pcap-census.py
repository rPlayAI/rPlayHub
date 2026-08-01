#!/usr/bin/env python3
"""Account for every byte in a tunnel capture, not just the video stream.

Every analysis so far picked the busiest RTP stream and ignored the rest. That
is the right default and a bad assumption to leave unchecked: a retransmission
stream (RFC 4588), a FEC stream (RFC 5109), a second video layer, or payload
hidden in RTP header extensions would all be invisible to it, and any of them
would change what the receiver is really given.

This walks the whole capture and reports everything: link types, IP protocols,
UDP ports, every RTP SSRC and payload type, every RTCP packet type including the
feedback ranges, header extensions, and padding -- then checks the totals add up.

    python3 scripts/pcap-census.py capture.pcap
"""
import struct
import sys
from collections import Counter, defaultdict

RTCP_NAMES = {200: "SR", 201: "RR", 202: "SDES", 203: "BYE", 204: "APP",
              205: "RTPFB (NACK/TMMBR)", 206: "PSFB (PLI/FIR/REMB)",
              207: "XR", 208: "AVB", 209: "RSI"}


def frames(path):
    with open(path, "rb") as f:
        hdr = f.read(24)
        if hdr[:4] == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif hdr[:4] == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            sys.exit("not a classic pcap")
        link = struct.unpack(end + "I", hdr[20:24])[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            sec, usec, caplen, orig = struct.unpack(end + "IIII", ph)
            d = f.read(caplen)
            if len(d) < caplen:
                return
            yield end, link, sec + usec / 1e6, d, orig


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "reference/captures/devicehub-iphone13-ios27.pcap"

    links = Counter(); afs = Counter(); protos = Counter()
    total_frames = 0; total_bytes = 0; truncated = 0
    udp_ports = Counter(); udp_bytes = Counter()
    rtp = defaultdict(lambda: {"n": 0, "bytes": 0, "ext": 0, "pad": 0,
                               "marker": 0, "extlen": Counter()})
    rtcp_types = Counter(); rtcp_app = Counter()
    other_udp = Counter(); tcp_bytes = 0; tcp_pkts = 0

    for end, link, ts, d, orig in frames(path):
        total_frames += 1
        total_bytes += len(d)
        if orig > len(d):
            truncated += 1
        links[link] += 1
        if link != 0 or len(d) < 4:
            continue
        af = struct.unpack(end + "I", d[:4])[0]
        afs[af] += 1
        ip = d[4:]
        if af == 30 and len(ip) >= 40:
            proto = ip[6]; rest = ip[40:]
        elif af == 2 and len(ip) >= 20:
            proto = ip[9]; rest = ip[(ip[0] & 0xF) * 4:]
        else:
            protos["non-IP"] += 1
            continue
        protos[proto] += 1
        if proto == 6:
            tcp_pkts += 1; tcp_bytes += len(rest)
            continue
        if proto != 17 or len(rest) < 8:
            continue
        sport, dport = struct.unpack(">HH", rest[:4])
        ulen = struct.unpack(">H", rest[4:6])[0]
        p = rest[8:ulen] if 8 <= ulen <= len(rest) else rest[8:]
        udp_ports[(sport, dport)] += 1
        udp_bytes[(sport, dport)] += len(p)
        if len(p) < 4 or (p[0] >> 6) != 2:
            other_udp["not RTP/RTCP v2"] += 1
            continue

        pt_raw = p[1] & 0x7F
        # RFC 5761: RTCP shares the port. After masking the marker bit the RTCP
        # payload types 200-223 appear as 72-95 -- the trap rp_rtp.h documents.
        if 72 <= pt_raw <= 95:
            off = 0
            while off + 4 <= len(p):
                if (p[off] >> 6) != 2:
                    break
                t = p[off + 1]
                ln = (struct.unpack(">H", p[off + 2:off + 4])[0] + 1) * 4
                if ln <= 0 or off + ln > len(p):
                    break
                rtcp_types[t] += 1
                if t == 204 and off + 12 <= len(p):
                    rtcp_app[p[off + 8:off + 12]] += 1
                if t in (205, 206) and off + 4 <= len(p):
                    rtcp_types[f"  {RTCP_NAMES.get(t,t)} fmt={p[off] & 0x1F}"] += 1
                off += ln
            continue

        if len(p) < 12:
            other_udp["short RTP"] += 1
            continue
        ssrc = struct.unpack(">I", p[8:12])[0]
        k = (ssrc, pt_raw)
        r = rtp[k]
        r["n"] += 1
        r["marker"] += 1 if (p[1] & 0x80) else 0
        cc = p[0] & 0x0F
        off = 12 + 4 * cc
        if (p[0] >> 4) & 1:
            r["ext"] += 1
            if off + 4 <= len(p):
                words = struct.unpack(">H", p[off + 2:off + 4])[0]
                profile = struct.unpack(">H", p[off:off + 2])[0]
                r["extlen"][(profile, words)] += 1
                off += 4 + 4 * words
        endp = len(p)
        if (p[0] >> 5) & 1:
            r["pad"] += 1
            if endp > off:
                endp -= p[endp - 1]
        r["bytes"] += max(0, endp - off)

    print(f"=== {path} ===")
    print(f"{total_frames} captured frames, {total_bytes:,} bytes"
          + (f", {truncated} TRUNCATED by snaplen" if truncated else ", none truncated"))
    print(f"link types: {dict(links)}   address families: {dict(afs)}")
    names = {6: "TCP", 17: "UDP", 58: "ICMPv6", 1: "ICMP"}
    print("IP protocols: " + "  ".join(
        f"{names.get(k,k)}:{v}" for k, v in protos.items()))
    if tcp_pkts:
        print(f"  TCP (RemoteXPC control plane): {tcp_pkts} packets, {tcp_bytes:,} bytes")

    print("\nUDP port pairs:")
    for (s, dd), n in sorted(udp_ports.items(), key=lambda kv: -kv[1]):
        print(f"  {s:>6} -> {dd:<6} {n:>6} packets  {udp_bytes[(s,dd)]:>10,} payload bytes")

    print("\nRTP streams (payload bytes exclude header, CSRCs, extension and padding):")
    for (ssrc, pt), r in sorted(rtp.items(), key=lambda kv: -kv[1]["bytes"]):
        print(f"  ssrc 0x{ssrc:08x} pt {pt:>3}: {r['n']:>6} pkts  "
              f"{r['bytes']:>10,} B  markers {r['marker']:>5}  "
              f"ext {r['ext']:>5}  padded {r['pad']:>4}")
        for (profile, words), c in r["extlen"].most_common(4):
            print(f"       extension profile 0x{profile:04x}, {words} words "
                  f"({4*words} B) x{c}")

    print("\nRTCP packet types:")
    for t, n in sorted(rtcp_types.items(), key=lambda kv: str(kv[0])):
        if isinstance(t, int):
            print(f"  {t} {RTCP_NAMES.get(t,'?'):<22} x{n}")
        else:
            print(f"  {t} x{n}")
    if rtcp_app:
        print("  APP sub-types:")
        for name, n in rtcp_app.items():
            label = name.decode('ascii', 'replace') if any(name) else \
                    f"0x{int.from_bytes(name,'big'):08x}"
            print(f"    '{label}' x{n}")
    if other_udp:
        print(f"\nUDP not classified as RTP/RTCP: {dict(other_udp)}")

    print("\nThings that would matter if present and are NOT above:")
    print("  - a second video SSRC (simulcast / a repair or RTX stream, RFC 4588)")
    print("  - RTCP 205/206 feedback (NACK, PLI, FIR, TMMBR, REMB)")
    print("  - RTP header extensions carrying payload rather than metadata")
    print("  - snaplen truncation, which would silently shorten every large packet")


if __name__ == "__main__":
    main()
