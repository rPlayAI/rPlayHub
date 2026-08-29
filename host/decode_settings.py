#!/usr/bin/env python3
"""Decode a Device Hub Settings-tab capture — what it READS and what it WRITES.

Capture first with host/capture-settings.sh (tcpdump on the CoreDevice tunnel utun), then:

    python3 host/decode_settings.py settings-*.pcap
    python3 host/decode_settings.py settings-*.pcap --all     # every XPC dict, not just hits

Why the tunnel and not usbmux: checked live against Device Hub with a device connected,
DeviceHub.app holds ZERO connections to /var/run/usbmuxd — every device connection is TCP over
IPv6 to the tunnel's ULA address. usbmux only brings the tunnel up. See capture-settings.sh.

The point of this over decode_devicehub.py: that one hunts for one known invocation
(startmediastream). Here we do NOT yet know the feature identifiers, so this prints every
CoreDevice invocation it can find, in time order, with direction and arguments — which is what
identifies both the read call (populating the tab) and the write call (changing a setting).
"""
import struct
import sys

import xpc
from decode_devicehub import (WRAPPER_MAGIC, http2_data, ip_payload, link_to_ip,
                              read_pcap, scan_xpc)

# Substrings that mark a dict as interesting. Deliberately broad: the real identifiers are what
# we are trying to LEARN, so anything mentioning a settings-ish concept is worth printing.
HINTS = ("appearance", "accessibility", "voiceover", "contrast", "motion", "transparen",
         "textsize", "text_size", "dynamictype", "colorfilter", "color_filter", "border",
         "liquid", "glass", "location", "simulatelocation", "setting", "preference",
         "feature.", "coredevice.feature")


def flatten(obj, prefix=""):
    """Yield (path, value) for every scalar in a nested dict/list."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield from flatten(v, f"{prefix}.{k}" if prefix else str(k))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from flatten(v, f"{prefix}[{i}]")
    else:
        yield prefix, obj


def interesting(obj):
    blob = " ".join(f"{p} {v}" for p, v in flatten(obj)).lower()
    return [h for h in HINTS if h in blob]


def describe(obj, indent="    "):
    lines = []
    for path, val in flatten(obj):
        if isinstance(val, bytes):
            val = val[:64].hex() + ("…" if len(val) > 64 else "")
        s = f"{indent}{path} = {val!r}"
        lines.append(s if len(s) < 300 else s[:300] + "…")
    return lines


def main(argv):
    paths = [a for a in argv[1:] if not a.startswith("-")]
    show_all = "--all" in argv
    if not paths:
        sys.exit(__doc__)

    for path in paths:
        print(f"=== {path} ===")
        packets = []
        for linktype, raw in read_pcap(path):
            r = ip_payload(link_to_ip(linktype, raw))
            if r:
                packets.append(r)

        # Per-stream, in capture order. Direction matters: the host->device dict is the request
        # that CHANGES a setting, and that is the one we ultimately have to reproduce.
        streams = {}
        for proto, l4, src, dst in packets:
            if proto != 6 or len(l4) < 20:
                continue
            sport, dport = struct.unpack_from(">HH", l4, 0)
            seq = struct.unpack_from(">I", l4, 4)[0]
            off = (l4[12] >> 4) * 4
            payload = l4[off:]
            if payload:
                streams.setdefault((src, dst, sport, dport), {})[seq] = payload

        total = hits = 0
        for (src, dst, sport, dport), segs in streams.items():
            buf = b"".join(segs[s] for s in sorted(segs))
            # XPC rides HTTP/2 DATA frames; also scan the raw buffer for streams that are not h2.
            candidates = list(http2_data(buf).values()) or []
            if WRAPPER_MAGIC in buf:
                candidates.append(buf)
            for blob in candidates:
                for obj in scan_xpc(blob):
                    total += 1
                    marks = interesting(obj)
                    if not (marks or show_all):
                        continue
                    hits += 1
                    # Device ports are the RSD service block; the host side is ephemeral.
                    arrow = f"{sport} -> {dport}"
                    print(f"\n[{arrow}] {'hints: ' + ','.join(marks) if marks else ''}")
                    for line in describe(obj):
                        print(line)

        print(f"\n{hits} of {total} XPC dicts matched"
              f"{' (--all: everything shown)' if show_all else ''}")
        if not total:
            print("No XPC dicts found. Was the capture on the tunnel utun (mtu 16000, inet6 fd..)?")


if __name__ == "__main__":
    main(sys.argv)
