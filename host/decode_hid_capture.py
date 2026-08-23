#!/usr/bin/env python3
"""Pull every HID report Device Hub sent out of a tunnel capture.

Companion to capture-devicehub.sh / decode_devicehub.py, for the input side: scans the TCP
streams for RemoteXPC wrappers and prints each universalhidservice `send` (report bytes + surface
id) in order, with timestamps, so a keyboard or button session can be read off directly.

    python3 host/decode_hid_capture.py devicehub-*.pcap [--surface 512]

Why: the keyboard surface (512) takes Apple "gesture" report IDs, not USB HID reports; dtuhidd
decodes them and logs "No gesture for report ID n" for the wrong ones. Probing from our side
established which IDs decode (2,3,4,7,14-17 with 16 bytes) but not which one types what
(2026-08-23). One capture of Device Hub typing answers it.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import xpc
from decode_devicehub import read_pcap, reassemble_tcp, http2_data, scan_xpc, ip_payload, link_to_ip  # noqa: E402

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    want = None
    if "--surface" in sys.argv:
        want = int(sys.argv[sys.argv.index("--surface") + 1])
    packets = []
    for linktype, raw in read_pcap(sys.argv[1]):
        r = ip_payload(link_to_ip(linktype, raw))
        if r:
            packets.append(r)
    n = 0
    for key, buf in reassemble_tcp(packets):
        for _sid, data in http2_data(buf).items():
            for obj in scan_xpc(data):
                if not isinstance(obj, dict):
                    continue
                payload = obj.get("payload")
                send = payload.get("send") if isinstance(payload, dict) else None
                if not send:
                    continue
                report, surface = send.get("_0"), send.get("_1")
                if want is not None and int(surface) != want:
                    continue
                n += 1
                print(f"{key[2]}->{key[3]} surface={int(surface)} len={len(report)} rid={report[0]:#04x} {report.hex()}")
    print(f"{n} reports", file=sys.stderr)


if __name__ == "__main__":
    main()
