#!/usr/bin/env python3
"""Measure how often the encoder downshifts resolution, under identical synthetic motion.

    python3 scripts/rvra-bitrate.py --label baseline [--swipe-seconds 20]

This is the instrument for the one RVRA lever that has never been varied on its own: bitrate.
`doc/RVRA-AND-PORTABILITY.md` established that the feature-list string cannot turn resolution
adaptation off -- the device ignores `RVRA1:0`. What remains untested is whether adaptation is
driven by the rate budget instead, which would make it escapable and would keep the Linux/Windows
port from needing either a two-tier product or a reimplementation of Apple's reference resampler.

Why this exists rather than watching the app's log. Three reasons, and each of them invalidated an
earlier hand-run comparison:

  * The motion must be identical between runs. Adaptation is provoked by movement, so a hand swipe
    that is a little faster in the second run produces more downshifts for a reason that has
    nothing to do with the variable under test.
  * There must be a still phase. If a run shows no downshifts it matters a great deal whether the
    phone was being swiped at the time, and a log of tier changes alone cannot say.
  * The reading must not need a viewer app. rPlayHub decodes, and a decoder that keeps up is a
    confounder; this reads the wire.

It measures off the daemon's Annex-B broadcast on :9877, using the per-frame trailer the device
appends to the last slice NAL of every access unit -- the only place on the wire that says what
resolution the encoder actually coded a picture at. Connecting to :9877 is itself what starts the
media stream, so nothing else needs to be running, and nothing else SHOULD be: a second viewer
splits the device's encoder budget and the bitrate figures stop meaning anything.
"""
import argparse
import json
import socket
import sys
import threading
import time

CONTROL = ("127.0.0.1", 9876)
VIDEO = ("127.0.0.1", 9877)

# The resolutions this encoder family uses. Matching against a known set, rather than reading two
# big-endian shorts wherever they fall, is what keeps entropy-coded slice data from matching by
# accident -- see HEVCStream.parseActiveRectTrailer, which this mirrors byte for byte.
TIERS = [(1184, 2576), (1088, 1920), (720, 1280)]
FULL = TIERS[0]


def parse_trailer(nal: bytes):
    """(width, height) from the trailer at the end of a slice NAL, or None."""
    n = len(nal)
    if n <= 24:
        return None
    for w, h in TIERS:
        pat = bytes((w >> 8, w & 0xFF, h >> 8, h & 0xFF, 0))
        i = n - 5
        while i >= max(1, n - 24):
            if nal[i:i + 5] == pat:
                return (w, h)
            i -= 1
    return None


def self_test(paths):
    """Run the trailer parser over recorded Annex-B and say what it found.

    A null result from this instrument is a claim about the encoder, so the parser has to be shown
    to report *something* before its silence is worth anything -- the same trap that had `strings`
    "proving" RVRA was absent from AVConference when it was simply looking in the wrong section.
    Point this at a capture known to contain downshifts and check the tiers come back.
    """
    ok = True
    for path in paths:
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"  {path}: cannot read -- {e}")
            ok = False
            continue
        nals = data.split(b"\x00\x00\x00\x01")[1:]
        hits = [t for t in (parse_trailer(n) for n in nals) if t]
        hist = {}
        for w, h in hits:
            hist[f"{w}x{h}"] = hist.get(f"{w}x{h}", 0) + 1
        changes = sum(1 for a, b in zip(hits, hits[1:]) if a != b)
        # Episodes, not just frames: option 2 in doc/RVRA-AND-PORTABILITY.md holds the last good
        # picture through a downshift, so what the viewer notices is how often the freeze starts
        # and how long it lasts, which a percentage on its own does not say.
        episodes, cur = [], 0
        for t in hits:
            if t != FULL:
                cur += 1
            elif cur:
                episodes.append(cur)
                cur = 0
        if cur:
            episodes.append(cur)
        below = 100.0 * sum(1 for t in hits if t != FULL) / len(hits) if hits else 0.0
        print(f"  {path}: {len(nals)} NALs, {len(hits)} carry a trailer, "
              f"{changes} tier changes, {hist or '{}'}")
        print(f"      {below:.1f}% below full tier in {len(episodes)} episodes"
              + (f", longest {max(episodes)} frames" if episodes else ""))
        if not hits:
            ok = False
    print("  parser reports trailers" if ok else "  PARSER FOUND NOTHING -- do not trust a null run")
    return 0 if ok else 1


def preflight():
    """Say whether there is a device that can actually stream, before spending a minute finding out.

    The daemon keeps serving a session after its device goes away: `tunnel_info` still answers with
    addresses and per-session service ports, and a viewer connecting to :9877 gets a silent nothing
    that is indistinguishable from an encoder sending no video. Connecting to the tunnel is what
    tells the two apart.
    """
    try:
        info = rpc("tunnel_info", timeout=8).get("result", {})
    except OSError as e:
        return f"the daemon is not answering on {CONTROL[0]}:{CONTROL[1]} ({e})"
    addr, port = info.get("device_addr"), info.get("services", {}).get("displayservice")
    if not addr or not port:
        return "the daemon has no tunnel; it has not bound a device"

    s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    s.settimeout(6)
    try:
        s.connect((addr, port))
    except OSError as e:
        attached = []
        try:
            for d in rpc("list_devices", timeout=8).get("result", {}).get("devices", []):
                attached.append(f"{d.get('udid','?')} ({d.get('connection','?')})")
        except OSError:
            pass
        return (f"the tunnel is stale: displayservice at [{addr}]:{port} is unreachable ({e}).\n"
                f"       The daemon is bound to {info.get('udid','?')}.\n"
                f"       Attached now: {', '.join(attached) or 'nothing'}.\n"
                f"       Reconnect the device and restart cdhost.")
    finally:
        s.close()
    return None


def rpc(method, params=None, timeout=10.0):
    s = socket.create_connection(CONTROL, timeout=timeout)
    try:
        s.sendall(json.dumps({"id": 1, "method": method,
                              "params": params or {}}).encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf.decode())
    finally:
        s.close()


class Swiper(threading.Thread):
    """Page back and forth across the screen at a fixed cadence.

    Horizontal, because it is the one gesture whose effect does not depend on what is on screen:
    a home-screen page turn and a scroll in an app both move the whole picture, which is the
    workload that provokes adaptation. Direction alternates so the device never runs out of room.
    """

    def __init__(self, duration_ms, gap_s):
        super().__init__(daemon=True)
        self.duration_ms = duration_ms
        self.gap_s = gap_s
        self.stop = threading.Event()
        self.sent = 0
        self.failed = 0

    def run(self):
        rightward = True
        while not self.stop.is_set():
            x0, x1 = (0.85, 0.15) if rightward else (0.15, 0.85)
            rightward = not rightward
            try:
                r = rpc("swipe", {"fx0": x0, "fy0": 0.5, "fx1": x1, "fy1": 0.5,
                                  "duration_ms": self.duration_ms})
                if r.get("ok"):
                    self.sent += 1
                else:
                    self.failed += 1
            except OSError:
                self.failed += 1
            self.stop.wait(self.gap_s)


class Reader(threading.Thread):
    """Split the Annex-B broadcast into NALs and record the tier of every frame that carries one."""

    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.frames = []        # (monotonic time, (w, h))
        self.bytes = 0

    def run(self):
        buf = b""
        while not self.stop.is_set():
            try:
                chunk = self.sock.recv(1 << 20)
            except OSError:
                break
            if not chunk:
                break
            with self.lock:
                self.bytes += len(chunk)
            buf += chunk
            # Keep the last partial NAL; everything before the final start code is complete.
            parts = buf.split(b"\x00\x00\x00\x01")
            buf = parts.pop()
            now = time.monotonic()
            for nal in parts:
                if not nal:
                    continue
                tier = parse_trailer(nal)
                if tier:
                    with self.lock:
                        self.frames.append((now, tier))

    def since(self, t0, t1):
        with self.lock:
            return [tier for t, tier in self.frames if t0 <= t <= t1]


def summarise(tiers):
    """Counts, transitions and the fraction of time spent below full resolution."""
    out = {"frames": len(tiers), "per_tier": {}, "changes": 0,
           "below_full_pct": 0.0}
    for t in tiers:
        k = f"{t[0]}x{t[1]}"
        out["per_tier"][k] = out["per_tier"].get(k, 0) + 1
    for a, b in zip(tiers, tiers[1:]):
        if a != b:
            out["changes"] += 1
    if tiers:
        out["below_full_pct"] = 100.0 * sum(1 for t in tiers if t != FULL) / len(tiers)
    return out


def phase(reader, seconds, label):
    t0 = time.monotonic()
    b0 = reader.bytes
    time.sleep(seconds)
    t1 = time.monotonic()
    tiers = reader.since(t0, t1)
    s = summarise(tiers)
    s["phase"] = label
    s["seconds"] = round(t1 - t0, 2)
    s["fps"] = round(len(tiers) / (t1 - t0), 1)
    s["mbps"] = round((reader.bytes - b0) * 8 / (t1 - t0) / 1e6, 2)
    s["changes_per_s"] = round(s["changes"] / (t1 - t0), 2)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run", help="what varies in this run, for the report")
    ap.add_argument("--still-seconds", type=float, default=6.0)
    ap.add_argument("--swipe-seconds", type=float, default=20.0)
    ap.add_argument("--swipe-ms", type=int, default=120, help="duration of one swipe")
    ap.add_argument("--swipe-gap", type=float, default=0.25, help="pause between swipes")
    ap.add_argument("--json", help="append the result as one JSON line to this file")
    ap.add_argument("--self-test", nargs="+", metavar="FILE",
                    help="run the trailer parser over recorded Annex-B and exit")
    args = ap.parse_args()

    if args.self_test:
        print("== parser self-test")
        return self_test(args.self_test)

    problem = preflight()
    if problem:
        sys.exit(f"cannot measure: {problem}")

    # Wait for the daemon rather than racing it: it is usually being restarted with new flags
    # immediately before this runs.
    deadline = time.time() + 90
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(VIDEO, timeout=5)
            break
        except OSError:
            time.sleep(1)
    if sock is None:
        sys.exit("no engine on 127.0.0.1:9877 after 90s")

    reader = Reader(sock)
    reader.start()

    # The stream only starts once a viewer connects, and the device's parameter sets and its one
    # unprompted IDR arrive first. Wait for real frames before timing anything.
    warm = time.time() + 45
    while not reader.frames and time.time() < warm:
        time.sleep(0.5)
    if not reader.frames:
        # Say which of the three possible silences this is. "No trailers" and "no video" look
        # identical from the caller's side and mean completely different things -- one is a claim
        # about the encoder, the other is a dead stream.
        try:
            si = rpc("stream_info").get("result", {})
        except OSError:
            si = {}
        if reader.bytes == 0:
            why = ("no video bytes arrived at all. "
                   f"The daemon reports rtp_packets={si.get('rtp_packets', '?')}, "
                   f"streaming={si.get('streaming', '?')} -- the media stream never started, so "
                   "this says nothing about resolution adaptation.")
        else:
            why = (f"{reader.bytes} bytes of video arrived but no NAL carried a resolution "
                   "trailer. That is a real finding about the stream -- check the parser with "
                   "--self-test against a known capture before believing it.")
        sys.exit(f"no measurement: {why}")
    time.sleep(2.0)   # let the rate settle after the opening keyframe burst

    print(f"== {args.label}")
    still_before = phase(reader, args.still_seconds, "still-before")
    print(f"  still  {still_before['frames']:4d} frames  {still_before['fps']:5.1f} fps  "
          f"{still_before['mbps']:5.2f} Mbps  {still_before['changes']:3d} changes  "
          f"{still_before['below_full_pct']:5.1f}% below full")

    swiper = Swiper(args.swipe_ms, args.swipe_gap)
    swiper.start()
    swiping = phase(reader, args.swipe_seconds, "swiping")
    swiper.stop.set()
    swiper.join(timeout=5)
    print(f"  swipe  {swiping['frames']:4d} frames  {swiping['fps']:5.1f} fps  "
          f"{swiping['mbps']:5.2f} Mbps  {swiping['changes']:3d} changes  "
          f"{swiping['below_full_pct']:5.1f}% below full   "
          f"({swiper.sent} swipes sent, {swiper.failed} failed)")

    still_after = phase(reader, args.still_seconds, "still-after")
    print(f"  still  {still_after['frames']:4d} frames  {still_after['fps']:5.1f} fps  "
          f"{still_after['mbps']:5.2f} Mbps  {still_after['changes']:3d} changes  "
          f"{still_after['below_full_pct']:5.1f}% below full")

    reader.stop.set()
    try:
        sock.close()
    except OSError:
        pass

    result = {"label": args.label,
              "swipes_sent": swiper.sent, "swipes_failed": swiper.failed,
              "phases": [still_before, swiping, still_after]}
    try:
        result["stream_info"] = rpc("stream_info").get("result", {})
    except OSError:
        pass

    if args.json:
        with open(args.json, "a") as f:
            f.write(json.dumps(result) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
