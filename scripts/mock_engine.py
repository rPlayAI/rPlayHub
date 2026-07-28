#!/usr/bin/env python3
"""Replay a recorded .h265 as if it were the live engine. No device, no root.

This exists so the app can be developed and verified against real device frames without a phone
attached and without sudo. It speaks the same two sockets as `host/mirror.py`:

    127.0.0.1:9877   Annex-B HEVC, parameter sets first, paced to a frame rate, looping
    127.0.0.1:9876   the JSON-line control API — taps and swipes are logged, not injected

    python3 scripts/mock_engine.py [screen3.h265] [--fps 33] [--width 1170] [--height 2532]

The default width/height are an iPhone 13 Pro's real screen. They deliberately differ from the
coded video size (1184×2576) because the encoder pads to 16-pixel alignment — reproducing that
mismatch here is the point, since it is what the app has to crop and map clicks against.
"""
import argparse
import json
import os
import socket
import sys
import threading
import time

ANNEXB = b"\x00\x00\x00\x01"
VPS, SPS, PPS = 32, 33, 34
LOGICAL_MAX = 16384


def nal_type(nal):
    return (nal[0] >> 1) & 0x3F if nal else -1


def load_access_units(path):
    """Split the file into parameter sets and access units, the way the app expects them."""
    data = open(path, "rb").read()
    nals = [n for n in data.split(ANNEXB)[1:] if n]
    params, units, current = {}, [], []
    for nal in nals:
        t = nal_type(nal)
        if t in (VPS, SPS, PPS):
            params[t] = nal
            continue
        if t < 32:
            # first_slice_segment_in_pic_flag — top bit after the 2-byte NAL header
            if len(nal) >= 3 and (nal[2] & 0x80) and current:
                units.append(current)
                current = []
            current.append(nal)
    if current:
        units.append(current)
    return params, units


class MockEngine:
    def __init__(self, path, fps, width, height, start_at=0, burst=0):
        self.params, self.units = load_access_units(path)
        self.fps = fps
        self.start_at = start_at
        self.burst = burst
        self.screen = (width, height)
        self.taps = 0
        self.swipes = 0
        self.loops = 0
        self.nals_sent = 0
        self._clients = []
        self._lock = threading.Lock()
        self.recorder = None        # mirrors the real engine's recording feature
        missing = [n for n in (VPS, SPS, PPS) if n not in self.params]
        if missing:
            print(f"warning: file has no {missing} parameter sets — a decoder cannot start")
        print(f"loaded {len(self.units)} access units, "
              f"parameter sets {sorted(self.params)} from {path}")

    # ---------------- video ----------------
    def _preamble(self):
        return b"".join(ANNEXB + self.params[t] for t in (VPS, SPS, PPS) if t in self.params)

    def serve_video(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 9877))
        srv.listen(8)
        print("video   : 127.0.0.1:9877")
        while True:
            c, _ = srv.accept()
            with self._lock:
                self._clients.append(c)
            try:
                c.sendall(self._preamble())     # late joiners need these first
            except OSError:
                self._drop(c)
            print(f"  viewer connected ({len(self._clients)} total)")

    def _drop(self, c):
        with self._lock:
            if c in self._clients:
                self._clients.remove(c)
        try:
            c.close()
        except OSError:
            pass

    def pump_video(self):
        """One access unit per frame interval, looping forever."""
        interval = 1.0 / self.fps
        # start_at reproduces a late join: begin mid-GOP so the viewer gets no keyframe until the
        # loop wraps. That is the exact condition that made the first live run render black.
        i = min(self.start_at, len(self.units) - 1)
        if i:
            print(f"starting mid-GOP at access unit {i} — no keyframe until the loop wraps "
                  f"in ~{(len(self.units) - i) / self.fps:.0f}s")
        while True:
            if not self._clients:
                time.sleep(0.1)
                continue
            unit = self.units[i]
            payload = b"".join(ANNEXB + n for n in unit)
            # Re-send parameter sets each loop so a decoder can resynchronize.
            if i == 0:
                payload = self._preamble() + payload
            with self._lock:
                targets = list(self._clients)
            for c in targets:
                try:
                    c.sendall(payload)
                except OSError:
                    self._drop(c)
            self.nals_sent += len(unit)
            with self._lock:
                rec = self.recorder
            if rec:
                for n in unit:
                    rec["f"].write(ANNEXB + n)
                    rec["nals"] += 1
                    rec["bytes"] += len(n) + 4
                    if 16 <= nal_type(n) < 24:
                        rec["saw_keyframe"] = True
            i += 1
            if i >= len(self.units):
                i = 0
                self.loops += 1
            # Bursty delivery: send N units with no gap, then sleep for the time they represent.
            # The average rate is unchanged; the arrival pattern is not, and that is the point.
            if self.burst:
                if i % self.burst == 0:
                    time.sleep(interval * self.burst)
            else:
                time.sleep(interval)

    # ---------------- control ----------------
    def dispatch(self, method, params):
        if method == "ping":
            return {"pong": True}
        if method == "list_devices":
            # Two rows so the sidebar is actually exercised: the replayed device, plus one that is
            # visible but not connected, which is the state a device hub has to render too.
            return {"devices": [
                {"id": "mock-0", "udid": "MOCK-REPLAY",
                 "name": "iPhone13 (replay)", "product_type": "iPhone14,2",
                 "os_version": "27.0", "connected": True, "transport": "mock",
                 "screen_size": {"w": self.screen[0], "h": self.screen[1]}},
                {"id": "mock-1", "udid": "MOCK-OFFLINE",
                 "name": "Christina's iPhone", "product_type": "iPhone13,3",
                 "os_version": "26.5.2", "connected": False, "transport": "usbmux"},
            ]}
        if method == "stream_info":
            return {"port": 9877, "codec": "hevc", "container": "annexb",
                    "viewers": len(self._clients), "nals": self.nals_sent,
                    "rtp_packets": 0, "loops": self.loops, "uptime_s": 0,
                    "recording": self._rec_stats()}
        if method == "start_recording":
            path = params.get("path") or "replay-recording.h265"
            with self._lock:
                if self.recorder:
                    raise ValueError(f"already recording to {self.recorder['path']}")
                d = os.path.dirname(os.path.abspath(path))
                if d:
                    os.makedirs(d, exist_ok=True)
                f = open(path, "wb")
                for t in (VPS, SPS, PPS):
                    if t in self.params:
                        f.write(ANNEXB + self.params[t])
                self.recorder = {"path": path, "f": f, "nals": 0, "bytes": 0,
                                 "started": time.time(), "saw_keyframe": False}
            print(f"  RECORD start -> {path}")
            return self._rec_stats()
        if method == "stop_recording":
            with self._lock:
                if not self.recorder:
                    raise ValueError("not recording")
                rec, self.recorder = self.recorder, None
            stats = self._stats_of(rec)
            rec["f"].close()
            print(f"  RECORD stop  {stats}")
            return stats
        if method == "tap":
            self.taps += 1
            fx, fy = self._fractions(params)
            print(f"  TAP   fx={fx:.4f} fy={fy:.4f}  -> device px "
                  f"({fx * self.screen[0]:.0f}, {fy * self.screen[1]:.0f})")
            return {"x": int(fx * LOGICAL_MAX), "y": int(fy * LOGICAL_MAX),
                    "logical_max": LOGICAL_MAX}
        if method == "swipe":
            self.swipes += 1
            fx0, fy0 = self._fractions(params, "x0", "y0")
            fx1, fy1 = self._fractions(params, "x1", "y1")
            print(f"  SWIPE ({fx0:.3f},{fy0:.3f}) -> ({fx1:.3f},{fy1:.3f})")
            return {"from": [int(fx0 * LOGICAL_MAX), int(fy0 * LOGICAL_MAX)],
                    "to": [int(fx1 * LOGICAL_MAX), int(fy1 * LOGICAL_MAX)],
                    "logical_max": LOGICAL_MAX}
        if method == "take_screenshot":
            raise ValueError("replay engine has no screenshot service")
        raise KeyError(method)

    def _stats_of(self, rec):
        return {"path": rec["path"], "nals": rec["nals"], "bytes": rec["bytes"],
                "duration_s": round(time.time() - rec["started"], 2),
                "saw_keyframe": rec["saw_keyframe"]}

    def _rec_stats(self):
        with self._lock:
            return self._stats_of(self.recorder) if self.recorder else None

    def _fractions(self, params, xk="x", yk="y"):
        fx, fy = params.get("f" + xk), params.get("f" + yk)
        if fx is not None and fy is not None:
            return float(fx), float(fy)
        x, y = params.get(xk), params.get(yk)
        if x is None or y is None:
            raise ValueError(f"need {xk}/{yk} or f{xk}/f{yk}")
        return float(x) / self.screen[0], float(y) / self.screen[1]

    def serve_control(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 9876))
        srv.listen(16)
        print("control : 127.0.0.1:9876")
        while True:
            c, _ = srv.accept()
            threading.Thread(target=self._control_conn, args=(c,), daemon=True).start()

    def _control_conn(self, sock):
        buf = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    sock.sendall(json.dumps(self._handle(line)).encode() + b"\n")
        except OSError:
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def _handle(self, line):
        try:
            req = json.loads(line)
        except ValueError as e:
            return {"ok": False, "error": {"code": "bad_json", "message": str(e)}}
        rid, method = req.get("id"), req.get("method")
        try:
            resp = {"ok": True, "result": self.dispatch(method, req.get("params") or {})}
        except KeyError:
            resp = {"ok": False, "error": {"code": "unsupported_method", "message": str(method)}}
        except ValueError as e:
            resp = {"ok": False, "error": {"code": "bad_request", "message": str(e)}}
        if rid is not None:
            resp["id"] = rid
        return resp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", default="screen3.h265")
    ap.add_argument("--fps", type=float, default=33.0)
    ap.add_argument("--width", type=int, default=1170)
    ap.add_argument("--height", type=int, default=2532)
    ap.add_argument("--start-at", type=int, default=0,
                    help="begin at this access unit — use it to reproduce a late, keyframe-less join")
    ap.add_argument("--burst", type=int, default=0, metavar="N",
                    help="send N access units back-to-back then sleep, instead of pacing evenly. "
                         "Live RTP arrives in bursts (400+ packets/s), and even pacing hides bugs "
                         "that only appear when several frames are assembled in the same instant.")
    args = ap.parse_args()

    engine = MockEngine(args.file, args.fps, args.width, args.height, args.start_at, args.burst)
    threading.Thread(target=engine.serve_video, daemon=True).start()
    threading.Thread(target=engine.serve_control, daemon=True).start()
    threading.Thread(target=engine.pump_video, daemon=True).start()
    print(f"replaying at {args.fps} fps, screen {args.width}×{args.height}. Ctrl-C to stop.\n")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print(f"\nstopped. {engine.taps} taps, {engine.swipes} swipes, "
              f"{engine.nals_sent} NALs sent, {engine.loops} loops")
    return 0


if __name__ == "__main__":
    sys.exit(main())
