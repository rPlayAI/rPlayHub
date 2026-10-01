#!/usr/bin/env python3
"""Layer 0 — usbmux transport (TCP-over-USB), dependency-light.

Talks to Apple's usbmuxd directly over its unix socket (/var/run/usbmuxd on macOS).
We do NOT displace usbmuxd — we are just another libusbmux client (same wire protocol
as libimobiledevice). This is the transport under lockdown + every device service.

Wire format (plist variant):
  header  = <u32 total_len (incl header)><u32 version=1><u32 msgtype=8 PLIST><u32 tag>
  payload = XML plist with a "MessageType" key

Verified against a real device via probe.py.
"""
import plistlib, socket, struct, sys

from ..errors import MuxError

USBMUXD_SOCKET = ("127.0.0.1", 27015) if sys.platform == "win32" else "/var/run/usbmuxd"

_TYPE_PLIST = 8
_RESULT_OK = 0


class UsbmuxConnection:
    """One connection to usbmuxd. After connect_to_port() it BECOMES the device pipe."""

    def __init__(self, address=USBMUXD_SOCKET):
        if isinstance(address, (tuple, list)):
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.connect(tuple(address))
        elif sys.platform == "win32":
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if ":" in str(address):
                host, port = str(address).split(":")
                self._sock.connect((host, int(port)))
            else:
                self._sock.connect(("127.0.0.1", 27015))
        else:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.connect(address)
        self._tag = 0

    # --- framed plist exchange with usbmuxd itself ---
    def _send(self, payload: dict):
        self._tag += 1
        body = plistlib.dumps(payload)
        hdr = struct.pack("<IIII", len(body) + 16, 1, _TYPE_PLIST, self._tag)
        self._sock.sendall(hdr + body)

    def _recv(self) -> dict:
        hdr = self._recvn(16)
        total, _version, _msgtype, _tag = struct.unpack("<IIII", hdr)
        body = self._recvn(total - 16)
        return plistlib.loads(body)

    def _recvn(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise MuxError("usbmuxd closed the connection")
            buf += chunk
        return buf

    def _request(self, payload: dict) -> dict:
        payload = {"ClientVersionString": "coredevice-host", "ProgName": "coredevice-host", **payload}
        self._send(payload)
        return self._recv()

    # --- API ---
    def list_devices(self) -> list[dict]:
        reply = self._request({"MessageType": "ListDevices"})
        return [d["Properties"] for d in reply.get("DeviceList", [])]

    def read_pair_record(self, udid: str) -> dict:
        """Fetch the stored lockdown pair record from usbmuxd (no root needed)."""
        reply = self._request({"MessageType": "ReadPairRecord", "PairRecordID": udid})
        data = reply.get("PairRecordData")
        if not data:
            raise MuxError(f"no pair record for {udid} (Number={reply.get('Number')})")
        return plistlib.loads(data)

    def connect_to_port(self, device_id: int, port: int) -> socket.socket:
        """Open a pipe to device:port. On success THIS socket is the raw device stream."""
        # usbmuxd wants the TCP port in network byte order inside the plist.
        swapped = ((port << 8) & 0xFF00) | (port >> 8)
        reply = self._request({"MessageType": "Connect", "DeviceID": device_id, "PortNumber": swapped})
        num = reply.get("Number", -1)
        if num != _RESULT_OK:
            raise MuxError(f"Connect to port {port} failed: Number={num} ({reply})")
        return self._sock

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


def list_devices(address=USBMUXD_SOCKET) -> list[dict]:
    c = UsbmuxConnection(address)
    try:
        return c.list_devices()
    finally:
        c.close()


def read_pair_record(udid: str, address=USBMUXD_SOCKET) -> dict:
    c = UsbmuxConnection(address)
    try:
        return c.read_pair_record(udid)
    finally:
        c.close()


def connect(udid: str | None, port: int, address=USBMUXD_SOCKET) -> socket.socket:
    """Return a raw socket to udid:port (first device if udid is None). Caller owns the socket."""
    c = UsbmuxConnection(address)
    devs = c.list_devices()
    if not devs:
        c.close()
        raise MuxError("no devices attached")
    dev = next((d for d in devs if d.get("SerialNumber") == udid), None) if udid else devs[0]
    if dev is None:
        c.close()
        raise MuxError(f"device {udid} not found; attached: {[d.get('SerialNumber') for d in devs]}")
    return c.connect_to_port(dev["DeviceID"], port)


if __name__ == "__main__":
    for d in list_devices():
        print(d.get("DeviceID"), d.get("SerialNumber"), d.get("ConnectionType"),
              d.get("ProductID"), file=sys.stderr)
        print(d)
