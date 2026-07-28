#!/usr/bin/env python3
"""Verify a running mirror engine — control API and live video, end to end.

Start the engine first (it needs root for the utun):

    sudo python3 host/mirror.py DEVICE-UDID-REDACTED

then, as a normal user:

    python3 scripts/mirror_check.py [--tap] [--seconds 3]

Checks, in order: ping · list_devices · stream_info · take_screenshot (written to disk) · live video
(counts NAL units arriving on the stream port) · optionally a real tap at the centre of the screen.

`--tap` moves the phone's UI, so it is off by default.
"""
import argparse
import base64
import json
import socket
import sys
import time

API = ("127.0.0.1", 9876)
STREAM = ("127.0.0.1", 9877)
ANNEXB = b"\x00\x00\x00\x01"

OK, BAD = "  ok  ", " FAIL "


class Client:
    """One connection, one request per line, one response per line."""

    def __init__(self, address=API, timeout=20.0):
        self.sock = socket.create_connection(address, timeout=timeout)
        self.buf = b""
        self._id = 0

    def call(self, method, **params):
        self._id += 1
        req = {"id": self._id, "method": method}
        if params:
            req["params"] = params
        self.sock.sendall(json.dumps(req).encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("engine closed the control connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        resp = json.loads(line)
        if not resp.get("ok"):
            err = resp.get("error", {})
            raise RuntimeError(f"{method}: {err.get('code')}: {err.get('message')}")
        return resp.get("result", {})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def count_nals(seconds):
    """Pull the live stream for a bit and report what actually arrived."""
    s = socket.create_connection(STREAM, timeout=seconds + 5)
    s.settimeout(seconds + 5)
    deadline = time.monotonic() + seconds
    data = b""
    while time.monotonic() < deadline:
        try:
            chunk = s.recv(1 << 16)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    nals = data.split(ANNEXB)[1:]
    types = {}
    for n in nals:
        if n:
            t = (n[0] >> 1) & 0x3F
            types[t] = types.get(t, 0) + 1
    return len(data), len(nals), types


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tap", action="store_true",
                    help="really tap the centre of the screen (moves the phone's UI)")
    ap.add_argument("--seconds", type=float, default=3.0, help="how long to sample video")
    ap.add_argument("--shot", default="mirror_check.png", help="where to write the screenshot")
    args = ap.parse_args()

    failures = 0
    try:
        c = Client()
    except OSError as e:
        print(f"{BAD} cannot reach the control API on {API[0]}:{API[1]} — is the engine running? ({e})")
        return 1

    try:
        c.call("ping")
        print(f"{OK} ping")
    except Exception as e:
        print(f"{BAD} ping — {e}"); failures += 1

    size = None
    try:
        devs = c.call("list_devices")["devices"]
        if not devs:
            print(f"{BAD} list_devices returned no devices"); failures += 1
        else:
            d = devs[0]
            size = d.get("screen_size")
            print(f"{OK} list_devices — {d.get('product_type')} iOS {d.get('os_version')} "
                  f"via {d.get('transport')}, screen {size}")
    except Exception as e:
        print(f"{BAD} list_devices — {e}"); failures += 1

    try:
        info = c.call("stream_info")
        print(f"{OK} stream_info — {info['rtp_packets']} RTP packets, {info['nals']} NALs, "
              f"{info['viewers']} viewer(s), up {info['uptime_s']}s")
        if info["rtp_packets"] == 0:
            print(f"{BAD} the device has sent no RTP at all — the media stream is not flowing")
            failures += 1
    except Exception as e:
        print(f"{BAD} stream_info — {e}"); failures += 1

    try:
        shot = c.call("take_screenshot")
        img = base64.b64decode(shot["image_b64"])
        with open(args.shot, "wb") as f:
            f.write(img)
        print(f"{OK} take_screenshot — {len(img)} bytes, "
              f"{shot['width']}x{shot['height']} -> {args.shot}")
    except Exception as e:
        print(f"{BAD} take_screenshot — {e}"); failures += 1

    try:
        nbytes, nnals, types = count_nals(args.seconds)
        # 32/33/34 are VPS/SPS/PPS; a late joiner must get those before anything else.
        params_present = all(t in types for t in (32, 33, 34))
        print(f"{OK} live video — {nbytes} bytes, {nnals} NALs in {args.seconds}s, types {types}")
        if not nnals:
            print(f"{BAD} no NAL units arrived — video is not reaching viewers"); failures += 1
        elif not params_present:
            print(f"{BAD} joined without VPS/SPS/PPS ({sorted(types)}) — a decoder cannot start")
            failures += 1
    except Exception as e:
        print(f"{BAD} live video — {e}"); failures += 1

    if args.tap:
        try:
            if size:
                r = c.call("tap", x=size["w"] // 2, y=size["h"] // 2)
            else:
                r = c.call("tap", fx=0.5, fy=0.5)
            print(f"{OK} tap — {r} (watch the phone)")
        except Exception as e:
            print(f"{BAD} tap — {e}"); failures += 1
    else:
        print("  skip  tap (pass --tap to really touch the screen)")

    c.close()
    print(f"\n{'all checks passed' if not failures else f'{failures} check(s) failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
