#!/usr/bin/env python3
"""Recover the RemotePairing control-channel framing from a LAN capture.

doc/REMOTEPAIRING-PROTOCOL.md has the message *set* (recovered from a symbol
dump) but not the bytes, and the device says nothing until the host speaks, so
the framing has to come from watching Device Hub do it. The control channel is
plain until authentication completes, so the handshake and the pairing exchange
are readable.

Rather than assume a header shape, this finds the framing empirically: it walks
each reassembled TCP stream looking for offsets where an OPACK object decodes
and consumes exactly the bytes a length field says it should. The header length
and the position of the length field fall out of that.

    python3 scripts/decode-remotepairing.py build/remotepairing-*.pcap
"""
import struct
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import opack


def packets(path):
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
            sec, usec, caplen, _ = struct.unpack(end + "IIII", ph)
            d = f.read(caplen)
            if len(d) < caplen:
                return
            yield link, sec + usec / 1e6, d


def tcp_streams(path):
    """Reassemble TCP payloads per direction, ordered by sequence number."""
    segs = defaultdict(dict)
    meta = {}
    for link, ts, frame in packets(path):
        if link == 1:                      # Ethernet
            if len(frame) < 14:
                continue
            etype = struct.unpack(">H", frame[12:14])[0]
            ip = frame[14:]
        elif link == 0:                    # DLT_NULL
            if len(frame) < 4:
                continue
            etype = 0x86DD if struct.unpack("<I", frame[:4])[0] == 30 else 0x0800
            ip = frame[4:]
        else:
            continue

        if etype == 0x86DD and len(ip) >= 40:
            if ip[6] != 6:
                continue
            src, dst = ip[8:24], ip[24:40]
            rest = ip[40:]
        elif etype == 0x0800 and len(ip) >= 20:
            if ip[9] != 6:
                continue
            ihl = (ip[0] & 0xF) * 4
            src, dst = ip[12:16], ip[16:20]
            total = struct.unpack(">H", ip[2:4])[0]
            rest = ip[ihl:total] if total >= ihl else ip[ihl:]
        else:
            continue
        if len(rest) < 20:
            continue
        sport, dport = struct.unpack(">HH", rest[:4])
        seq = struct.unpack(">I", rest[4:8])[0]
        off = (rest[12] >> 4) * 4
        payload = rest[off:]
        if not payload:
            continue
        key = (src, sport, dst, dport)
        segs[key][seq] = payload
        meta.setdefault(key, ts)

    out = {}
    for key, byseq in segs.items():
        data = b""
        for s in sorted(byseq):
            data += byseq[s]
        out[key] = data
    return out, meta


def ipstr(b):
    if len(b) == 4:
        return ".".join(str(x) for x in b)
    return ":".join(f"{b[i]:02x}{b[i+1]:02x}" for i in range(0, 16, 2))


def find_framing(data):
    """Return a list of (header_bytes, opack_object) by trying header lengths."""
    best = None
    for hdr_len in range(0, 33):
        for lensize, lefmt in ((2, ">H"), (4, ">I"), (2, "<H"), (4, "<I")):
            for lenpos in range(0, max(1, hdr_len - lensize + 1)):
                msgs, off, ok = [], 0, True
                while off + hdr_len <= len(data):
                    body_len = struct.unpack_from(
                        lefmt, data, off + lenpos)[0] if hdr_len else 0
                    if hdr_len == 0 or body_len <= 0 or off + hdr_len + body_len > len(data):
                        ok = False
                        break
                    body = data[off + hdr_len:off + hdr_len + body_len]
                    if not opack.looks_like_opack(body):
                        ok = False
                        break
                    msgs.append((data[off:off + hdr_len], opack.decode(body)[0]))
                    off += hdr_len + body_len
                    if off == len(data):
                        break
                if ok and msgs and off == len(data):
                    cand = (len(msgs), hdr_len, lensize, lefmt, lenpos, msgs)
                    if best is None or cand[0] > best[0]:
                        best = cand
    return best


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: decode-remotepairing.py <capture.pcap>")
    streams, meta = tcp_streams(sys.argv[1])
    print(f"{len(streams)} TCP directions with payload\n")
    for key, data in sorted(streams.items(), key=lambda kv: -len(kv[1])):
        src, sp, dst, dp = key
        print(f"=== {ipstr(src)}:{sp} -> {ipstr(dst)}:{dp}   {len(data)} bytes ===")
        print(f"    first 48: {data[:48].hex()}")
        print(f"    ascii   : " +
              "".join(chr(b) if 32 <= b < 127 else "." for b in data[:48]))
        best = find_framing(data)
        if not best:
            print("    (no consistent OPACK framing found -- may be encrypted,"
                  " or not this protocol)\n")
            continue
        n, hdr_len, lensize, lefmt, lenpos, msgs = best
        print(f"    FRAMING: {hdr_len}-byte header, "
              f"{lensize}-byte {'big' if '>' in lefmt else 'little'}-endian "
              f"length at offset {lenpos}; {n} messages")
        for h, obj in msgs[:6]:
            print(f"      header {h.hex()}")
            print("      " + opack.pretty(obj, 3).replace("\n", "\n      "))
        print()


if __name__ == "__main__":
    main()
