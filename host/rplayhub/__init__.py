"""rplayhub — the Python experiment harness for the rplay-hub CoreDevice host.

Status: PARTIAL REFACTOR. The shipping implementation is Swift + C; this package exists to
prove protocol behaviour against a real phone before it is committed to in those languages.

What is here, in dependency order:
    errors            one exception hierarchy
    wire/             frozen codecs (XPC objects, RemoteXPC over HTTP/2, CBOR)
    transport/        the bottom seam — usbmux (built), RemotePairing + relay (planned)
    net/              tun device (macOS verified, Linux unverified) + the packet pump
    rsd               service discovery and the cached ServiceCatalog

What is NOT here yet: the service broker, the screen/HID service wrappers on top of the
broker, the reconnecting session, and the multi-device server. The verified originals for
screen/HID/screenshot still live as the flat modules in host/ (screen.py, hid.py,
coredevice.py) and still run via `sudo python3 host/tunnel_up.py`.
"""

__all__ = ["errors", "wire", "transport", "net", "rsd"]
