#!/usr/bin/env python3
"""Decode a CoreDevice tunnel capture into the XPC messages Apple actually sent.

    python3 scripts/decode-remotexpc.py capture.pcap [--dump-blobs]

The CoreDevice tunnel carries RSD and every coredevice.* service in CLEARTEXT: HTTP/2 frames whose
DATA payloads are XPC objects. So a tcpdump on Apple's utun while Device Hub mirrors a screen
contains the real `startmediastream` offer — the one our own offer has only imitated from a
symbol-dump RE.

Pipeline: pcap → BSD-loopback framing → IPv6/TCP → per-stream reassembly → HTTP/2 frames → XPC.
The XPC codec is the project's own, already verified byte-for-byte against a real device.

--dump-blobs writes any avcMediaStreamNegotiatorMediaBlob to disk, zlib-inflated, since that blob
is the actual prize: codec banks, bitrate tiers, feature strings and the rtcpPSFB_* flags.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "host"))
from rplayhub.wire import xpc  # noqa: E402

HTTP2_PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"


def read_pcap(path):
    """Yield raw link-layer frames. Handles both byte orders and pcap-ng's absence."""
    with open(path, "rb") as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            return
        magic = hdr[:4]
        if magic == b"\xa1\xb2\xc3\xd4":
            endian = ">"
        elif magic == b"\xd4\xc3\xb2\xa1":
            endian = "<"
        elif magic == b"\x0a\x0d\x0d\x0a":
            raise SystemExit("this is a pcap-ng file; capture with `tcpdump -w` (classic pcap)")
        else:
            raise SystemExit(f"not a pcap file (magic {magic.hex()})")
        linktype = struct.unpack(endian + "I", hdr[20:24])[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            _s, _us, incl, _orig = struct.unpack(endian + "IIII", ph)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield linktype, data


def ip_payload(linktype, frame):
    """Strip link framing and return (src, dst, protocol, payload) for IPv6/IPv4."""
    if linktype == 0:            # BSD loopback / utun: 4-byte address family
        if len(frame) < 4:
            return None
        frame = frame[4:]
    elif linktype == 1:          # Ethernet
        if len(frame) < 14:
            return None
        frame = frame[14:]
    if not frame:
        return None
    version = frame[0] >> 4
    if version == 6 and len(frame) >= 40:
        nxt = frame[6]
        return frame[8:24], frame[24:40], nxt, frame[40:]
    if version == 4 and len(frame) >= 20:
        ihl = (frame[0] & 0x0F) * 4
        return frame[12:16], frame[16:20], frame[9], frame[ihl:]
    return None


def tcp_streams(path):
    """Reassemble TCP payloads per direction, ordered by sequence number."""
    segs = {}
    for linktype, frame in read_pcap(path):
        parsed = ip_payload(linktype, frame)
        if not parsed:
            continue
        src, dst, proto, payload = parsed
        if proto != 6 or len(payload) < 20:
            continue
        sport, dport = struct.unpack(">HH", payload[:4])
        seq = struct.unpack(">I", payload[4:8])[0]
        off = (payload[12] >> 4) * 4
        body = payload[off:]
        if not body:
            continue
        key = (src, sport, dst, dport)
        segs.setdefault(key, {})[seq] = body

    for key, by_seq in segs.items():
        out, expected = bytearray(), None
        for seq in sorted(by_seq):
            chunk = by_seq[seq]
            if expected is not None and seq < expected:
                # Retransmission or overlap: keep only what is new.
                skip = expected - seq
                if skip >= len(chunk):
                    continue
                chunk = chunk[skip:]
            out += chunk
            expected = seq + len(by_seq[seq])
        yield key, bytes(out)


def http2_data(stream_bytes):
    """Yield (http2_stream_id, data_payload) for every DATA frame."""
    buf = stream_bytes
    if buf.startswith(HTTP2_PREFACE):
        buf = buf[len(HTTP2_PREFACE):]
    off = 0
    while off + 9 <= len(buf):
        length = int.from_bytes(buf[off:off + 3], "big")
        ftype = buf[off + 3]
        sid = int.from_bytes(buf[off + 5:off + 9], "big") & 0x7FFFFFFF
        payload = buf[off + 9:off + 9 + length]
        if len(payload) < length:
            return
        if ftype == 0x0 and payload:
            yield sid, payload
        off += 9 + length


def xpc_objects(data_by_stream):
    """Parse XPC wrappers out of concatenated DATA payloads."""
    for sid, blob in data_by_stream.items():
        off = 0
        while off + 24 <= len(blob):
            if int.from_bytes(blob[off:off + 4], "little") != xpc.WRAPPER_MAGIC:
                off += 1
                continue
            plen = int.from_bytes(blob[off + 8:off + 16], "little")
            total = 16 + 8 + max(0, plen - 8) if plen else 24
            total = 16 + 8 + (plen - 8 if plen >= 8 else 0)
            end = off + (24 if not plen else 16 + 8 + (plen - 8) + 8)
            try:
                _flags, mid, obj = xpc.parse_wrapper(blob[off:])
            except Exception:
                off += 4
                continue
            if obj is not None:
                yield sid, mid, obj
            off += max(24, total if total > 24 else 24)
            # Advance past this message; lengths vary, so resync on the next magic.
            nxt = blob.find(xpc.WRAPPER_MAGIC.to_bytes(4, "little"), off)
            off = nxt if nxt > 0 else len(blob)


def summarise(obj, indent="    "):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if isinstance(v, (dict, list)):
                print(f"{indent}{k}:")
                summarise(v, indent + "  ")
            elif isinstance(v, bytes):
                print(f"{indent}{k}: <{len(v)} bytes> {v[:24].hex()}")
            else:
                print(f"{indent}{k}: {v}")
    elif isinstance(obj, list):
        for v in obj[:8]:
            summarise(v, indent + "  ")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    dump = "--dump-blobs" in sys.argv

    total_msgs = 0
    interesting = 0
    for key, stream in tcp_streams(path):
        src, sport, dst, dport = key
        data = {}
        for sid, payload in http2_data(stream):
            data[sid] = data.get(sid, b"") + payload
        if not data:
            continue
        for sid, mid, obj in xpc_objects(data):
            total_msgs += 1
            if not isinstance(obj, dict):
                continue
            feature = obj.get("CoreDevice.featureIdentifier")
            keys = list(obj)
            label = feature or ("handshake" if "MessageType" in obj else None)
            if label is None and "CoreDevice.output" not in obj:
                continue
            interesting += 1
            print(f"\n=== port {sport} -> {dport}  http2 stream {sid}  msg {mid}")
            print(f"    {label or 'response'}   keys: {keys[:8]}")
            summarise(obj)

            blob = None
            inp = obj.get("CoreDevice.input")
            if isinstance(inp, dict):
                offer = inp.get("negotiatorOffer")
                if isinstance(offer, bytes):
                    print(f"    negotiatorOffer: {len(offer)} bytes (binary plist)")
                    if dump:
                        out = f"{path}.offer.{mid}.plist"
                        open(out, "wb").write(offer)
                        print(f"      wrote {out}")
                    try:
                        import plistlib
                        pl = plistlib.loads(offer)
                        for k, v in pl.items():
                            if isinstance(v, bytes):
                                print(f"      {k}: <{len(v)} bytes>")
                                if "MediaBlob" in k:
                                    blob = v
                            else:
                                print(f"      {k}: {v}")
                    except Exception as e:
                        print(f"      (could not parse plist: {e})")
            if blob is not None:
                try:
                    raw = zlib.decompress(blob)
                    print(f"      mediaBlob inflates to {len(raw)} bytes of protobuf")
                    if dump:
                        out = f"{path}.mediablob.{mid}.bin"
                        open(out, "wb").write(raw)
                        print(f"      wrote {out}  <-- THE OFFER WE HAVE BEEN GUESSING AT")
                except Exception as e:
                    print(f"      (mediaBlob did not inflate: {e})")

    print(f"\n{total_msgs} XPC messages parsed, {interesting} interesting")
    if not total_msgs:
        print("No XPC found. If the capture is from a QUIC/RemotePairing tunnel rather than the")
        print("CoreDeviceProxy one, it is encrypted and this will not work.")


if __name__ == "__main__":
    main()
