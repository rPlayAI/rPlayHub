"""Offline checks for mirror.py — everything that does not need a phone."""
import json
import socket
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mirror


def nal(t, body=b"\x01\x02\x03"):
    """Build an HEVC NAL with type t in the 2-byte header."""
    return bytes([(t << 1) & 0x7E, 0x01]) + body


def test_broadcaster_late_joiner():
    b = mirror.VideoBroadcaster()
    b.publish(nal(32))            # VPS
    b.publish(nal(33))            # SPS
    b.publish(nal(34))            # PPS
    b.publish(nal(1))             # a slice, before anyone connects

    a, c = socket.socketpair()
    b.add_client(a)
    b.publish(nal(19))            # IDR after joining

    c.settimeout(2.0)
    got = c.recv(65536)
    # The late joiner must receive the cached parameter sets, then the new NAL.
    assert got.count(mirror._ANNEXB) == 4, got.count(mirror._ANNEXB)
    types = [mirror._nal_type(p) for p in got.split(mirror._ANNEXB)[1:]]
    assert types == [32, 33, 34, 19], types
    assert b.viewers == 1
    a.close(); c.close()
    print("  broadcaster: late joiner gets cached VPS/SPS/PPS then live NALs — OK")


def test_broadcaster_drops_dead_client():
    b = mirror.VideoBroadcaster()
    a, c = socket.socketpair()
    b.add_client(a)
    c.close()
    a.close()                     # sending now must fail and self-heal
    b.publish(nal(1))
    assert b.viewers == 0, b.viewers
    print("  broadcaster: dead client dropped, publish survives — OK")


def test_nal_type_and_png():
    assert mirror._nal_type(nal(32)) == 32
    assert mirror._nal_type(b"") == -1
    png = b"\x89PNG\r\n\x1a\n" + b"\x00" * 8 + (1170).to_bytes(4, "big") + (2532).to_bytes(4, "big")
    assert mirror._png_size(png) == (1170, 2532)
    assert mirror._png_size(b"not a png") is None
    print("  helpers: NAL type + PNG IHDR size parse — OK")


class FakeCore(mirror.MirrorCore):
    """A core with no device behind it, for exercising request dispatch."""
    def __init__(self):
        self.screen_size = (1170, 2532)
        self.taps = []
        self.swipes = []

    def dispatch(self, method, params):
        if method == "tap":
            fx, fy = self._fractions(params)
            self.taps.append((fx, fy))
            return {"x": int(fx * mirror.LOGICAL_MAX), "y": int(fy * mirror.LOGICAL_MAX),
                    "logical_max": mirror.LOGICAL_MAX}
        if method == "swipe":
            fx0, fy0 = self._fractions(params, "x0", "y0")
            fx1, fy1 = self._fractions(params, "x1", "y1")
            self.swipes.append((fx0, fy0, fx1, fy1))
            return {"logical_max": mirror.LOGICAL_MAX}
        return mirror.MirrorCore.dispatch(self, method, params)


def test_dispatch_and_envelope():
    c = FakeCore()

    r = c._handle_line(b'{"id":1,"method":"ping"}')
    assert r == {"ok": True, "result": {"pong": True}, "id": 1}, r

    r = c._handle_line(b'{"id":2,"method":"nope"}')
    assert r["ok"] is False and r["error"]["code"] == "unsupported_method", r

    r = c._handle_line(b"{not json")
    assert r["ok"] is False and r["error"]["code"] == "bad_json", r

    r = c._handle_line(b'["a","list"]')
    assert r["ok"] is False and r["error"]["code"] == "bad_request", r

    # pixels resolved against the real screen size
    r = c._handle_line(b'{"id":3,"method":"tap","params":{"x":585,"y":1266}}')
    assert r["ok"] is True, r
    assert r["result"]["logical_max"] == 16384
    assert abs(c.taps[-1][0] - 0.5) < 0.01 and abs(c.taps[-1][1] - 0.5) < 0.01, c.taps

    # normalized fractions
    r = c._handle_line(b'{"id":4,"method":"tap","params":{"fx":0.25,"fy":0.75}}')
    assert r["ok"] is True and c.taps[-1] == (0.25, 0.75), (r, c.taps)

    # out-of-range pixels clamp rather than escaping 0..1
    c._handle_line(b'{"method":"tap","params":{"x":99999,"y":-5}}')
    assert c.taps[-1] == (1.0, 0.0), c.taps

    # missing coordinates is a bad_request, not a crash
    r = c._handle_line(b'{"id":5,"method":"tap","params":{}}')
    assert r["ok"] is False and r["error"]["code"] == "bad_request", r

    r = c._handle_line(b'{"id":6,"method":"swipe","params":'
                       b'{"x0":585,"y0":2000,"x1":585,"y1":500}}')
    assert r["ok"] is True and len(c.swipes) == 1, r

    # a request with no id gets a response with no id (notification style)
    r = c._handle_line(b'{"method":"ping"}')
    assert "id" not in r, r

    # unknown screen size + pixel coords must explain itself
    c.screen_size = None
    r = c._handle_line(b'{"id":7,"method":"tap","params":{"x":10,"y":10}}')
    assert r["ok"] is False and "0..1" in r["error"]["message"], r
    print("  dispatch: envelope, error codes, pixel+fraction coords, clamping — OK")


def test_recorder():
    import tempfile
    d = tempfile.mkdtemp()
    params = {32: nal(32), 33: nal(33), 34: nal(34)}

    # Default: take everything, so a short recording is never empty even with no keyframe.
    p1 = os.path.join(d, "nokey.h265")
    r = mirror.Recorder(p1, params)
    for _ in range(5):
        r.write(nal(1))
    s = r.close()
    assert s["saw_keyframe"] is False, s
    assert s["nals"] == 8, s                      # 3 parameter sets + 5 slices
    body = open(p1, "rb").read()
    types = [mirror._nal_type(x) for x in body.split(mirror._ANNEXB)[1:]]
    assert types == [32, 33, 34, 1, 1, 1, 1, 1], types

    # wait_for_keyframe: drop leading slices so the file starts at an IRAP and is decodable.
    p2 = os.path.join(d, "waited.h265")
    r = mirror.Recorder(p2, params, wait_for_keyframe=True)
    for _ in range(3):
        r.write(nal(1))                           # before any keyframe — dropped
    r.write(nal(19))                              # IDR_W_RADL
    r.write(nal(1))
    s = r.close()
    assert s["saw_keyframe"] is True, s
    assert s["skipped_before_keyframe"] == 3, s
    types = [mirror._nal_type(x)
             for x in open(p2, "rb").read().split(mirror._ANNEXB)[1:]]
    assert types == [32, 33, 34, 19, 1], types
    print("  recorder: parameter sets first, keyframe gating, honest stats — OK")


def test_broadcaster_feeds_recorder():
    import tempfile
    b = mirror.VideoBroadcaster()
    b.publish(nal(32)); b.publish(nal(33)); b.publish(nal(34))
    path = os.path.join(tempfile.mkdtemp(), "live.h265")

    b.start_recording(path)
    b.publish(nal(19))
    b.publish(nal(1))
    stats = b.stop_recording()
    assert stats["saw_keyframe"] is True, stats
    # Cached parameter sets must land in the file even though they arrived before recording.
    types = [mirror._nal_type(x)
             for x in open(path, "rb").read().split(mirror._ANNEXB)[1:]]
    assert types == [32, 33, 34, 19, 1], types

    try:
        b.stop_recording()
    except ValueError:
        pass
    else:
        raise AssertionError("stopping twice should raise")

    b.start_recording(path + "2")
    try:
        b.start_recording(path + "3")
    except ValueError:
        pass
    else:
        raise AssertionError("starting twice should raise")
    b.stop_recording()
    print("  broadcaster: live NALs reach the recorder, double start/stop refused — OK")


if __name__ == "__main__":
    print("offline checks for mirror.py")
    test_nal_type_and_png()
    test_broadcaster_late_joiner()
    test_broadcaster_drops_dead_client()
    test_dispatch_and_envelope()
    test_recorder()
    test_broadcaster_feeds_recorder()
    print("all offline checks passed")
