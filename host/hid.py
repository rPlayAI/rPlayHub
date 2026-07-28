#!/usr/bin/env python3
"""HID injection — control the phone via com.apple.coredevice.hid.universalhidservice.

A CoreDevice service = a RemoteXPC channel to its RSD port through the tunnel. To tap:
send a mainTouchscreen HID report (surface _ServiceID 257) with a contact sample then a
release. Report/envelope formats verified against devicectl sniffs (pmd3 oracle).
"""
import struct, time

import xpc
from remotexpc import RemoteXPC

HID_FEATURE = "com.apple.coredevice.feature.remote.universalhidservice"
MAIN_TOUCHSCREEN = 257          # _ServiceID of the real touchscreen (58-byte rid 0x09)
TS_REPORT_ID = 0x09
TS_STATE_CONTACT = 0xC2         # contact in progress at this position
TS_STATE_RELEASE = 0x02         # lift


def _ts(): return time.monotonic_ns() & ((1 << 48) - 1)


def touchscreen_report(state: int, x: int, y: int, timestamp: int | None = None) -> bytes:
    """58-byte mainTouchscreen report. x,y are UInt16 (0..65535, normalized across screen)."""
    if timestamp is None:
        timestamp = _ts()
    return (bytes([TS_REPORT_ID, 0x01, 0x05, state])
            + struct.pack("<HH", x & 0xFFFF, y & 0xFFFF)
            + b"\x00" * 32 + b"\x02\x00\x00\x00"
            + timestamp.to_bytes(6, "little") + b"\x00" * 8)


class UniversalHID:
    def __init__(self, host, port):
        self.x = RemoteXPC(host, port)

    def list_surfaces(self):
        r = self.x.send_receive({
            "featureIdentifier": HID_FEATURE, "messageType": "Request",
            "payload": {"connectedServices": {}},
        })
        return r

    def send_report(self, service_id: int, report: bytes):
        self.x.send_request({
            "featureIdentifier": HID_FEATURE, "messageType": "Request",
            "payload": {"send": {"_0": report, "_1": xpc.U64(service_id)}},
        })

    def tap(self, fx: float, fy: float, surface: int = MAIN_TOUCHSCREEN, hold_ms: int = 60):
        """Tap at fractional screen position (fx,fy) in 0..1. Contact frames then release."""
        x, y = int(fx * 65535), int(fy * 65535)
        deadline = time.time() + hold_ms / 1000.0
        while time.time() < deadline:                      # a few contact samples
            self.send_report(surface, touchscreen_report(TS_STATE_CONTACT, x, y))
            time.sleep(0.012)
        self.send_report(surface, touchscreen_report(TS_STATE_RELEASE, x, y))

    def swipe(self, fx0, fy0, fx1, fy1, surface=MAIN_TOUCHSCREEN, ms=300, steps=20):
        for i in range(steps + 1):
            t = i / steps
            x = int((fx0 + (fx1 - fx0) * t) * 65535)
            y = int((fy0 + (fy1 - fy0) * t) * 65535)
            self.send_report(surface, touchscreen_report(TS_STATE_CONTACT, x, y))
            time.sleep(ms / 1000.0 / steps)
        self.send_report(surface, touchscreen_report(TS_STATE_RELEASE, x, y))

    def close(self):
        self.x.close()


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 4:
        sys.exit("usage: python3 hid.py <device_addr> <hid_port> <fx> <fy>   (0..1 fractions; tunnel up)")
    host, port, fx, fy = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    h = UniversalHID(host, port)
    print("surfaces:", h.list_surfaces())
    print(f"tapping at ({fx},{fy}) ...")
    h.tap(fx, fy)
    print("done")
    h.close()
