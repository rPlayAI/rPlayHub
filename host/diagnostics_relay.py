#!/usr/bin/env python3
"""diagnostics_relay over the CoreDevice tunnel — Restart / Shutdown / Sleep / queries.

The service is `com.apple.mobile.diagnostics_relay.shim.remote`: a CLASSIC lockdown-style
service (u32-be-length-prefixed plists, no TLS of its own) reached by opening a plain TCP
connection to its RSD-assigned port through the tunnel. Any local process may do this while
cdhost holds the tunnel — same property DirectStream.swift relies on; root is not needed.

Protocol reference: libimobiledevice's diagnostics_relay. Requests are {"Request": name};
replies carry {"Status": "Success"|"Failure"|...} plus payloads for the query requests.

Actions and what they do to a real phone:
    query        DiagnosticsQuery   — benign, proves the channel
    gestalt      MobileGestalt      — benign device-property read
    ioregistry   IORegistry         — benign hardware-tree read  (entry optional)
    sleep        Sleep              — locks the screen (recoverable)
    restart      Restart            — reboots the phone; tunnel drops, comes back
    shutdown     Shutdown           — powers OFF the phone; needs manual power-on

Usage:
    python3 host/diagnostics_relay.py <action> [args...]     # daemon must be running
"""
import json
import plistlib
import socket
import struct
import sys

SERVICE = "com.apple.mobile.diagnostics_relay.shim.remote"
API_PORT = ("127.0.0.1", 9876)


def rpc(method, params=None):
    s = socket.create_connection(API_PORT, timeout=5)
    req = {"id": 1, "method": method}
    if params:
        req["params"] = params
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode())


def open_service():
    """Find the relay's per-session port through the live tunnel, connect, check in."""
    info = rpc("tunnel_info").get("result", {})
    addr, rsd_port = info.get("device_addr"), info.get("rsd_port")
    if not addr or not rsd_port:
        sys.exit(f"daemon has no live tunnel: {info}")

    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    import rsd
    peer = rsd.connect(addr, rsd_port)
    port = rsd.service_port(peer, SERVICE)
    if not port:
        sys.exit(f"{SERVICE} not advertised (iOS too old?)")

    sock = socket.create_connection((addr, port), timeout=15)
    rsd_checkin(sock)
    return sock


def rsd_checkin(sock):
    """RSDCheckin -> ack, then StartService -> ok. Required BEFORE any service protocol;
    without it the device accepts the connection and then closes it on the first real
    request -- which reads exactly like 'the service ignored us' if you skip it."""
    reply = request(sock, {"Label": "rplay-hub", "ProtocolVersion": "2",
                           "Request": "RSDCheckin"})
    if reply.get("Request") != "RSDCheckin":
        sys.exit(f"unexpected RSDCheckin reply: {reply}")
    reply = _recv_plist(sock)
    if reply.get("Request") != "StartService":
        sys.exit(f"expected StartService, got: {reply}")
    if reply.get("Error"):
        sys.exit(f"StartService refused: {reply['Error']}")


def request(sock, payload):
    body = plistlib.dumps(payload)                      # XML plist, u32-be length prefix
    sock.sendall(struct.pack(">I", len(body)) + body)
    return _recv_plist(sock)


def _recv_plist(sock):
    (n,) = struct.unpack(">I", _recvn(sock, 4))
    return plistlib.loads(_recvn(sock, n))


def _recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"peer closed after {len(buf)}/{n} bytes")
        buf += chunk
    return buf


ACTIONS = {
    "query":     lambda args: {"Request": "DiagnosticsQuery"},
    "gestalt":   lambda args: {"Request": "MobileGestalt", "MobileGestaltKeys": args},
    "ioregistry": lambda args: {"Request": "IORegistry", **({"Entry": args[0]} if args else {})},
    "sleep":     lambda args: {"Request": "Sleep"},
    "restart":   lambda args: {"Request": "Restart"},
    "shutdown":  lambda args: {"Request": "Shutdown"},
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ACTIONS:
        sys.exit("usage: python3 diagnostics_relay.py "
                 + "|".join(ACTIONS) + " [args...]")
    action, args = sys.argv[1], sys.argv[2:]
    sock = open_service()
    try:
        reply = request(sock, ACTIONS[action](args))
        print(json.dumps(reply, indent=1, default=str)[:4000])
    finally:
        sock.close()


if __name__ == "__main__":
    main()
