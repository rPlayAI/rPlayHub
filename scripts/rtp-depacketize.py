#!/usr/bin/env python3
"""An independent HEVC RTP depacketizer, written from RFC 7798, reading a pcap.

This exists to disagree with us. doc/RENDERING-HANDOFF.md records that our C
depacketizer and our Python one agreed exactly -- and that the agreement was
worthless, because both had inherited the same omission (RTP padding was never
stripped, in either). A second opinion only counts if it comes from somewhere
else, so nothing here is derived from core/rp_rtp.c or core/rp_rtp_assembler.c.
It is written from the RFC, and its output is meant to be diffed against theirs
byte for byte.

It reads the ORIGINAL capture. It never reads an assembled .h265, because those
are derived artifacts whose provenance we cannot check.

    python3 scripts/rtp-depacketize.py capture.pcap out.h265
    python3 scripts/rtp-depacketize.py capture.pcap out.h265 --verbose

RFC 7798 payload structures, keyed on the 6-bit type in the 2-byte NAL header:
   48  AP  aggregation packet: [DONL] (u16 size, NAL) repeated
   49  FU  fragmentation unit: PayloadHdr(2) FUHeader(1) [DONL] fragment
   50  PACI (not expected here)
   else    a single NAL unit, carried whole
"""
import argparse
import struct
import sys
from collections import Counter


def pcap_packets(path):
    """Yield UDP payloads from a classic pcap (DLT_NULL, as tcpdump writes for
    a utun)."""
    with open(path, "rb") as f:
        hdr = f.read(24)
        if hdr[:4] == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif hdr[:4] == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            sys.exit("not a classic pcap")
        linktype = struct.unpack(end + "I", hdr[20:24])[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                return
            _, _, caplen, _ = struct.unpack(end + "IIII", ph)
            frame = f.read(caplen)
            if len(frame) < caplen:
                return
            if linktype != 0 or len(frame) < 4:
                continue
            af = struct.unpack(end + "I", frame[:4])[0]
            ip = frame[4:]
            if af == 30 and len(ip) >= 40 and ip[6] == 17:
                rest = ip[40:]
            elif af == 2 and len(ip) >= 20 and ip[9] == 17:
                rest = ip[(ip[0] & 0xF) * 4:]
            else:
                continue
            if len(rest) < 8:
                continue
            ulen = struct.unpack(">H", rest[4:6])[0]
            payload = rest[8:ulen] if 8 <= ulen <= len(rest) else rest[8:]
            if payload:
                yield payload


def parse_rtp(pkt):
    """Return (ssrc, pt, seq, timestamp, marker, payload) or None.

    Handles, because each of these silently corrupts the payload if skipped
    wrongly: the CSRC list, the extension header, and trailing padding.
    """
    if len(pkt) < 12:
        return None
    b0 = pkt[0]
    if (b0 >> 6) != 2:
        return None
    padding = (b0 >> 5) & 1
    extension = (b0 >> 4) & 1
    cc = b0 & 0x0F
    b1 = pkt[1]
    marker = (b1 >> 7) & 1
    pt = b1 & 0x7F
    seq = struct.unpack(">H", pkt[2:4])[0]
    ts = struct.unpack(">I", pkt[4:8])[0]
    ssrc = struct.unpack(">I", pkt[8:12])[0]

    off = 12 + 4 * cc
    if len(pkt) < off:
        return None
    if extension:
        if len(pkt) < off + 4:
            return None
        ext_words = struct.unpack(">H", pkt[off + 2:off + 4])[0]
        off += 4 + 4 * ext_words
        if len(pkt) < off:
            return None

    end = len(pkt)
    if padding:
        if end <= off:
            return None
        pad = pkt[end - 1]
        # A padding count of zero, or one larger than what is present, is not
        # something to guess about -- treat the packet as unusable.
        if pad == 0 or end - pad < off:
            return None
        end -= pad

    return ssrc, pt, seq, ts, marker, pkt[off:end]


class Depacketizer:
    """RFC 7798. Reorders by sequence number first: fragments welded together in
    arrival order are wrong whenever arrival order is not sequence order."""

    def __init__(self, donl=False, verbose=False):
        self.donl = donl
        self.verbose = verbose
        self.nals = []
        self.frag = None          # bytearray of the FU being reassembled
        self.frag_type = None
        self.stats = Counter()
        self.frame_marks = 0

    def feed_sorted(self, packets):
        """packets: list of (extended_seq, marker, payload) already in order."""
        for eseq, marker, payload in packets:
            self._one(payload)
            if marker:
                self.frame_marks += 1

    def _one(self, payload):
        if len(payload) < 2:
            self.stats["short"] += 1
            return
        hdr = struct.unpack(">H", payload[:2])[0]
        nal_type = (hdr >> 9) & 0x3F

        if nal_type == 49:                       # FU
            if len(payload) < 3:
                self.stats["short_fu"] += 1
                return
            fu = payload[2]
            start, end = (fu >> 7) & 1, (fu >> 6) & 1
            real_type = fu & 0x3F
            off = 3 + (2 if self.donl and start else 0)
            frag = payload[off:]
            if start:
                if self.frag is not None:
                    self.stats["fu_restart_without_end"] += 1
                # Rebuild the NAL header with the real type from the FU header.
                new_hdr = (hdr & 0x81FF) | (real_type << 9)
                self.frag = bytearray(struct.pack(">H", new_hdr))
                self.frag_type = real_type
                self.frag += frag
            elif self.frag is not None:
                if real_type != self.frag_type:
                    self.stats["fu_type_changed"] += 1
                    self.frag = None
                    return
                self.frag += frag
            else:
                self.stats["fu_continuation_without_start"] += 1
                return
            if end and self.frag is not None:
                self.nals.append(bytes(self.frag))
                self.frag = None
                self.frag_type = None

        elif nal_type == 48:                     # AP
            off = 2 + (2 if self.donl else 0)
            while off + 2 <= len(payload):
                size = struct.unpack(">H", payload[off:off + 2])[0]
                off += 2
                if size == 0 or off + size > len(payload):
                    self.stats["ap_overrun"] += 1
                    break
                self.nals.append(payload[off:off + size])
                off += size
                if self.donl:
                    off += 1                     # DOND on subsequent units
            self.stats["ap"] += 1

        elif nal_type == 50:
            self.stats["paci_unhandled"] += 1

        else:                                    # single NAL unit
            self.nals.append(bytes(payload))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("out")
    ap.add_argument("--pt", type=int, default=None)
    ap.add_argument("--donl", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    # Census: separate the streams. The capture carries video and audio, and
    # feeding audio into a video depacketizer destroys it.
    streams = {}
    for pkt in pcap_packets(args.pcap):
        if len(pkt) >= 2 and (pkt[0] >> 6) == 2 and 200 <= (pkt[1] & 0x7F) <= 223:
            continue                              # RTCP shares the port
        r = parse_rtp(pkt)
        if not r:
            continue
        ssrc, pt, seq, ts, marker, payload = r
        k = (ssrc, pt)
        s = streams.setdefault(k, {"pkts": [], "bytes": 0})
        s["pkts"].append((seq, ts, marker, payload))
        s["bytes"] += len(payload)

    print("RTP streams:")
    for (ssrc, pt), s in sorted(streams.items(), key=lambda kv: -kv[1]["bytes"]):
        print(f"  ssrc 0x{ssrc:08x} pt {pt:>3}  {len(s['pkts']):>6} packets  "
              f"{s['bytes']:>10} payload bytes")
    if args.pt is not None:
        chosen = [k for k in streams if k[1] == args.pt]
        if not chosen:
            sys.exit(f"no stream with payload type {args.pt}")
        key = chosen[0]
    else:
        key = max(streams, key=lambda k: streams[k]["bytes"])
    print(f"selected ssrc 0x{key[0]:08x} pt {key[1]} "
          f"(chosen by volume; payload type does not identify the codec)")

    pkts = streams[key]["pkts"]

    # Extend sequence numbers across the 16-bit wrap, then sort. Arrival order
    # is not sequence order and reassembling in arrival order is wrong.
    pkts_by_arrival = pkts
    cycles, prev = 0, None
    extended = []
    for seq, ts, marker, payload in pkts_by_arrival:
        if prev is not None:
            if seq < prev - 32768:
                cycles += 1
            elif seq > prev + 32768:
                cycles -= 1
        prev = seq
        extended.append((cycles * 65536 + seq, marker, payload, ts))
    extended.sort(key=lambda x: x[0])

    seqs = [e[0] for e in extended]
    gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if b != a + 1]
    dups = len(seqs) - len(set(seqs))
    print(f"sequence: {len(seqs)} packets, {seqs[-1] - seqs[0] + 1} span, "
          f"{len(gaps)} gaps, {dups} duplicates")
    if gaps:
        print(f"  gaps: {gaps[:10]}")

    d = Depacketizer(donl=args.donl, verbose=args.verbose)
    d.feed_sorted([(e[0], e[1], e[2]) for e in extended])

    with open(args.out, "wb") as f:
        for nal in d.nals:
            f.write(b"\x00\x00\x00\x01" + nal)

    types = Counter((n[0] >> 1) & 0x3F for n in d.nals if len(n) >= 2)
    total = sum(len(n) for n in d.nals)
    print(f"emitted {len(d.nals)} NALs, {total} payload bytes, "
          f"{d.frame_marks} RTP frame markers -> {args.out}")
    print(f"NAL types: {dict(sorted(types.items()))}")
    if d.stats:
        print(f"anomalies: {dict(d.stats)}")
    if d.frag is not None:
        print(f"  ! a fragmented NAL was still open at end of stream "
              f"({len(d.frag)} bytes) -- dropped")


if __name__ == "__main__":
    main()
