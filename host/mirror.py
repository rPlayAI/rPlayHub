#!/usr/bin/env python3
"""The mirror + control core — live screen out, taps in, over our own CoreDevice stack.

This is the engine a View Screen window sits on. Two things make it different from
`tunnel_up.py record`, which only ever wrote a file and exited:

  1. The media stream stays open for the whole session. That is not just for video — touch is
     only routed to UIKit while a media stream runs (the dtuhidd auth gate), so holding it open
     is what makes taps instant instead of paying ~1.2 s of stream setup each time.
  2. Video is broadcast live to any number of clients as Annex-B HEVC, and control comes in over
     the same newline-delimited JSON contract ~/rplay already speaks, so its existing Python
     client and MCP wrapper drive this unchanged.

Run (root, because of the utun):

    sudo python3 host/mirror.py [udid]

Then, in two other terminals:

    ffplay -fflags nobuffer -flags low_delay tcp://127.0.0.1:9877   # live screen
    printf '{"id":1,"method":"tap","params":{"x":200,"y":400}}\\n' | nc 127.0.0.1 9876

API (contract: app/api/PROTOCOL.md; adopted from refs/rplay/doc/sdk-api.md):
    ping · list_devices · take_screenshot · tap · swipe · stream_info
Requests  {"id":…, "method":…, "params":{…}}
Responses {"id":…, "ok":true, "result":{…}} | {"id":…, "ok":false, "error":{"code","message"}}

Python is the harness here: this proves the live loop end-to-end so the Swift app and the C core
can be built against something known to work.
"""
import base64
import json
import os
import queue
import re
import socket
import struct
import sys
import threading
import time

# Verified flat modules — the wire code that already works against real phones.
import coredevice
import hid
import screen
from rplayhub import rsd
from rplayhub.net import TunnelPump, open_tun
from rplayhub.report import write_report
from rplayhub.transport.usbmux_transport import UsbmuxTransport, list_devices

API_PORT = 9876          # same port ~/rplay uses, same contract
STREAM_PORT = 9877       # raw Annex-B HEVC, one byte stream per client
LOGICAL_MAX = 16384      # ~/rplay's touch coordinate space, echoed back to clients

# NAL types we must cache so a client joining mid-stream can decode. The two codecs disagree on
# everything here — header layout, which types are parameter sets, and which are keyframes — so
# every classification has to know which codec is in play.
_HEVC_VPS, _HEVC_SPS, _HEVC_PPS = 32, 33, 34
_HEVC_IRAP = range(16, 24)      # BLA/IDR/CRA — a decoder can start here
_H264_SPS, _H264_PPS, _H264_IDR = 7, 8, 5
PARAM_SET_ORDER = {"hevc": (_HEVC_VPS, _HEVC_SPS, _HEVC_PPS), "h264": (_H264_SPS, _H264_PPS)}
_ANNEXB = b"\x00\x00\x00\x01"


def _nal_type(nal: bytes, codec: str = "hevc") -> int:
    """HEVC uses a 2-byte header with the type in bits 1-6; H.264 a 1-byte header, bits 0-4."""
    if not nal:
        return -1
    return nal[0] & 0x1F if codec == "h264" else (nal[0] >> 1) & 0x3F


def _classify(nal: bytes, codec: str):
    """Return (nal_type, is_parameter_set, is_keyframe)."""
    t = _nal_type(nal, codec)
    if codec == "h264":
        return t, t in (_H264_SPS, _H264_PPS), t == _H264_IDR
    return t, t in (_HEVC_VPS, _HEVC_SPS, _HEVC_PPS), t in _HEVC_IRAP


def _describe_device_error(exc):
    """Turn a CoreDevice error into (message, code, is_permanent).

    The device returns a useful localized description buried in an NSKeyedArchiver blob; printing
    the blob is noise. Some refusals are permanent — "requires iOS 27.0 or later" will never
    succeed however many times it is retried — and telling those apart stops the engine spinning.
    """
    text = str(exc)
    message, code = text, None
    # The readable part is already decoded into userInfo by the XPC layer.
    m = re.search(r"'NSLocalizedDescription':\s*'([^']+)'", text)
    if m:
        message = m.group(1)
    c = re.search(r"'code':\s*(\d+)", text)
    if c:
        code = int(c.group(1))

    permanent_markers = ("requires iOS", "not supported", "unsupported")
    permanent = code == 9021 or any(k.lower() in message.lower() for k in permanent_markers)
    return message, code, permanent


def _png_size(data: bytes):
    """Width/height straight out of a PNG IHDR, so we can report a real screen size."""
    if len(data) >= 24 and data[:8] == b"\x89PNG\r\n\x1a\n":
        w, h = struct.unpack(">II", data[16:24])
        return int(w), int(h)
    return None


class Recorder:
    """Writes the live stream to an Annex-B file while it is also being viewed.

    Opens with the cached parameter sets so the file is self-describing, then takes every NAL
    as it arrives. Nothing is filtered: the device only emits an IRAP every ~10 seconds, so
    skipping until a keyframe could leave an empty file for a short recording. Decoders discard
    the leading partial GOP by themselves — `saw_keyframe` reports whether one arrived, which is
    what tells you the file starts cleanly.
    """

    def __init__(self, path, param_sets, wait_for_keyframe=False, codec="hevc"):
        self.codec = codec
        self.path = path
        self._f = open(path, "wb")
        self.nals = 0
        self.bytes = 0
        self.started_at = time.time()
        self.saw_keyframe = False
        # With wait_for_keyframe the file is guaranteed decodable from its first frame, at the
        # cost of starting up to a GOP late (~10s on this device). Off by default so a short
        # recording is never empty.
        self.wait_for_keyframe = wait_for_keyframe
        self.skipped = 0
        for t in PARAM_SET_ORDER[codec]:
            if t in param_sets:
                self._write(param_sets[t])

    def _write(self, nal):
        self._f.write(_ANNEXB + nal)
        self.nals += 1
        self.bytes += len(nal) + 4

    def write(self, nal):
        t, is_param, is_key = _classify(nal, self.codec)
        if is_key:
            self.saw_keyframe = True
        elif self.wait_for_keyframe and not self.saw_keyframe and not is_param:
            self.skipped += 1        # a VCL NAL with no keyframe to anchor it yet
            return
        self._write(nal)

    def stats(self):
        return {"path": self.path, "nals": self.nals, "bytes": self.bytes,
                "duration_s": round(time.time() - self.started_at, 2),
                "saw_keyframe": self.saw_keyframe,
                "skipped_before_keyframe": self.skipped,
                "waiting_for_keyframe": self.wait_for_keyframe and not self.saw_keyframe}

    def close(self):
        s = self.stats()
        try:
            self._f.close()
        except OSError:
            pass
        return s


class VideoBroadcaster:
    """Fan Annex-B HEVC out to every connected viewer.

    Two rules that matter for a live stream: a viewer that joins late gets the cached parameter
    sets first (otherwise its decoder has nothing to configure itself with and shows nothing),
    and a viewer that cannot keep up gets dropped rather than blocking the capture thread.
    """

    def __init__(self, on_viewer=None, codec="hevc"):
        self.codec = codec
        self._clients = []
        self._lock = threading.Lock()
        self._param_sets = {}          # nal_type -> latest VPS/SPS/PPS
        self._last_irap = None         # most recent keyframe, for late joiners
        self.last_irap_at = 0.0
        self.irap_count = 0
        self.nal_count = 0
        self.recorder = None           # set while recording; fed from publish()
        self._on_viewer = on_viewer    # called when someone connects, to ask for a keyframe

    # --- recording ---
    def start_recording(self, path, wait_for_keyframe=False):
        with self._lock:
            if self.recorder:
                raise ValueError(f"already recording to {self.recorder.path}")
            self.recorder = Recorder(path, dict(self._param_sets), wait_for_keyframe,
                                     self.codec)
            return self.recorder.stats()

    def stop_recording(self):
        with self._lock:
            if not self.recorder:
                raise ValueError("not recording")
            rec, self.recorder = self.recorder, None
        return rec.close()

    def add_client(self, sock):
        """Attach a viewer, giving it everything a decoder needs to start.

        Parameter sets alone are NOT enough. Without a keyframe the decoder has nothing to anchor
        on and every following P-frame references pictures it never saw — the window just stays
        black. That is exactly what happened on the first live run. So a late joiner also gets the
        most recent keyframe, and `on_viewer` asks for a fresh one, because a cached keyframe from
        30 seconds ago decodes to a stale picture and then degrades.
        """
        sock.settimeout(5.0)
        with self._lock:
            parts = [_ANNEXB + self._param_sets[t]
                     for t in PARAM_SET_ORDER[self.codec] if t in self._param_sets]
            # Only a RECENT keyframe is worth sending. An old one decodes to a stale picture and
            # then every following P-frame references frames the viewer never got, which looks
            # like heavy artifacting. Better to send nothing and let the forced keyframe arrive.
            if self._last_irap and (time.monotonic() - self.last_irap_at) < 2.0:
                parts.append(_ANNEXB + self._last_irap)
            preamble = b"".join(parts)
            self._clients.append(sock)
        if preamble:
            try:
                sock.sendall(preamble)
            except OSError:
                self.drop(sock)
                return
        if self._on_viewer:
            try:
                self._on_viewer()
            except Exception as e:
                print(f"  (keyframe request failed: {e!r})")

    def drop(self, sock):
        with self._lock:
            if sock in self._clients:
                self._clients.remove(sock)
        try:
            sock.close()
        except OSError:
            pass

    def publish(self, nal: bytes):
        t, is_param, is_key = _classify(nal, self.codec)
        if is_param:
            with self._lock:
                self._param_sets[t] = nal
        elif is_key:
            with self._lock:
                self._last_irap = nal
                self.last_irap_at = time.monotonic()
                self.irap_count += 1
        self.nal_count += 1
        frame = _ANNEXB + nal
        with self._lock:
            targets = list(self._clients)
            rec = self.recorder
        if rec:
            try:
                rec.write(nal)
            except OSError as e:
                print(f"  recording stopped: {e!r}")
                with self._lock:
                    self.recorder = None
        for c in targets:
            try:
                c.sendall(frame)
            except OSError:
                self.drop(c)

    @property
    def viewers(self):
        with self._lock:
            return len(self._clients)


class MirrorCore:
    """One device: tunnel up, services found, media stream held open, control ready."""

    def __init__(self, udid=None, api_port=API_PORT, stream_port=STREAM_PORT, codec="auto"):
        self.transport = UsbmuxTransport(udid)
        self.api_port = api_port
        self.stream_port = stream_port
        # What we ASK for. What we get is read back off the RTP payload type, because the device
        # chooses from the banks we offer.
        self.codec_requested = codec
        self.restart_on_viewer = False
        self.stream_at_start = False
        self.stream_unavailable = None      # set to a message when retrying cannot help
        self._stream_lock = threading.Lock()
        self.codec = "hevc" if codec in ("auto", "hevc") else codec
        self._payload_types = {}     # every payload type seen, RTCP included, with counts
        self._seen_video_pt = set()
        self._samples = []
        self._nal_types = {}
        self.rtcp_packets = 0

        self.link = None
        self.tun = None
        self.pump = None
        self.catalog = None
        self.stream = None
        self.touch = None
        self.video = VideoBroadcaster(on_viewer=self._request_keyframe, codec=self.codec)
        self._depacketize = screen.depacketizer_for(self.codec)
        self.screen_size = None
        self._keyframe_lock = threading.Lock()
        self._last_restart = 0.0
        self._health = []              # rolling stream-health samples, also written to the report
        self._restarts = 0
        # ~2 seconds of headroom at a few thousand packets/s. Bounded on purpose: for live video a
        # backlog is worse than a gap, so the oldest packet is discarded rather than blocking.
        self._rtp_q = queue.Queue(maxsize=4096)
        self.queue_drops = 0
        self.started_at = None
        self._stop = threading.Event()
        self._reorder = screen.ReorderBuffer(window=256)
        self._lost_at_frame_start = 0
        self._fu = bytearray()

    # ---------------- bringup ----------------
    def start(self):
        if not self.transport.wait_available(timeout=15):
            raise SystemExit(f"device {self.transport.udid or '(any)'} is not visible to "
                             f"usbmuxd — wake it, or check the cable / wifi sync")

        print(f"transport: {self.transport.describe()}")
        self.link = self.transport.open()
        p = self.link.params
        print(f"tunnel: {p}")

        self.tun = open_tun()
        self.tun.configure(p)
        print(f"tun: {self.tun.name}")

        self.pump = TunnelPump(self.tun, self.link, on_fail=self._on_link_down).start()
        time.sleep(0.5)                      # let the interface settle before we dial RSD

        self.catalog = rsd.ServiceCatalog.fetch(p.dev_addr, p.rsd_port)
        print(f"rsd: {self.catalog.summary()}")
        self._write_report()        # captured now, so a refusal below is still on disk

        # displayservice is load-bearing twice over: it is the video, and it is the HID auth
        # gate (the device only routes touch to UIKit while a media stream runs). Without it
        # there is nothing to mirror and nothing to tap, so stop and say so plainly.
        if not self.catalog.has(rsd.DISPLAY_SVC):
            os_version = self.catalog.properties.get("OSVersion", "?")
            raise SystemExit(
                f"\nthis device does not advertise {rsd.DISPLAY_SVC}\n"
                f"  iOS {os_version} — screen mirroring is not available here, and neither is\n"
                f"  touch, because touch is only routed while a media stream runs.\n"
                f"  Run `sudo python3 host/rsd_probe.py {self.transport.udid} --all` to see the\n"
                f"  full service catalog for this device.")

        self._open_touch()
        self._probe_screen_size()
        if self.stream_at_start:
            self._open_media_stream()
        else:
            # Deliberately NOT started yet. Measured on this device: exactly one IDR is emitted, at
            # stream start, and RTCP PLI and FIR are both ignored (8 of each produced no keyframe).
            # A viewer that connects after the stream has begun therefore has nothing it can ever
            # decode from. So the stream starts when the first viewer arrives, which puts it in
            # place for the parameter sets and that single IDR.
            print("  media stream: deferred until a viewer connects (the device sends its only "
                  "IDR at stream start)")

        self.started_at = time.time()
        self._write_report()
        threading.Thread(target=self._serve_stream, name="stream-server", daemon=True).start()
        threading.Thread(target=self._serve_api, name="api-server", daemon=True).start()
        threading.Thread(target=self._heartbeat, name="health", daemon=True).start()
        threading.Thread(target=self._rtp_worker, name="rtp-worker", daemon=True).start()
        print(f"\nlive. video: ffplay -fflags nobuffer -flags low_delay "
              f"tcp://127.0.0.1:{self.stream_port}")
        print(f"      control: 127.0.0.1:{self.api_port} (JSON lines)")

    def _open_media_stream(self):
        """Start the screen stream and keep it running — this also opens the HID auth gate.

        The displayservice port is re-resolved every time rather than reused from the catalog
        captured at startup. Measured: a `startmediastream` issued seconds later times out, and it
        does so both when the stream is deferred to a viewer's arrival and when it is restarted —
        i.e. the failure tracks elapsed time, not the reason for the call. A stale RSD port is the
        simplest explanation, so ask RSD again.
        """
        p = self.link.params
        port = self._resolve_display_port()
        self.stream = screen.open_stream(p.dev_addr, p.our_addr, port,
                                         on_packet=self._on_rtp,
                                         codec=self.codec_requested)
        time.sleep(1.2)                      # the gate takes a moment to open
        print(f"  media stream up ({self.stream['stats']['packets']} RTP packets so far)")

    def _diagnose_services(self):
        """Say which coredevice services still answer, so a wedged one is obvious.

        Measured failure mode: the tunnel, RSD, screencaptureservice and universalhidservice all
        work perfectly while displayservice alone stops accepting TCP connections. Nothing on the
        host can revive it — the device has to be rebooted. Distinguishing that from "the tunnel
        died" saves a lot of pointless debugging.
        """
        import socket as _socket
        results = {}
        for name in (rsd.SCREENSHOT_SVC, rsd.HID_SVC, rsd.DISPLAY_SVC):
            port = self.catalog.port(name)
            short = name.rsplit(".", 1)[-1]
            if port is None:
                results[short] = "not advertised"
                continue
            try:
                c = _socket.create_connection((self.link.params.dev_addr, int(port)), timeout=5)
                c.close()
                results[short] = f"port {port} OK"
            except Exception as e:
                results[short] = f"port {port} UNREACHABLE ({type(e).__name__})"
        print("    service reachability:")
        for k, v in results.items():
            print(f"      {k:24} {v}")
        display = results.get(rsd.DISPLAY_SVC.rsplit(".", 1)[-1], "")
        others_ok = all("OK" in v for k, v in results.items()
                        if k != rsd.DISPLAY_SVC.rsplit(".", 1)[-1])
        if "UNREACHABLE" in display and others_ok:
            print("    → displayservice is wedged on the DEVICE: everything else answers, so the")
            print("      tunnel and RSD are fine. Nothing here can revive it. REBOOT THE PHONE.")

    def _media_server_status(self):
        """Ask displayservice what its media server thinks is going on.

        Identifiers taken from CoreDeviceUtilities, which holds the whole feature surface. This is
        the question worth asking when startmediastream times out: the device allows one stream at
        a time, so a stale one from an earlier run blocks every later session, and this says
        whether that is what is happening.
        """
        try:
            port = self.catalog.require(rsd.DISPLAY_SVC)
            svc = coredevice.CoreDeviceService(self.link.params.dev_addr, port)
        except Exception as e:
            return f"could not reach displayservice: {e!r}"
        try:
            out = svc.invoke("com.apple.coredevice.feature.getmediastreamserverstatus", {},
                             action_identifier="com.apple.coredevice.action.mediastreamstatus")
            return repr(out)
        except Exception as e:
            return f"getmediastreamserverstatus failed: {e!r}"
        finally:
            try:
                svc.close()
            except Exception:
                pass

    def _resolve_display_port(self):
        """Current displayservice port. Falls back to the startup catalog if RSD is unreachable."""
        cached = self.catalog.port(rsd.DISPLAY_SVC)
        try:
            fresh = rsd.ServiceCatalog.fetch(self.link.params.dev_addr, self.link.params.rsd_port)
        except Exception as e:
            print(f"  (could not re-query RSD: {e!r} — using the cached port {cached})")
            return self.catalog.require(rsd.DISPLAY_SVC)
        port = fresh.port(rsd.DISPLAY_SVC)
        if port is None:
            print(f"  (RSD no longer advertises displayservice — using the cached port {cached})")
            return self.catalog.require(rsd.DISPLAY_SVC)
        if str(port) != str(cached):
            print(f"  displayservice port moved {cached} -> {port} (that is why a later "
                  f"startmediastream timed out)")
        self.catalog = fresh          # keep every other service port fresh too
        return int(port)

    def _ensure_stream(self, why):
        """Open the media stream if it is not already running. Safe to call from any thread."""
        with self._stream_lock:
            if self.stream is not None:
                return False
            print(f"  starting the media stream ({why})")
            self._reorder = screen.ReorderBuffer(window=256)
            self._lost_at_frame_start = 0
            self._fu = bytearray()
            try:
                self._open_media_stream()
                return True
            except Exception as e:
                message, code, permanent = _describe_device_error(e)
                print(f"  ✗ could not start the media stream: {message}"
                      + (f" (code {code})" if code else ""))
                if permanent:
                    # Retrying a permanent refusal only spams. The device has told us the answer.
                    self.stream_unavailable = message
                    print("    this will not succeed by retrying — not trying again")
                    return False
                print(f"    media server status: {self._media_server_status()}")
                self._diagnose_services()
                # A half-negotiated stream still occupies the device's single slot, so tear it
                # down rather than leaving it to expire and block the next session.
                stale, self.stream = self.stream, None
                if stale:
                    try:
                        screen.close_stream(stale)
                        print("    (tore down the half-started stream so the device slot frees)")
                    except Exception:
                        pass
                return False

    def _request_keyframe(self):
        """A viewer connected and needs a keyframe it can decode from.

        Both PLI and FIR are sent, and retried, because measurement showed neither one shot is
        enough: a single PLI produced no new IDR across 20 seconds, and a viewer that connects
        while the first IDR is still being reassembled from FU fragments has nothing to start
        from. So keep asking until a keyframe actually appears.
        """
        if self._ensure_stream("viewer connected"):
            # Started fresh for this viewer: it will receive the parameter sets and the initial
            # IDR as the stream's first packets, which is the only reliable way in on this device.
            return

        # A viewer that joins an already-running stream missed the one keyframe the device
        # ever sends, and there is no way to make another: PLI and FIR are ignored, and
        # restarting the media stream does not work — measured repeatedly, the FIRST
        # startmediastream of a session succeeds and later ones time out, leaving no stream at
        # all. Restarting therefore turns "this viewer is blind" into "nobody gets video", which
        # is strictly worse. Say so instead.
        if self.video.irap_count > 0:
            print("  viewer joined an already-running stream, so it missed the only keyframe the "
                  "device sends. It cannot decode. Restart the engine to give it a fresh stream.")

        rtcp = self.stream.get("rtcp") if self.stream else None
        if rtcp is None:
            return
        # Already running, so this viewer missed the IDR. Ask anyway — it costs nothing — but the
        # retry loop will report honestly that the device ignores it.
        threading.Thread(target=self._keyframe_retry_loop, args=(rtcp,),
                         name="keyframe-request", daemon=True).start()

        if self.restart_on_viewer:
            now = time.monotonic()
            with self._keyframe_lock:
                if now - self._last_restart < 5.0:
                    return
                self._last_restart = now
            threading.Thread(target=self._restart_media_stream,
                             name="keyframe-restart", daemon=True).start()

    def _keyframe_retry_loop(self, rtcp, attempts=8, interval=1.5):
        """Ask for a keyframe until one arrives, then stop. Reports what worked."""
        start_count = self.video.irap_count
        for i in range(1, attempts + 1):
            if self._stop.is_set() or self.video.irap_count > start_count:
                if self.video.irap_count > start_count:
                    print(f"  keyframe arrived after {i - 1} request(s) "
                          f"(pli={rtcp.pli_sent} fir={rtcp.fir_sent})")
                return
            pli = rtcp.send_pli()
            fir = rtcp.send_fir()
            if not (pli or fir):
                print("  cannot request a keyframe yet (no RTP seen, so no peer/SSRC)")
            elif i == 1:
                print(f"  viewer joined — requesting a keyframe (PLI + FIR)")
            time.sleep(interval)
        if self.video.irap_count == start_count:
            print(f"  ⚠ no keyframe after {attempts} PLI+FIR requests — the device is ignoring "
                  f"RTCP feedback; a viewer joining between IDRs cannot start decoding")

    def _restart_media_stream(self):
        """Stop and re-negotiate the media stream, which is what produces a fresh keyframe."""
        if self._stop.is_set():
            return
        self._restarts += 1
        print("  restarting the media stream (experimental — this has killed the stream before)")
        old, self.stream = self.stream, None
        if old:
            try:
                screen.close_stream(old)
            except Exception as e:
                print(f"  (stopping the old stream: {e!r})")
        self._reorder = screen.ReorderBuffer(window=256)
        self._lost_at_frame_start = 0
        self._fu = bytearray()
        try:
            self._open_media_stream()
        except Exception as e:
            print(f"  ✗ could not restart the media stream: {e!r}")

    def _open_touch(self):
        """Optional: video without input is still a useful mirror, so degrade rather than die."""
        if not self.catalog.has(rsd.HID_SVC):
            print(f"  no {rsd.HID_SVC} — view-only, tap/swipe will be refused")
            return
        port = self.catalog.require(rsd.HID_SVC)
        self.touch = hid.UniversalHID(self.link.params.dev_addr, port)
        print("  touch channel open")

    def _probe_screen_size(self):
        """One screenshot at startup gives exact device pixels for list_devices."""
        if not self.catalog.has(rsd.SCREENSHOT_SVC):
            print("  no screencaptureservice — clients must send fx/fy fractions")
            return
        try:
            png = self._screenshot_bytes("png")
            self.screen_size = _png_size(png)
            print(f"  screen size: {self.screen_size}")
        except Exception as e:
            print(f"  (could not determine screen size: {e!r} — clients must send fx/fy)")

    def _heartbeat(self, period=2.0):
        """Sample stream health continuously and say so out loud.

        A frozen picture and a healthy pipeline look identical in a startup-only log, which is
        why the first freeze could not be diagnosed after the fact. Each sample records RTP and
        NAL rates; the samples go into the JSON report too, so a run can be examined once it is
        over rather than only while it is happening.
        """
        prev_rtp = prev_nals = 0
        stalls = 0
        while not self._stop.is_set():
            time.sleep(period)
            if self._stop.is_set():
                break
            if self.stream is None and self.video.viewers > 0 and not self.stream_unavailable:
                # A failed start used to leave the engine permanently without a stream. Retry.
                self._ensure_stream("retrying after a failed start")
            rtp = self.stream["stats"]["packets"] if self.stream else 0
            nals = self.video.nal_count
            d_rtp, d_nals = rtp - prev_rtp, nals - prev_nals
            prev_rtp, prev_nals = rtp, nals

            age = (round(time.monotonic() - self.video.last_irap_at, 1)
                   if self.video.last_irap_at else None)
            sample = {"t": round(time.time() - self.started_at, 1),
                      "rtp_per_s": round(d_rtp / period, 1),
                      "nals_per_s": round(d_nals / period, 1),
                      "viewers": self.video.viewers,
                      "keyframes": self.video.irap_count,
                      "keyframe_age_s": age,
                      "rtp_lost": self._reorder.lost,
                      "rtp_late": self._reorder.late,
                      "loss_pct": self._reorder.loss_pct,
                      "queue_drops": self.queue_drops,
                      "rtcp": self.rtcp_packets,
                      "rr_sent": (self.stream.get("rtcp").rr_sent
                                  if self.stream and self.stream.get("rtcp") else 0),
                      "pli_sent": (self.stream.get("rtcp").pli_sent
                                   if self.stream and self.stream.get("rtcp") else 0),
                      "fir_sent": (self.stream.get("rtcp").fir_sent
                                   if self.stream and self.stream.get("rtcp") else 0),
                      # The one that matters: LTR acks are this device's loss recovery.
                      "ltr_acked": (self.stream.get("rtcp").ltr_acked
                                    if self.stream and self.stream.get("rtcp") else 0),
                      "payload_types": dict(self._payload_types),
                      "stream": bool(self.stream)}
            self._health.append(sample)
            del self._health[:-120]        # keep the last ~4 minutes
            self._write_report()           # a clean run must leave evidence too

            if d_rtp == 0:
                stalls += 1
                print(f"  ⚠ no RTP for {stalls * period:.0f}s — the device stopped sending "
                      f"(stream {'up' if self.stream else 'DOWN'}, "
                      f"{self.video.viewers} viewer(s))")
                if stalls * period >= 6:
                    self._write_report()
                if stalls == 3:
                    # One probe, once: is the device still there, and is its screen on? A
                    # screenshot uses a different service from the media stream, so it separates
                    # "the tunnel/device died" from "there is nothing to capture because the
                    # display slept" — an idle locked iPhone sleeps its screen within ~30s, which
                    # matches the stream stopping after a varying 17-35s.
                    threading.Thread(target=self._probe_stall, name="stall-probe",
                                     daemon=True).start()
            else:
                if stalls:
                    print(f"  RTP resumed after {stalls * period:.0f}s")
                stalls = 0
                print(f"  health: {sample['rtp_per_s']:.0f} RTP/s  "
                      f"{sample['nals_per_s']:.0f} NAL/s  "
                      f"loss {self._reorder.loss_pct}%  "
                      f"rr={sample['rr_sent']} pli={sample['pli_sent']} "
                      f"fir={sample['fir_sent']}  "
                      f"{'' if not self.queue_drops else f'qdrop {self.queue_drops}  '}"
                      f"{self.video.viewers} viewer(s)  "
                      f"{self.video.irap_count} keyframe(s)"
                      f"{'' if age is None else f', last {age}s ago'}")
                if self._nal_types:
                    top = sorted(self._nal_types.items(), key=lambda kv: -kv[1])[:6]
                    print(f"    nal types ({self.codec}): "
                          + "  ".join(f"{t}×{n}" for t, n in top))
                if self.stream_unavailable:
                    print(f"    video unavailable: {self.stream_unavailable}")
                elif not self._seen_video_pt:
                    # Every packet so far has been RTCP: the device accepted the negotiation but is
                    # sending no video. That is a codec-negotiation problem, not a decode problem.
                    print(f"    ⚠ NO VIDEO packets yet — {self.rtcp_packets} RTCP only, "
                          f"payload types seen {sorted(self._payload_types)}")

    def _probe_stall(self):
        """Called once when RTP stalls: says whether the device is alive and its screen lit."""
        try:
            png = self._screenshot_bytes("png")
        except Exception as e:
            print(f"    stall probe: screenshot FAILED ({e!r}) — the device or tunnel is gone, "
                  f"not just the video stream")
            return
        size = _png_size(png) or (0, 0)
        # A slept display screenshots as a uniform dark image; a lit one compresses far worse.
        density = len(png) / max(1, size[0] * size[1])
        print(f"    stall probe: device is ALIVE — screenshot {size[0]}x{size[1]}, "
              f"{len(png)} bytes ({density:.4f} B/px)")
        if density < 0.02:
            print("    the screenshot is nearly blank, so the phone's display has slept and "
                  "displayservice has nothing to capture. Wake the phone, or set "
                  "Settings > Display & Brightness > Auto-Lock to Never while testing.")
        else:
            print("    the display is ON and the device is responsive, so the media stream itself "
                  "stopped — a stream-lifetime problem, not a sleeping screen.")

    def _write_report(self):
        """Snapshot what this run found. Overwritten as the run progresses."""
        if not self.catalog:
            return
        p = self.link.params
        try:
            path = write_report(f"engine-{self.transport.udid}.json", {
                "udid": self.transport.udid,
                "transport": self.link.transport_name,
                "tunnel": {"device": p.dev_addr, "us": p.our_addr,
                           "rsd_port": p.rsd_port, "mtu": p.mtu},
                "tun": getattr(self.tun, "name", None),
                "device": self.catalog.device_info(),
                "service_count": len(self.catalog),
                "needed": {n: self.catalog.port(n) for n in rsd.CORE_SERVICES},
                "missing_core": self.catalog.missing_core(),
                "screen_size": self.screen_size,
                "touch": self.touch is not None,
                "rtp_packets": self.stream["stats"]["packets"] if self.stream else 0,
                "nals": self.video.nal_count,
                "keyframes": self.video.irap_count,
                "restarts": self._restarts,
                "rtcp_packets": self.rtcp_packets,
                "payload_types": dict(self._payload_types),
                "nal_types": dict(self._nal_types),
                "first_payloads": list(self._samples),
                "started": self.started_at,
                "health": list(self._health),
            })
            print(f"  report: {path}")
        except Exception as e:
            print(f"  (could not write report: {e!r})")

    def _on_link_down(self, reason):
        # Reconnect belongs here next; for now say so loudly rather than looking alive.
        print(f"\n!! tunnel died: {reason}\n   (no reconnect yet — restart the process)")
        self._stop.set()

    # ---------------- video ----------------
    def _on_rtp(self, pkt):
        """Runs on the UDP drain thread. Do as little as possible here.

        Everything downstream — depacketizing, fanning out to TCP viewers, writing the recording —
        used to happen on this thread. A viewer whose socket blocked for even a moment therefore
        stalled the UDP reader, and the kernel dropped packets, which shows up as snow and garbled
        frames. So this just hands the packet to a worker and returns.

        The queue is bounded and drops the OLDEST packet when full: for live video, stale packets
        are worthless, and blocking here would recreate the very problem this avoids.
        """
        try:
            self._rtp_q.put_nowait(pkt)
        except queue.Full:
            try:
                self._rtp_q.get_nowait()
                self._rtp_q.put_nowait(pkt)
                self.queue_drops += 1
            except (queue.Empty, queue.Full):
                self.queue_drops += 1

    def _rtp_worker(self):
        """Depacketize and fan out, off the UDP reader's back."""
        while not self._stop.is_set():
            try:
                pkt = self._rtp_q.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self._process_rtp(pkt)
            except Exception as e:
                print(f"  (RTP processing error: {e!r})")

    def _stream_mbps(self):
        """Received video bitrate since the stream opened, in Mbit/s."""
        if not self.stream:
            return 0.0
        st = self.stream.get("stats") or {}
        elapsed = time.time() - st.get("started", 0)
        if elapsed <= 0:
            return 0.0
        return round(st.get("bytes", 0) * 8 / elapsed / 1e6, 2)

    def _process_rtp(self, pkt):
        payload, seq, is_rtcp, pt = screen.parse_rtp(pkt)
        if pt >= 0:
            self._payload_types[pt] = self._payload_types.get(pt, 0) + 1
        if is_rtcp:
            self.rtcp_packets += 1
            return
        if not payload:
            return
        if len(self._samples) < 8:
            # The first video payloads verbatim. One NAL per packet where fragmentation was
            # expected means the payload structure is not what the depacketizer assumes, and the
            # only way to settle that is to look at the bytes. Printed as well as recorded,
            # because a report written before the video starts captures nothing.
            first = payload[0] if payload else 0
            self._samples.append({"pt": pt, "seq": seq, "len": len(payload),
                                  "first16": payload[:16].hex()})
            print(f"  rtp[{len(self._samples)}] pt={pt} seq={seq} len={len(payload)} "
                  f"h264_type={first & 0x1F} hevc_type={(first >> 1) & 0x3F} "
                  f"bytes={payload[:12].hex()}")
        if pt not in self._seen_video_pt:
            self._seen_video_pt.add(pt)
            # Do NOT infer the codec from the RTP payload type. Measured on this device: it sends
            # HEVC under payload type 100, which is the number our offer used for the AVC bank.
            # Trusting the number switched the depacketizer to H.264 and shredded a stream that
            # had been decoding correctly. The payload structure is the only reliable signal.
            detected = screen.sniff_codec(payload)
            if detected and detected != self.codec:
                print(f"  codec: payload structure says {detected.upper()} "
                      f"(RTP payload type {pt}, requested {self.codec_requested}) — switching")
                self.codec = detected
                self.video.codec = detected
                self._depacketize = screen.depacketizer_for(detected)
            else:
                print(f"  codec: {self.codec.upper()} on RTP payload type {pt}"
                      f"{'' if detected else ' (structure inconclusive — keeping the request)'}")

        ordered = []
        self._reorder.push(seq, payload, ordered)    # RTP arrives out of order
        for p in ordered:
            nals = []
            self._depacketize(p, self._fu, nals)
            for nal in nals:
                t = _nal_type(nal, self.codec)
                self._nal_types[t] = self._nal_types.get(t, 0) + 1
                self.video.publish(nal)

        # The marker bit ends a frame, so this is the moment to acknowledge it. Acking is what
        # keeps this stream decodable: the encoder emits one IDR and then predicts forever, and it
        # re-anchors on frames we confirm rather than on frames it merely hopes arrived.
        #
        # Only intact frames are acked. Acknowledging a frame we received with a hole would point
        # the encoder at a reference picture we cannot reconstruct, which corrupts everything
        # afterwards — the exact failure LTR exists to prevent. The reorder buffer's running loss
        # count is the test: unchanged across this frame means nothing went missing.
        if pkt[1] & 0x80:
            rtcp = self.stream.get("rtcp") if self.stream else None
            lost_now = self._reorder.lost
            # Two conditions, both about only ever acking a frame we really hold. No loss was
            # recorded while this frame was arriving, and the reorder buffer has nothing still
            # held back — a non-empty buffer means some earlier packet has not been released yet,
            # so the frame is not actually complete however final this packet looks.
            intact = lost_now == self._lost_at_frame_start and not self._reorder.buf
            if rtcp is not None and intact:
                rtcp.send_ltr_ack(int.from_bytes(pkt[4:8], "big"))
            self._lost_at_frame_start = lost_now

    def _serve_stream(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.stream_port))
        srv.listen(8)
        srv.settimeout(1.0)
        while not self._stop.is_set():
            try:
                c, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            print(f"  viewer connected ({self.video.viewers + 1} total)")
            self.video.add_client(c)

    # ---------------- control ----------------
    def _screenshot_bytes(self, fmt="png"):
        port = self.catalog.require(rsd.SCREENSHOT_SVC)
        svc = coredevice.CoreDeviceService(self.link.params.dev_addr, port)
        try:
            out = svc.invoke(
                "com.apple.coredevice.feature.capturescreenshot",
                {"displayUniqueID": None, "requestedFormat": fmt},
                action_identifier="com.apple.coredevice.action.capturescreenshot",
            )
        finally:
            svc.close()
        img = out.get("image")
        if not img:
            raise RuntimeError(f"no image in screenshot output: {list(out)}")
        return img

    def _default_recording_path(self):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        return os.path.join(root, "recordings", f"{self.transport.udid}-{stamp}.h265")

    def _fractions(self, params, xkey="x", ykey="y"):
        """Accept normalized fx/fy, or pixels resolved against the real screen size.

        ~/rplay's clients send pixels taken from list_devices' screen_size, so both work.
        """
        clamp = lambda v: min(max(float(v), 0.0), 1.0)
        fx, fy = params.get("f" + xkey), params.get("f" + ykey)
        if fx is not None and fy is not None:
            # Clamp here too: the HID report masks x/y to 16 bits, so an out-of-range
            # fraction would silently land somewhere else on screen instead of erroring.
            return clamp(fx), clamp(fy)
        x, y = params.get(xkey), params.get(ykey)
        if x is None or y is None:
            raise ValueError(f"need {xkey}/{ykey} in pixels or f{xkey}/f{ykey} in 0..1")
        if not self.screen_size:
            raise ValueError("screen size unknown — send f%s/f%s in 0..1 instead"
                             % (xkey, ykey))
        w, h = self.screen_size
        return clamp(float(x) / w), clamp(float(y) / h)

    def dispatch(self, method, params):
        """One request → result dict. Raises ValueError for bad_request."""
        if method == "ping":
            return {"pong": True}

        if method == "list_devices":
            info = self.catalog.device_info()
            dev = {
                "id": info["udid"],
                "udid": info["udid"],
                "name": info["name"],
                "product_type": info["product_type"],
                "os_version": info["os_version"],
                "connected": self.pump.alive,
                "transport": self.link.transport_name,
                "codec": self.codec,
            }
            if self.screen_size:
                dev["screen_size"] = {"w": self.screen_size[0], "h": self.screen_size[1]}
            return {"devices": [dev]}

        if method == "take_screenshot":
            if not self.catalog.has(rsd.SCREENSHOT_SVC):
                raise ValueError("device does not advertise screencaptureservice")
            fmt = params.get("format", "png")
            img = self._screenshot_bytes("png")     # the device only gives us PNG
            size = _png_size(img) or (0, 0)
            if fmt != "png":
                # Be honest rather than silently returning the wrong format.
                raise ValueError(f"format '{fmt}' not supported — the device returns PNG")
            return {"format": "png", "width": size[0], "height": size[1],
                    "image_b64": base64.b64encode(img).decode()}

        if method == "tap":
            self._ensure_stream("tap needs the HID auth gate")
            if self.touch is None:
                raise ValueError("no HID service on this device — mirror is view-only")
            fx, fy = self._fractions(params)
            hold = int(params.get("duration_ms", 60))
            self.touch.tap(fx, fy, hold_ms=hold)
            return {"x": int(fx * LOGICAL_MAX), "y": int(fy * LOGICAL_MAX),
                    "logical_max": LOGICAL_MAX}

        if method == "swipe":
            self._ensure_stream("swipe needs the HID auth gate")
            if self.touch is None:
                raise ValueError("no HID service on this device — mirror is view-only")
            fx0, fy0 = self._fractions(params, "x0", "y0")
            fx1, fy1 = self._fractions(params, "x1", "y1")
            ms = int(params.get("duration_ms", 300))
            self.touch.swipe(fx0, fy0, fx1, fy1, ms=ms)
            return {"from": [int(fx0 * LOGICAL_MAX), int(fy0 * LOGICAL_MAX)],
                    "to": [int(fx1 * LOGICAL_MAX), int(fy1 * LOGICAL_MAX)],
                    "logical_max": LOGICAL_MAX}

        if method == "start_recording":
            path = params.get("path") or self._default_recording_path()
            d = os.path.dirname(os.path.abspath(path))
            if d:
                os.makedirs(d, exist_ok=True)
            wait = bool(params.get("wait_for_keyframe", False))
            info = self.video.start_recording(path, wait)
            print(f"  recording -> {path}"
                  f"{' (waiting for a keyframe first)' if wait else ''}")
            return info

        if method == "stop_recording":
            info = self.video.stop_recording()
            print(f"  recorded {info['nals']} NALs, {info['bytes']} bytes in "
                  f"{info['duration_s']}s -> {info['path']}"
                  f"{'' if info['saw_keyframe'] else '  (NO KEYFRAME — see below)'}")
            if not info["saw_keyframe"]:
                print("   the device sends an IRAP only every ~10s, so a short recording can")
                print("   miss one; the file will start mid-GOP. RTCP PLI would fix this.")
            return info

        if method == "hid_surfaces":
            # The HID surfaces the device advertises. This is the RE step needed for a Home button:
            # we only know the touchscreen surface (257) and its 58-byte report so far.
            if self.touch is None:
                raise ValueError("no HID service on this device")
            return {"surfaces": repr(self.touch.list_surfaces())}

        if method == "press_button":
            button = str(params.get("button", "")).lower()
            if self.touch is None:
                raise ValueError("no HID service on this device — mirror is view-only")
            # Deliberately not faked. Touch works because the mainTouchscreen report layout was
            # reverse-engineered; Home needs the button/consumer surface and its own report, which
            # we have not established. Call `hid_surfaces` to see what the device offers.
            raise ValueError(
                f"press_button({button or '?'}) is not implemented: the HID report layout for "
                f"buttons is not known yet. `hid_surfaces` lists what the device advertises; only "
                f"the touchscreen surface ({hid.MAIN_TOUCHSCREEN}) is understood so far.")

        if method == "media_status":
            return {"status": self._media_server_status()}

        if method == "stream_info":
            rec = self.video.recorder
            age = (round(time.monotonic() - self.video.last_irap_at, 1)
                   if self.video.last_irap_at else None)
            return {"port": self.stream_port, "codec": self.codec, "container": "annexb",
                    "codec_requested": self.codec_requested,
                    "viewers": self.video.viewers, "nals": self.video.nal_count,
                    "rtp_packets": self.stream["stats"]["packets"] if self.stream else 0,
                    "uptime_s": round(time.time() - self.started_at, 1),
                    # Keyframe health is the first thing to check when a viewer shows black.
                    "keyframes": self.video.irap_count,
                    "last_keyframe_age_s": age,
                    "rtp_lost": self._reorder.lost,
                    "rtp_late": self._reorder.late,
                    "rtp_duplicates": self._reorder.duplicates,
                    "loss_pct": self._reorder.loss_pct,
                    "queue_drops": self.queue_drops,
                    # LTR acks are this device's loss recovery; if this is not climbing with the
                    # frame count, the encoder has nothing to re-anchor on.
                    "ltr_acked": (self.stream.get("rtcp").ltr_acked
                                  if self.stream and self.stream.get("rtcp") else 0),
                    # The number that actually predicts picture quality. Device Hub's own capture
                    # measured 2.35 Mbps average / 3.96 Mbps peak for this same 1184x2576 screen,
                    # so anything in that range is parity with Apple and the artefacting under
                    # motion is the encoder's, not ours. A visibly better picture needs this to go
                    # up — which is what RPLAY_TIER_SCALE exists to test.
                    "mbps": self._stream_mbps(),
                    "recording": rec.stats() if rec else None}

        raise KeyError(method)

    def _handle_line(self, line: bytes) -> dict:
        try:
            req = json.loads(line)
        except ValueError as e:
            return {"ok": False, "error": {"code": "bad_json", "message": str(e)}}
        if not isinstance(req, dict) or not isinstance(req.get("method"), str):
            return {"ok": False, "error": {"code": "bad_request",
                                           "message": "need a JSON object with a `method`"}}
        rid, method = req.get("id"), req["method"]
        params = req.get("params") or {}
        try:
            result = self.dispatch(method, params)
            resp = {"ok": True, "result": result}
        except KeyError:
            resp = {"ok": False, "error": {"code": "unsupported_method",
                                           "message": f"method '{method}' not implemented"}}
        except ValueError as e:
            resp = {"ok": False, "error": {"code": "bad_request", "message": str(e)}}
        except Exception as e:
            resp = {"ok": False, "error": {"code": "internal_error", "message": repr(e)}}
        if rid is not None:
            resp["id"] = rid
        return resp

    def _serve_api(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.api_port))       # localhost only — this is device control
        srv.listen(16)
        srv.settimeout(1.0)
        while not self._stop.is_set():
            try:
                c, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._api_conn, args=(c,), daemon=True).start()

    def _api_conn(self, sock):
        """One connection, newline-delimited JSON both ways. Requests are serialized per
        connection, which is what we want: the HID channel is not safe to interleave."""
        buf = b""
        try:
            while not self._stop.is_set():
                chunk = sock.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    resp = self._handle_line(line)
                    sock.sendall(json.dumps(resp).encode() + b"\n")
        except OSError:
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

    # ---------------- teardown ----------------
    def run_forever(self):
        try:
            while not self._stop.is_set():
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\ninterrupted")
        finally:
            self.close()

    def close(self):
        self._stop.set()
        if self.touch:
            try:
                self.touch.close()
            except Exception:
                pass
        if self.stream:
            try:
                screen.close_stream(self.stream)
            except Exception:
                pass
        if self.pump:
            self.pump.stop()
        if self.link:
            self.link.close()
        if self.tun:
            self.tun.close()
        print("torn down.")


def _install_signal_handlers(core):
    """Always release the device session, however we are asked to stop.

    This matters more than it looks: the device allows ONE media stream, and an engine that dies
    without calling stopmediastream leaves that slot occupied for the whole negotiated timeout,
    blocking every later session. Ctrl-C, launchctl unload and a plain kill must all tear down.
    """
    import atexit
    import signal

    done = threading.Event()

    def shutdown(signum=None, _frame=None):
        if done.is_set():
            return
        done.set()
        if signum:
            print(f"\nsignal {signum} — releasing the device session")
        try:
            core.close()
        except Exception as e:
            print(f"  (teardown error: {e!r})")
        if signum:
            raise SystemExit(0)

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        try:
            signal.signal(sig, shutdown)
        except (ValueError, OSError):
            pass          # not the main thread, or the platform disallows it
    atexit.register(shutdown)


def main(argv):
    args = argv[1:]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0

    codec = "auto"
    if "--codec" in args:
        i = args.index("--codec")
        try:
            codec = args[i + 1]
        except IndexError:
            print("--codec needs a value: auto | hevc | h264", file=sys.stderr)
            return 2
        del args[i:i + 2]
        if codec not in ("auto", "hevc", "h264"):
            print(f"--codec must be auto, hevc or h264 — not {codec!r}", file=sys.stderr)
            return 2

    stream_at_start = "--stream-at-start" in args
    if stream_at_start:
        args.remove("--stream-at-start")
    restart_on_viewer = "--restart-on-viewer" in args
    if restart_on_viewer:
        args.remove("--restart-on-viewer")

    udid = args[0] if args else None
    if not list_devices():
        print("no devices attached to usbmuxd", file=sys.stderr)
        return 1
    core = MirrorCore(udid, codec=codec)
    core.restart_on_viewer = restart_on_viewer
    core.stream_at_start = stream_at_start
    if restart_on_viewer:
        print("(--restart-on-viewer: known to kill the stream on this device — experimental)")
    _install_signal_handlers(core)
    core.start()
    core.run_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
