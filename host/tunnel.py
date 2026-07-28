#!/usr/bin/env python3
"""Layer 2 — CoreDevice tunnel handshake over com.apple.internal.devicecompute.CoreDeviceProxy.

Wire format (verified): CoreDeviceProxy tunnel packets are
  <b"CDTunnel"><u16 be body-length><JSON body>
The lockdown SSL session is the auth — no separate pairing needed for this door.
  request  = {"type": "clientHandshakeRequest", "mtu": 16000}
  response = {"clientParameters": {address, netmask, mtu}, "serverAddress": "<device IPv6>",
              "serverRSDPort": <int>}
After the handshake, the socket carries RAW IPv6 packets (no CDTunnel wrapper).
Getting serverAddress + serverRSDPort back == Layer 2 verified (the streaming substrate is up).

Run: python3 tunnel.py [udid]
"""
import json, struct, sys

import lockdown
import usbmux

SERVICE = "com.apple.internal.devicecompute.CoreDeviceProxy"
MTU = 16000
CDTUNNEL_MAGIC = b"CDTunnel"


def _send_frame(sock, obj):
    body = json.dumps(obj).encode()
    sock.sendall(CDTUNNEL_MAGIC + struct.pack(">H", len(body)) + body)


def _recvn(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"device closed after {len(buf)}/{n} bytes")
        buf += chunk
    return buf


def _recv_frame(sock):
    header = _recvn(sock, len(CDTUNNEL_MAGIC) + 2)      # magic + u16 length
    (n,) = struct.unpack(">H", header[-2:])
    return json.loads(_recvn(sock, n)), n


def establish_tunnel(udid: str | None = None):
    """Bring up the CoreDevice tunnel. Returns (service_socket, handshake_response).
    The socket then carries raw IPv6 packets."""
    if udid is None:
        udid = usbmux.list_devices()[0]["SerialNumber"]
    pr = usbmux.read_pair_record(udid)
    lc = lockdown.LockdownClient(udid)
    lc.start_session(pr)
    svc = lc.start_service(SERVICE)
    sock = lockdown.connect_service(udid, svc["Port"], pr, svc["EnableServiceSSL"])
    lc.close()
    _send_frame(sock, {"type": "clientHandshakeRequest", "mtu": MTU})
    resp, _ = _recv_frame(sock)
    return sock, resp


def main():
    udid = sys.argv[1] if len(sys.argv) > 1 else usbmux.list_devices()[0]["SerialNumber"]
    pr = usbmux.read_pair_record(udid)

    lc = lockdown.LockdownClient(udid)
    lc.start_session(pr)
    svc = lc.start_service(SERVICE)
    print(f"CoreDeviceProxy: port={svc['Port']} ssl={svc['EnableServiceSSL']}")

    sock = lockdown.connect_service(udid, svc["Port"], pr, svc["EnableServiceSSL"])
    lc.close()

    req = {"type": "clientHandshakeRequest", "mtu": MTU}
    print(f"-> {req}")
    _send_frame(sock, req)
    try:
        resp, n = _recv_frame(sock)
        print(f"<- ({n} bytes) {resp}")
        cp = resp.get("clientParameters", {})
        addr = resp.get("serverAddress") or cp.get("address")
        rsd = resp.get("serverRSDPort")
        if addr or rsd:
            print(f"\nLAYER 2 VERIFIED: device tunnel address={addr} serverRSDPort={rsd} "
                  f"clientParams={cp}")
    except (EOFError, ValueError, struct.error) as e:
        print(f"!! {e}")
        # Dump whatever raw bytes are sitting on the socket to learn the real framing.
        sock.settimeout(1.5)
        try:
            raw = sock.recv(256)
            print("raw peek:", raw.hex())
        except OSError:
            print("(no raw bytes readable — device likely rejected the request framing)")
    finally:
        sock.close()


if __name__ == "__main__":
    main()
