"""The CDTunnel handshake — CoreDeviceProxy's framing, isolated from how we got the socket.

Wire format (verified live):
    <b"CDTunnel"><u16 be body-length><JSON body>
    request  = {"type": "clientHandshakeRequest", "mtu": 16000}
    response = {"clientParameters": {address, netmask, mtu},
                "serverAddress": "<device IPv6>", "serverRSDPort": <int>}
After the handshake the socket carries RAW IPv6 packets with no CDTunnel wrapper.

The lockdown SSL session is the auth for this door — there is no separate pairing step.
Kept transport-independent on purpose: the same framing runs over a usbmux pipe today and
over a relayed socket later.
"""
import json, struct

from . import TunnelParams

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
    header = _recvn(sock, len(CDTUNNEL_MAGIC) + 2)
    (n,) = struct.unpack(">H", header[-2:])
    return json.loads(_recvn(sock, n))


def handshake(sock, mtu: int = MTU) -> tuple[TunnelParams, dict]:
    """Run the client handshake on an already-open CoreDeviceProxy socket.

    Returns (params, raw_response). After this returns, `sock` is a raw IPv6 pipe.
    """
    _send_frame(sock, {"type": "clientHandshakeRequest", "mtu": mtu})
    resp = _recv_frame(sock)
    cp = resp.get("clientParameters") or {}
    params = TunnelParams(
        our_addr=cp.get("address"),
        dev_addr=resp.get("serverAddress"),
        rsd_port=resp.get("serverRSDPort"),
        mtu=cp.get("mtu") or mtu,
        netmask=cp.get("netmask"),
    )
    return params, resp
