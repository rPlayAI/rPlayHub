#!/usr/bin/env python3
"""Extract the screen video from a CoreDevice tunnel pcap → Annex-B .h265/.h264.

Auto-detects the video RTP payload type and the codec (HEVC vs AVC), handles the RTP
extension header, reorders by sequence number, reassembles RFC 7798 (HEVC) / RFC 6184 (AVC),
and strips the displayservice NAL trailer. Then: ffmpeg -i out.h26x out.mp4

Usage: python3 extract_video.py <capture.pcap> <out.h265>
"""
import collections, struct, sys
import decode_devicehub as dd

TRAILER = bytes.fromhex("04f00ac0000003000004ec0ab003")
ANNEXB = b"\x00\x00\x00\x01"


def _strip(n):
    return n[:-len(TRAILER)] if n.endswith(TRAILER) else n


def _rtp(p):
    """Return (pt, seq, payload) with full header (CSRC + extension) stripped, or None."""
    if len(p) < 12 or (p[0] >> 6) != 2:
        return None
    pt = p[1] & 0x7F
    seq = struct.unpack_from(">H", p, 2)[0]
    off = 12 + 4 * (p[0] & 0x0F)
    if p[0] & 0x10 and off + 4 <= len(p):
        off += 4 + 4 * struct.unpack_from(">H", p, off + 2)[0]
    return pt, seq, p[off:]


def depacketize_hevc(payload, fu, out):
    if len(payload) < 2:
        return
    t = (payload[0] >> 1) & 0x3F
    if t == 48:                      # AP
        i = 2
        while i + 2 <= len(payload):
            sz = struct.unpack_from(">H", payload, i)[0]; i += 2
            out.append(_strip(payload[i:i + sz])); i += sz
    elif t == 49:                    # FU
        fuh = payload[2]
        if fuh & 0x80:
            fu[:] = bytes([(payload[0] & 0x81) | ((fuh & 0x3F) << 1), payload[1]]) + payload[3:]
        else:
            fu.extend(payload[3:])
        if (fuh & 0x40) and fu:
            out.append(_strip(bytes(fu))); fu.clear()
    else:
        out.append(_strip(payload))


def depacketize_avc(payload, fu, out):
    if not payload:
        return
    t = payload[0] & 0x1F
    if t == 24:                      # STAP-A
        i = 1
        while i + 2 <= len(payload):
            sz = struct.unpack_from(">H", payload, i)[0]; i += 2
            out.append(_strip(payload[i:i + sz])); i += sz
    elif t == 28:                    # FU-A
        fuh = payload[1]
        if fuh & 0x80:
            fu[:] = bytes([(payload[0] & 0xE0) | (fuh & 0x1F)]) + payload[2:]
        else:
            fu.extend(payload[2:])
        if (fuh & 0x40) and fu:
            out.append(_strip(bytes(fu))); fu.clear()
    else:
        out.append(_strip(payload))


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: python3 extract_video.py <capture.pcap> <out.h265>")
    pcap, outpath = sys.argv[1], sys.argv[2]

    by_pt = collections.defaultdict(list)     # pt -> [(seq,payload)]
    for lt, raw in dd.read_pcap(pcap):
        r = dd.ip_payload(dd.link_to_ip(lt, raw))
        if not r:
            continue
        proto, l4, src, dst = r
        if proto != 17 or len(l4) < 20:
            continue
        rr = _rtp(l4[8:])
        if not rr:
            continue
        pt, seq, payload = rr
        if 96 <= pt <= 127 and payload:       # dynamic RTP
            by_pt[pt].append((seq, payload))

    if not by_pt:
        sys.exit("no dynamic RTP found")
    # video = the payload type with the most bytes
    vpt = max(by_pt, key=lambda pt: sum(len(p) for _, p in by_pt[pt]))
    pkts = sorted(by_pt[vpt], key=lambda x: x[0])    # REORDER by sequence number
    # detect codec definitively: HEVC parameter/packet NAL types (32=VPS 33=SPS 34=PPS 48=AP 49=FU)
    # vs AVC (7=SPS 8=PPS 5=IDR 24=STAP 28=FU). Vote over a sample.
    hevc_votes = sum(1 for _, p in pkts[:60] if p and (p[0] & 0x80) == 0
                     and ((p[0] >> 1) & 0x3F) in (32, 33, 34, 19, 20, 21, 48, 49, 1))
    avc_votes = sum(1 for _, p in pkts[:60] if p and (p[0] & 0x80) == 0
                    and (p[0] & 0x1F) in (7, 8, 5, 24, 28))
    hevc = hevc_votes >= avc_votes
    depack = depacketize_hevc if hevc else depacketize_avc

    fu, nals = bytearray(), []
    for _seq, payload in pkts:
        depack(payload, fu, nals)
    with open(outpath, "wb") as f:
        for n in nals:
            f.write(ANNEXB + n)

    tf = collections.Counter(((n[0] >> 1) & 0x3F) if hevc else (n[0] & 0x1F) for n in nals if n)
    print(f"video PT={vpt} codec={'HEVC' if hevc else 'AVC'}  {len(pkts)} RTP -> {len(nals)} NALs")
    print(f"NAL types: {dict(tf)}")
    print(f"wrote {outpath}  ->  ffmpeg -i {outpath} {outpath.rsplit('.',1)[0]}.mp4")


if __name__ == "__main__":
    main()
