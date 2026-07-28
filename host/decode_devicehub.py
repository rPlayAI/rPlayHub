#!/usr/bin/env python3
"""Decode an Apple Device Hub "View Screen" capture (tcpdump on the CoreDevice utun).

The tunnel is cleartext IPv6: RemoteXPC over HTTP/2 (TCP) + RTP/RTCP (UDP). This:
  - reassembles TCP, scans for XPC wrappers (magic 0x29B00B92), decodes them (xpc.py)
  - finds the `com.apple.coredevice.feature.startmediastream` invocation + the device answer
  - decompresses `avcMediaStreamNegotiatorMediaBlob` (zlib) and walks the protobuf
  - dumps every RTCP packet the receiver/device sends (RR / PLI / FIR / LTR-ACK)

Capture first (see capture-devicehub.sh), then:  python3 decode_devicehub.py devicehub.pcap
No deps beyond stdlib + local xpc.py.
"""
import plistlib, struct, sys, zlib

import xpc

WRAPPER_MAGIC = b"\x92\x0b\xb0\x29"   # 0x29B00B92 little-endian


# ---------------- pcap ----------------
def read_pcap(path):
    d = open(path, "rb").read()
    magic = d[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"):
        le = magic == b"\xd4\xc3\xb2\xa1"
        end = "<" if le else ">"
        linktype = struct.unpack_from(end + "I", d, 20)[0]
        off = 24
        while off + 16 <= len(d):
            _s, _u, incl, _orig = struct.unpack_from(end + "IIII", d, off)
            off += 16
            yield linktype, d[off:off + incl]
            off += incl
    else:
        sys.exit("not a classic pcap (pcapng not supported — use tcpdump -w, not tshark)")


def link_to_ip(linktype, data):
    if linktype == 0:                 # DLT_NULL — 4-byte protocol family
        return data[4:]
    if linktype in (12, 14, 101):     # DLT_RAW
        return data
    if linktype == 1:                 # Ethernet
        return data[14:]
    return data


def ip_payload(data):
    """Return (proto, l4payload, src, dst) for IPv4/IPv6, else None."""
    if not data:
        return None
    v = data[0] >> 4
    if v == 6 and len(data) >= 40:
        proto = data[6]
        src = data[8:24].hex(); dst = data[24:40].hex()
        return proto, data[40:], src, dst
    if v == 4 and len(data) >= 20:
        ihl = (data[0] & 0xF) * 4
        return data[9], data[ihl:], data[12:16].hex(), data[16:20].hex()
    return None


# ---------------- TCP reassembly (per stream, by seq) ----------------
def reassemble_tcp(packets):
    streams = {}   # (src,dst,sport,dport) -> {seq: payload}
    for proto, l4, src, dst in packets:
        if proto != 6 or len(l4) < 20:
            continue
        sport, dport = struct.unpack_from(">HH", l4, 0)
        seq = struct.unpack_from(">I", l4, 4)[0]
        off = (l4[12] >> 4) * 4
        payload = l4[off:]
        if not payload:
            continue
        streams.setdefault((src, dst, sport, dport), {})[seq] = payload
    out = []
    for key, segs in streams.items():
        buf = b""
        for seq in sorted(segs):
            buf += segs[seq]
        out.append((key, buf))
    return out


HTTP2_MAGIC = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

def http2_data(tcp_buf):
    """Reassemble HTTP/2 DATA frame payloads per h2 stream_id (strips the 9-byte
    frame headers so XPC wrappers spanning multiple frames reassemble)."""
    i = len(HTTP2_MAGIC) if tcp_buf[:len(HTTP2_MAGIC)] == HTTP2_MAGIC else 0
    streams = {}
    while i + 9 <= len(tcp_buf):
        length = int.from_bytes(tcp_buf[i:i + 3], "big")
        ftype = tcp_buf[i + 3]
        sid = int.from_bytes(tcp_buf[i + 5:i + 9], "big") & 0x7FFFFFFF
        payload = tcp_buf[i + 9:i + 9 + length]
        if len(payload) < length:
            break
        if ftype == 0:  # DATA
            streams[sid] = streams.get(sid, b"") + payload
        i += 9 + length
    return streams


# ---------------- XPC scan ----------------
def scan_xpc(buf):
    dicts, i = [], 0
    while True:
        i = buf.find(WRAPPER_MAGIC, i)
        if i < 0:
            break
        try:
            _f, _m, obj = xpc.parse_wrapper(buf[i:])
            if isinstance(obj, dict) and obj:
                dicts.append(obj)
        except Exception:
            pass
        i += 4
    return dicts


# ---------------- protobuf walk ----------------
def walk_protobuf(data, indent="    "):
    out, i = [], 0
    while i < len(data):
        try:
            tag, i = _varint(data, i)
        except Exception:
            break
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            val, i = _varint(data, i); out.append(f"{indent}f{field} varint = {val}")
        elif wire == 2:
            ln, i = _varint(data, i); chunk = data[i:i + ln]; i += ln
            if _printable(chunk):
                out.append(f"{indent}f{field} str = {chunk.decode()!r}")
            elif _is_protobuf(chunk):
                out.append(f"{indent}f{field} len={ln} {{")
                out += walk_protobuf(chunk, indent + "  ")
                out.append(f"{indent}}}")
            else:
                out.append(f"{indent}f{field} bytes[{ln}] = {chunk[:48].hex()}")
        elif wire == 5:
            out.append(f"{indent}f{field} i32 = {struct.unpack_from('<I', data, i)[0]}"); i += 4
        elif wire == 1:
            out.append(f"{indent}f{field} i64 = {struct.unpack_from('<Q', data, i)[0]}"); i += 8
        else:
            break
    return out


def _varint(d, i):
    r = s = 0
    while True:
        b = d[i]; i += 1; r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, i
        s += 7


def _printable(b):
    return len(b) >= 2 and all(32 <= c < 127 for c in b)


def _is_protobuf(d):
    """True if d parses cleanly as protobuf, consuming exactly all bytes."""
    if not d:
        return False
    i, n = 0, 0
    try:
        while i < len(d):
            tag, i = _varint(d, i)
            field, wire = tag >> 3, tag & 7
            if field == 0 or wire in (3, 4, 6, 7):
                return False
            if wire == 0:
                _, i = _varint(d, i)
            elif wire == 2:
                ln, i = _varint(d, i); i += ln
            elif wire == 5:
                i += 4
            elif wire == 1:
                i += 8
            n += 1
        return i == len(d) and n > 0
    except Exception:
        return False


# ---------------- RTCP ----------------
RTCP_PT = {200: "SR", 201: "RR", 202: "SDES", 203: "BYE", 204: "APP", 205: "RTPFB", 206: "PSFB"}
FB_FMT = {(205, 1): "NACK", (206, 1): "PLI", (206, 4): "FIR", (206, 2): "SLI", (206, 15): "AFB/app(LTR?)"}


def dump_rtcp(udp_l4payloads):
    seen = {}
    for pl in udp_l4payloads:
        # UDP: 8-byte header then payload
        if len(pl) < 12:
            continue
        p = pl[8:]
        if (p[0] >> 6) != 2 or not (200 <= p[1] <= 223):
            continue   # RTCP PT range only — skip RTP datagrams
        while len(p) >= 4 and (p[0] >> 6) == 2 and 200 <= p[1] <= 223:
            pt = p[1]; fmt = p[0] & 0x1F
            length = (struct.unpack_from(">H", p, 2)[0] + 1) * 4
            name = RTCP_PT.get(pt, f"PT{pt}")
            if pt in (205, 206):
                name += ":" + FB_FMT.get((pt, fmt), f"fmt{fmt}")
            seen[name] = seen.get(name, 0) + 1
            p = p[length:]
    return seen


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python3 decode_devicehub.py <capture.pcap>")
    packets, udp_payloads = [], []
    for linktype, raw in read_pcap(sys.argv[1]):
        r = ip_payload(link_to_ip(linktype, raw))
        if not r:
            continue
        proto, l4, src, dst = r
        packets.append(r)
        if proto == 17:
            udp_payloads.append(l4)

    dicts = []
    for _key, buf in reassemble_tcp(packets):
        for _sid, data in http2_data(buf).items():   # strip HTTP/2 framing first
            dicts += scan_xpc(data)
        dicts += scan_xpc(buf)                        # fallback: also scan raw
    print(f"decoded {len(dicts)} XPC dicts; {len(udp_payloads)} UDP datagrams\n")

    start = [d for d in dicts if d.get("CoreDevice.featureIdentifier", "").endswith("startmediastream")]
    answers = [d for d in dicts if "CoreDevice.output" in d and "connection" in d.get("CoreDevice.output", {})]

    for d in start:
        print("=" * 70, "\nSTARTMEDIASTREAM INVOCATION")
        inp = d.get("CoreDevice.input", {})
        print("  clientSupportedFeatures:", inp.get("clientSupportedFeatures"))
        print("  timeout:", inp.get("timeout"), " direction:", inp.get("direction"), " type:", inp.get("type"))
        print("  options:")
        for k, v in (inp.get("options") or {}).items():
            print(f"    {k} = {v}")
        blob = inp.get("negotiatorOffer")
        if isinstance(blob, (bytes, bytearray)):
            try:
                pl = plistlib.loads(bytes(blob))
                print("  negotiatorOffer (bplist) keys:", sorted(pl))
                mb = pl.get("avcMediaStreamNegotiatorMediaBlob")
                print("  negotiatorMode:", pl.get("avcMediaStreamNegotiatorMode"))
                if mb:
                    proto = zlib.decompress(mb)
                    print(f"\n  --- mediaBlob protobuf ({len(proto)} bytes decompressed) ---")
                    print("\n".join(walk_protobuf(proto)))
            except Exception as e:
                print("  (offer parse failed:", e, ")")

    for d in answers:
        out = d["CoreDevice.output"]
        cfg = out.get("connection", {}).get("streamConfig", {})
        print("=" * 70, "\nDEVICE ANSWER streamConfig:")
        for k, v in cfg.items():
            print(f"    {k} = {v}")

    print("=" * 70, "\nRTCP the receiver/device sent:")
    for name, n in sorted(dump_rtcp(udp_payloads).items(), key=lambda x: -x[1]):
        print(f"    {name}: {n}")


if __name__ == "__main__":
    main()
