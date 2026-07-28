#!/usr/bin/env python3
"""RemoteXPC connection over HTTP/2 — the channel every CoreDevice service speaks.

RSD (service discovery) and each coredevice.* service (HID, screen, ...) all ride this:
open a socket to the service port THROUGH the tunnel, do the HTTP/2 + XPC init handshake,
then send/receive XPC dicts. Uses xpc.py (self-tested codec).
"""
import socket, struct, uuid

import xpc

HTTP2_MAGIC = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
DATA, HEADERS, RST_STREAM, SETTINGS, GOAWAY, WINDOW_UPDATE = 0x0, 0x1, 0x3, 0x4, 0x7, 0x8
FLAG_ACK, FLAG_END_HEADERS = 0x1, 0x4
ROOT, REPLY = 1, 3
INITIAL_WINDOW = 16 * 1024 * 1024
MESSAGING_PROTOCOL_VERSION = 7
REMOTE_XPC_VERSION_FLAGS = 0x0100000000000006


def _frame(ftype, flags, sid, payload=b""):
    return len(payload).to_bytes(3, "big") + bytes([ftype, flags]) + \
        (sid & 0x7FFFFFFF).to_bytes(4, "big") + payload


class RemoteXPC:
    def __init__(self, host, port, timeout=8):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self._mid = 0
        self._bufs = {}
        self._do_handshake()

    # ---- low-level ----
    def _recvn(self, n):
        b = b""
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError("RemoteXPC closed")
            b += c
        return b

    def _read_frame(self):
        h = self._recvn(9)
        length = int.from_bytes(h[0:3], "big")
        return h[3], h[4], int.from_bytes(h[5:9], "big") & 0x7FFFFFFF, (self._recvn(length) if length else b"")

    def _do_handshake(self):
        self.s.sendall(HTTP2_MAGIC)
        self.s.sendall(_frame(SETTINGS, 0, 0, struct.pack(">HI", 0x3, 100) + struct.pack(">HI", 0x4, INITIAL_WINDOW)))
        self.s.sendall(_frame(WINDOW_UPDATE, 0, 0, struct.pack(">I", INITIAL_WINDOW - 65535)))
        self.s.sendall(_frame(HEADERS, FLAG_END_HEADERS, ROOT))
        self.s.sendall(_frame(DATA, 0, ROOT, xpc.build_wrapper({}, message_id=0)))
        self._mid = 1
        self.s.sendall(_frame(HEADERS, FLAG_END_HEADERS, REPLY))
        self.s.sendall(_frame(DATA, 0, ROOT, xpc.build_wrapper(None, flags=0x0201)))
        self.s.sendall(_frame(DATA, 0, REPLY, xpc.build_wrapper(None, flags=xpc.F_ALWAYS_SET | xpc.F_INIT_HANDSHAKE)))
        while True:
            ftype, flags, _, _ = self._read_frame()
            if ftype == SETTINGS and not (flags & FLAG_ACK):
                self.s.sendall(_frame(SETTINGS, FLAG_ACK, 0)); break
            if ftype in (GOAWAY, RST_STREAM):
                raise ConnectionError("device closed during HTTP/2 setup")

    # ---- XPC request/response ----
    def send_request(self, d, wanting_reply=False):
        flags = xpc.F_ALWAYS_SET | (xpc.F_DATA_PRESENT if d else 0) | (xpc.F_WANTING_REPLY if wanting_reply else 0)
        self.s.sendall(_frame(DATA, 0, ROOT, xpc.build_wrapper(d, message_id=self._mid, flags=flags)))
        self._mid += 1

    def receive(self):
        while True:
            ftype, flags, sid, payload = self._read_frame()
            if ftype in (GOAWAY, RST_STREAM):
                raise ConnectionError("device closed")
            if ftype == SETTINGS and not (flags & FLAG_ACK):
                self.s.sendall(_frame(SETTINGS, FLAG_ACK, 0)); continue
            if ftype != DATA or not payload:
                continue
            buf = self._bufs.get(sid, b"") + payload
            try:
                _f, _m, obj = xpc.parse_wrapper(buf)
                self._bufs[sid] = b""
            except (ValueError, struct.error):
                self._bufs[sid] = buf; continue
            # skip no-payload frames and empty-dict acks (matches pmd3 receive_response);
            # the real reply is the next non-empty object.
            if obj is None or (isinstance(obj, dict) and not obj):
                continue
            return obj

    def send_receive(self, d):
        self.send_request(d, wanting_reply=True)
        return self.receive()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# ---- RSD: service discovery (the device-handshake variant) ----
def enumerate_services(host, port):
    """Connect to RSD and return peer_info (Properties + Services{name:{Port}})."""
    x = RemoteXPC(host, port)
    x.send_request({
        "MessageType": "Handshake",
        "MessagingProtocolVersion": xpc.U64(MESSAGING_PROTOCOL_VERSION),
        "UUID": uuid.uuid4(),
        "Properties": {"RemoteXPCVersionFlags": xpc.U64(REMOTE_XPC_VERSION_FLAGS),
                       "SensitivePropertiesVisible": True},
        "Services": {},
    })
    while True:
        obj = x.receive()
        if isinstance(obj, dict) and ("Services" in obj or "Properties" in obj):
            x.close()
            return obj
