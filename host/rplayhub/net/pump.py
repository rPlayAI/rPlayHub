"""The packet pump — splice IPv6 packets between the tun device and the tunnel stream.

Fixes a real bug in the original tunnel_up.py splice(): when the tunnel died (the wifi path
drops when iOS sleeps the connection), one direction's thread returned and the other raised
inside a daemon thread, and nothing told the main program. The tunnel looked up but was dead.

Here, either direction ending marks the pump failed exactly once and fires on_fail, which is
what a session supervisor needs to trigger a reconnect. Liveness counters are exposed so a
watchdog can tell "idle" from "wedged".
"""
import struct
import threading
import time

IPV6_HDR_LEN = 40


class TunnelPump:
    def __init__(self, tun, link, on_fail=None):
        self.tun = tun
        self.link = link
        self.stream = link.stream
        self._on_fail = on_fail
        self._stop = threading.Event()
        self._failed = threading.Event()
        self.fail_reason = None
        self._lock = threading.Lock()
        self._threads = []
        self.stats = {"tx_packets": 0, "tx_bytes": 0, "rx_packets": 0, "rx_bytes": 0,
                      "last_rx": 0.0, "last_tx": 0.0}

    # --- lifecycle ---
    def start(self):
        for target, name in ((self._host_to_device, "pump-tx"),
                             (self._device_to_host, "pump-rx")):
            t = threading.Thread(target=self._guard(target), name=name, daemon=True)
            t.start()
            self._threads.append(t)
        return self

    def stop(self, join_timeout=2.0):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=join_timeout)

    @property
    def alive(self):
        return not self._failed.is_set() and not self._stop.is_set()

    def _guard(self, fn):
        def run():
            try:
                fn()
            except Exception as e:                    # never let it vanish silently
                self._fail(f"{fn.__name__}: {e!r}")
            else:
                if not self._stop.is_set():
                    self._fail(f"{fn.__name__} ended")
        return run

    def _fail(self, reason):
        with self._lock:
            if self._failed.is_set() or self._stop.is_set():
                return
            self._failed.set()
            self.fail_reason = reason
        if self._on_fail:
            try:
                self._on_fail(reason)
            except Exception:
                pass

    # --- directions ---
    def _host_to_device(self):
        """Host stack → phone. One tun read is exactly one IPv6 packet."""
        while not self._stop.is_set():
            pkt = self.tun.recv_packet(timeout=0.5)
            if pkt is None:
                continue
            self.stream.sendall(pkt)
            self.stats["tx_packets"] += 1
            self.stats["tx_bytes"] += len(pkt)
            self.stats["last_tx"] = time.monotonic()

    def _device_to_host(self):
        """Phone → host stack. The stream is a byte stream, so reframe from the IPv6 header.

        Payload Length covers everything after the 40-byte fixed header, extension headers
        included, so header+payload is the whole packet.
        """
        while not self._stop.is_set():
            hdr = self._read_exactly(IPV6_HDR_LEN)
            if hdr is None:
                return
            (plen,) = struct.unpack(">H", hdr[4:6])
            body = self._read_exactly(plen) if plen else b""
            if body is None:
                return
            self.tun.send_packet(hdr + body)
            self.stats["rx_packets"] += 1
            self.stats["rx_bytes"] += len(hdr) + len(body)
            self.stats["last_rx"] = time.monotonic()

    def _read_exactly(self, n):
        buf = b""
        while len(buf) < n:
            if self._stop.is_set():
                return None
            try:
                chunk = self.stream.recv(n - len(buf))
            except TimeoutError:
                continue
            except OSError:
                return None
            if not chunk:
                return None
            buf += chunk
        return buf
