#!/usr/bin/env python3
"""AFC (Apple File Conduit) over the tunnel: list and read files.

Two services speak it: `com.apple.afc.shim.remote` (the Media partition, /var/mobile/Media) and
`com.apple.crashreportcopymobile.shim.remote` (rooted at the crash-report directory; poke
`crashreportmover` first so pending reports are moved into it). Both need RSDCheckin first.

Packet: magic "CFA6LPAA", entire_len u64, this_len u64 (header+payload, excluding trailing data),
packet_num u64, operation u64, then payload; all little-endian. Reference: libimobiledevice afc.c.

Usage (daemon running):
    python3 host/afc.py ls [path]                 # Media partition
    python3 host/afc.py cat <path> [out]
    python3 host/afc.py crashes                   # list crash reports
    python3 host/afc.py crash <name> [out]        # pull one
"""
import socket
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import rsd
import diagnostics_relay as dr

AFC = "com.apple.afc.shim.remote"
CRASH_COPY = "com.apple.crashreportcopymobile.shim.remote"
CRASH_MOVER = "com.apple.crashreportmover.shim.remote"

MAGIC = b"CFA6LPAA"
OP_STATUS, OP_DATA, OP_READ_DIR, OP_GET_FILE_INFO = 0x01, 0x02, 0x03, 0x0A
OP_FILE_OPEN, OP_FILE_OPEN_RESULT, OP_FILE_READ, OP_FILE_CLOSE = 0x0D, 0x0E, 0x0F, 0x14
MODE_RDONLY = 1


class AFCClient:
    def __init__(self, service):
        info = dr.rpc("tunnel_info")["result"]
        self.addr = info["device_addr"]
        self.peer = rsd.connect(self.addr, info["rsd_port"])
        port = rsd.service_port(self.peer, service)
        self.s = socket.create_connection((self.addr, port), timeout=30)
        dr.rsd_checkin(self.s)
        self.seq = 0

    def _send(self, op, payload=b"", data=b""):
        hdr = struct.pack("<8sQQQQ", MAGIC, 40 + len(payload) + len(data), 40 + len(payload), self.seq, op)
        self.s.sendall(hdr + payload + data)
        self.seq += 1

    def _recv(self):
        hdr = dr._recvn(self.s, 40)
        magic, entire, this, _seq, op = struct.unpack("<8sQQQQ", hdr)
        assert magic == MAGIC, magic
        body = dr._recvn(self.s, entire - 40)
        return op, body

    def _call(self, op, payload=b"", data=b""):
        self._send(op, payload, data)
        rop, body = self._recv()
        if rop == OP_STATUS:
            (code,) = struct.unpack("<Q", body[:8])
            if code != 0:
                raise OSError(code, f"AFC error {code}")
            return None
        return rop, body

    def listdir(self, path):
        r = self._call(OP_READ_DIR, path.encode() + b"\0")
        names = r[1].split(b"\0")
        return [n.decode() for n in names if n and n not in (b".", b"..")]

    def stat(self, path):
        r = self._call(OP_GET_FILE_INFO, path.encode() + b"\0")
        parts = r[1].split(b"\0")
        return {parts[i].decode(): parts[i + 1].decode() for i in range(0, len(parts) - 1, 2)}

    def read(self, path):
        r = self._call(OP_FILE_OPEN, struct.pack("<Q", MODE_RDONLY) + path.encode() + b"\0")
        (handle,) = struct.unpack("<Q", r[1][:8])
        size = int(self.stat(path).get("st_size", 0))
        out = b""
        try:
            while len(out) < size:
                chunk = min(1 << 20, size - len(out))
                r = self._call(OP_FILE_READ, struct.pack("<QQ", handle, chunk))
                if not r or not r[1]:
                    break
                out += r[1]
        finally:
            self._call(OP_FILE_CLOSE, struct.pack("<Q", handle))
        return out


def poke_mover(addr, peer):
    """crashreportmover moves fresh reports into the copy directory; it answers 'ping' once done."""
    port = rsd.service_port(peer, CRASH_MOVER)
    s = socket.create_connection((addr, port), timeout=30)
    dr.rsd_checkin(s)
    print("mover:", dr._recvn(s, 4))
    s.close()


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["ls"]:
        c = AFCClient(AFC)
        for n in c.listdir(a[1] if len(a) > 1 else "/"):
            print(n)
    elif a[:1] == ["cat"]:
        c = AFCClient(AFC)
        data = c.read(a[1])
        open(a[2], "wb").write(data) if len(a) > 2 else sys.stdout.buffer.write(data[:2000])
        print(f"\n{len(data)} bytes", file=sys.stderr)
    elif a[:1] == ["crashes"]:
        c = AFCClient(CRASH_COPY)
        poke_mover(c.addr, c.peer)
        for n in sorted(c.listdir("/")):
            print(n)
    elif a[:1] == ["crash"]:
        c = AFCClient(CRASH_COPY)
        data = c.read("/" + a[1])
        open(a[2], "wb").write(data) if len(a) > 2 else sys.stdout.buffer.write(data[:1500])
        print(f"\n{len(data)} bytes", file=sys.stderr)
    else:
        sys.exit(__doc__)
